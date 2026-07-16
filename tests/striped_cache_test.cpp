// StripedCache tests. Two layers:
//  1. Single-threaded: the striped wrapper preserves LruCache semantics.
//  2. Multi-threaded hammer: many threads, overlapping keys. Each value
//     deterministically encodes its key, so any torn write, cross-stripe
//     mixup, or use-after-free shows up as a mismatched read (or a TSan
//     report — see the tsan build in CMakeLists.txt).

#include "cache/striped_cache.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string key_for(int i) { return "player:" + std::to_string(i); }
std::string value_for(int i) { return "state-of-player-" + std::to_string(i); }

void test_single_thread_semantics() {
    meridian::StripedCache cache(/*total_capacity=*/64, /*num_stripes=*/4);

    cache.put(key_for(1), value_for(1));
    cache.put(key_for(2), value_for(2));

    assert(cache.get(key_for(1)) == value_for(1));
    assert(cache.get(key_for(2)) == value_for(2));
    assert(!cache.get(key_for(3)).has_value());
    assert(cache.contains(key_for(1)));
    assert(cache.size() == 2);

    assert(cache.erase(key_for(1)));
    assert(!cache.erase(key_for(1)));
    assert(!cache.get(key_for(1)).has_value());
}

void test_ttl_through_striped_api() {
    int64_t now_ms = 0;
    meridian::StripedCache cache(64, 4, [&now_ms] { return now_ms; });

    cache.put("session", "in-match", /*ttl_ms=*/1000);
    assert(cache.get("session") == "in-match");

    now_ms = 1000;
    assert(!cache.get("session").has_value());
}

void test_capacity_is_enforced_per_stripe() {
    // 4 stripes x 4 slots. Insert far more keys than fit; the cache must
    // never exceed its (rounded-up) ceiling.
    meridian::StripedCache cache(16, 4);
    for (int i = 0; i < 1000; ++i) {
        cache.put(key_for(i), value_for(i));
    }
    assert(cache.size() <= 16);

    // Recently inserted keys should mostly still be present. The very last
    // key is always present: its own insert can't have evicted it.
    assert(cache.get(key_for(999)) == value_for(999));
}

void test_concurrent_hammer() {
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 100000;
    constexpr int kKeySpace = 512;  // small: forces overlap + evictions

    meridian::StripedCache cache(/*total_capacity=*/256, /*num_stripes=*/16);
    std::atomic<bool> corruption{false};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) {
            }
            // Cheap per-thread xorshift RNG; std::mt19937 is overkill here.
            uint64_t rng = 0x9e3779b97f4a7c15ULL * (t + 1);
            for (int i = 0; i < kOpsPerThread; ++i) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                int k = static_cast<int>(rng % kKeySpace);
                if (rng % 10 < 3) {
                    cache.put(key_for(k), value_for(k));
                } else if (rng % 100 == 0) {
                    cache.erase(key_for(k));
                } else {
                    auto v = cache.get(key_for(k));
                    // A miss is fine (evicted/never written). A *wrong*
                    // value is a concurrency bug.
                    if (v.has_value() && *v != value_for(k)) {
                        corruption.store(true, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    assert(!corruption.load());
    assert(cache.size() <= 256 + 16);  // capacity ceiling incl. round-up
}

}  // namespace

int main() {
    test_single_thread_semantics();
    test_ttl_through_striped_api();
    test_capacity_is_enforced_per_stripe();
    test_concurrent_hammer();

    std::cout << "all striped_cache tests passed\n";
    return 0;
}
