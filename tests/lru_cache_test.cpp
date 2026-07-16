// Plain assert-based tests — no framework dependency for phase 1.
// Each test gets a fresh cache and a fake clock it can advance by hand,
// so TTL tests are deterministic and never sleep.

#include "cache/lru_cache.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

// Hand-advanced clock. Captured by reference into the cache's ClockFn.
struct FakeClock {
    int64_t now_ms = 0;
    meridian::ClockFn fn() {
        return [this] { return now_ms; };
    }
};

void test_put_get_basic() {
    meridian::LruCache cache(4);
    cache.put("player:1", "mmr=3200");
    cache.put("player:2", "mmr=2100");

    assert(cache.get("player:1") == "mmr=3200");
    assert(cache.get("player:2") == "mmr=2100");
    assert(!cache.get("player:3").has_value());
    assert(cache.size() == 2);
}

void test_overwrite_updates_value() {
    meridian::LruCache cache(4);
    cache.put("player:1", "mmr=3200");
    cache.put("player:1", "mmr=3225");

    assert(cache.get("player:1") == "mmr=3225");
    assert(cache.size() == 1);
}

void test_eviction_is_lru_order() {
    meridian::LruCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    cache.put("d", "4");  // capacity 3: "a" is LRU, must go

    assert(!cache.get("a").has_value());
    assert(cache.get("b") == "2");
    assert(cache.get("c") == "3");
    assert(cache.get("d") == "4");
    assert(cache.size() == 3);
}

void test_get_refreshes_recency() {
    meridian::LruCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    // Touch "a" so it becomes MRU; "b" is now the eviction victim.
    assert(cache.get("a").has_value());
    cache.put("d", "4");

    assert(cache.get("a") == "1");
    assert(!cache.get("b").has_value());
    assert(cache.get("c") == "3");
    assert(cache.get("d") == "4");
}

void test_overwrite_refreshes_recency() {
    meridian::LruCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    cache.put("a", "1v2");  // overwrite makes "a" MRU; "b" becomes victim
    cache.put("d", "4");

    assert(cache.get("a") == "1v2");
    assert(!cache.get("b").has_value());
}

void test_ttl_expiry() {
    FakeClock clock;
    meridian::LruCache cache(4, clock.fn());

    cache.put("session:1", "in-match", /*ttl_ms=*/1000);
    cache.put("session:2", "queued");  // no TTL, never expires

    clock.now_ms = 999;
    assert(cache.get("session:1") == "in-match");

    clock.now_ms = 1000;  // expiry boundary is inclusive: now >= deadline
    assert(!cache.get("session:1").has_value());
    assert(cache.get("session:2") == "queued");

    // The expired entry was collected on access, freeing its slot.
    assert(cache.size() == 1);
}

void test_ttl_refreshed_by_overwrite() {
    FakeClock clock;
    meridian::LruCache cache(4, clock.fn());

    cache.put("session:1", "in-match", 1000);
    clock.now_ms = 900;
    cache.put("session:1", "in-match", 1000);  // fresh 1000ms lease

    clock.now_ms = 1500;
    assert(cache.get("session:1") == "in-match");
    clock.now_ms = 1900;
    assert(!cache.get("session:1").has_value());
}

void test_erase() {
    meridian::LruCache cache(4);
    cache.put("a", "1");

    assert(cache.erase("a"));
    assert(!cache.erase("a"));
    assert(!cache.get("a").has_value());
    assert(cache.size() == 0);
}

void test_contains_respects_ttl() {
    FakeClock clock;
    meridian::LruCache cache(4, clock.fn());

    cache.put("a", "1", 500);
    assert(cache.contains("a"));

    clock.now_ms = 500;
    assert(!cache.contains("a"));
}

void test_capacity_one() {
    // Degenerate list case: head == tail for every operation.
    meridian::LruCache cache(1);
    cache.put("a", "1");
    cache.put("b", "2");

    assert(!cache.get("a").has_value());
    assert(cache.get("b") == "2");
    assert(cache.size() == 1);
}

void test_expired_entry_evicted_naturally() {
    // An expired entry that is never read again should still fall out of
    // the cache through normal LRU eviction, not leak forever.
    FakeClock clock;
    meridian::LruCache cache(2, clock.fn());

    cache.put("stale", "x", 10);
    clock.now_ms = 100;  // "stale" expired, but we never touch it
    cache.put("b", "2");
    cache.put("c", "3");  // full: "stale" is LRU and gets evicted

    assert(cache.size() == 2);
    assert(cache.get("b") == "2");
    assert(cache.get("c") == "3");
}

}  // namespace

int main() {
    test_put_get_basic();
    test_overwrite_updates_value();
    test_eviction_is_lru_order();
    test_get_refreshes_recency();
    test_overwrite_refreshes_recency();
    test_ttl_expiry();
    test_ttl_refreshed_by_overwrite();
    test_erase();
    test_contains_respects_ttl();
    test_capacity_one();
    test_expired_entry_evicted_naturally();

    std::cout << "all lru_cache tests passed\n";
    return 0;
}
