#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cache/kv_store.h"
#include "cache/lru_cache.h"

namespace meridian {

// Thread-safe cache: N independent LruCache "stripes", each guarded by its
// own mutex. A key always maps to the same stripe, so two threads only
// contend when they touch keys in the same stripe (~1/N of the time under
// a spread-out workload).
//
// Two consequences worth knowing:
//  - LRU order is per-stripe, not global. Eviction picks the least-recent
//    entry *within the victim key's stripe*, which is an approximation of
//    true LRU. This is the standard trade (memcached does the same); exact
//    global LRU would need a single recency list and we'd be back to one
//    lock serializing everything.
//  - Capacity is split evenly across stripes, so a pathological workload
//    that hammers one stripe can evict from it while others sit half-empty.
//    The hash mixing below makes that unlikely for real key sets.
class StripedCache : public KvStore {
public:
    // total_capacity is divided across stripes (rounded up, so the true
    // ceiling can exceed total_capacity by up to num_stripes-1 entries).
    StripedCache(std::size_t total_capacity, std::size_t num_stripes = 16,
                 ClockFn clock = steady_now_ms);

    StripedCache(const StripedCache&) = delete;
    StripedCache& operator=(const StripedCache&) = delete;

    // ttl_ms default lives here, not on KvStore — see the note there.
    void put(const std::string& key, const std::string& value,
             int64_t ttl_ms = 0) override;
    std::optional<std::string> get(const std::string& key) override;
    bool erase(const std::string& key) override;
    bool contains(const std::string& key) const override;

    // Sums stripe sizes, locking one stripe at a time — a consistent
    // snapshot of each stripe but not of the whole cache. Fine for stats,
    // not for invariant checks while writers are running.
    std::size_t size() const override;

    std::size_t stripe_count() const { return stripes_.size(); }

private:
    // A stripe owns its lock and its shard of the keyspace. alignas keeps
    // each stripe's mutex on its own cache line so cores hammering
    // different stripes don't false-share.
    struct alignas(64) Stripe {
        Stripe(std::size_t cap, ClockFn clock)
            : cache(cap, std::move(clock)) {}
        mutable std::mutex mu;
        LruCache cache;
    };

    std::size_t stripe_for(const std::string& key) const;

    // unique_ptr because Stripe holds a mutex and an LruCache, neither
    // movable, and vector needs movable elements.
    std::vector<std::unique_ptr<Stripe>> stripes_;
};

}  // namespace meridian
