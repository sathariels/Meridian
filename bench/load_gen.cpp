// Load generator: simulates many concurrent "players" hammering the cache
// and reports throughput + hit rate. Its main job in phase 2 is to make the
// lock-striping win measurable:
//
//   ./load_gen --stripes 1    # global-mutex behavior (baseline)
//   ./load_gen --stripes 16   # striped
//
// Workload shape mimics a game backend: a small set of hot keys (players in
// popular lobbies, top-ranked profiles) absorbs most traffic. 90% of ops
// target the hottest 10% of the keyspace; reads outnumber writes 4:1.

#include "cache/striped_cache.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int threads = 8;
    int stripes = 16;
    int keys = 100000;        // total keyspace
    int ops_per_thread = 500000;
    int capacity = 50000;     // cache holds half the keyspace -> real misses
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc - 1; ++i) {
        std::string flag = argv[i];
        int value = std::atoi(argv[i + 1]);
        if (flag == "--threads") opt.threads = value;
        else if (flag == "--stripes") opt.stripes = value;
        else if (flag == "--keys") opt.keys = value;
        else if (flag == "--ops") opt.ops_per_thread = value;
        else if (flag == "--capacity") opt.capacity = value;
    }
    return opt;
}

// Per-thread counters, summed after join. Padded so two threads' counters
// never share a cache line — otherwise the counters themselves would
// false-share and distort the very contention we're measuring.
struct alignas(64) ThreadStats {
    uint64_t gets = 0;
    uint64_t hits = 0;
    uint64_t puts = 0;
};

void worker(meridian::StripedCache& cache, const Options& opt, int tid,
            std::atomic<bool>& start, ThreadStats& stats) {
    while (!start.load(std::memory_order_acquire)) {
    }

    uint64_t rng = 0x9e3779b97f4a7c15ULL * (tid + 1);
    auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    const int hot_keys = opt.keys / 10;
    for (int i = 0; i < opt.ops_per_thread; ++i) {
        uint64_t r = next();
        // 90% of traffic -> hot 10% of keys.
        int k = (r % 100 < 90) ? static_cast<int>(next() % hot_keys)
                               : static_cast<int>(next() % opt.keys);
        std::string key = "player:" + std::to_string(k);

        if (r % 5 == 0) {  // 20% writes
            cache.put(key, "mmr=3000;region=na;state=in-match");
            ++stats.puts;
        } else {
            ++stats.gets;
            if (cache.get(key).has_value()) {
                ++stats.hits;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);

    meridian::StripedCache cache(opt.capacity, opt.stripes);
    std::atomic<bool> start{false};
    std::vector<ThreadStats> stats(opt.threads);

    std::vector<std::thread> threads;
    threads.reserve(opt.threads);
    for (int t = 0; t < opt.threads; ++t) {
        threads.emplace_back(worker, std::ref(cache), std::cref(opt), t,
                             std::ref(start), std::ref(stats[t]));
    }

    auto t0 = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }
    auto t1 = std::chrono::steady_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    uint64_t gets = 0, hits = 0, puts = 0;
    for (const auto& s : stats) {
        gets += s.gets;
        hits += s.hits;
        puts += s.puts;
    }
    uint64_t total_ops = gets + puts;

    std::cout << "threads=" << opt.threads << " stripes=" << opt.stripes
              << " keys=" << opt.keys << " capacity=" << opt.capacity
              << "\n";
    std::cout << "ops:        " << total_ops << " in " << secs << "s\n";
    std::cout << "throughput: " << static_cast<uint64_t>(total_ops / secs)
              << " ops/sec\n";
    std::cout << "hit rate:   "
              << (gets > 0 ? 100.0 * hits / gets : 0.0) << "%\n";
    return 0;
}
