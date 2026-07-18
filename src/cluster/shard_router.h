#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/kv_store.h"
#include "cache/striped_cache.h"
#include "cluster/hash_ring.h"

namespace meridian {

// Routes every key to one of several region shards via the hash ring;
// each shard is its own StripedCache. Implements KvStore, so the protocol
// layer can't tell it apart from a single cache — sharding is invisible
// to clients, which is the point.
//
// Honest scope note (also a good interview answer): in-process shards
// don't buy performance — StripedCache already scales across cores. What
// this layer buys is the *routing architecture*: once replication lands
// (phase 6), a shard becomes something that can live on another machine,
// die, and rejoin, and the ring is what keeps key movement minimal when
// that happens. Real games route to regions by player geography, not by
// hash; consistent hashing is how you shard *within* a region's fleet.
// Here the region names are stand-ins for nodes.
class ShardRouter : public KvStore {
public:
    // capacity_per_shard is per shard, not total.
    ShardRouter(const std::vector<std::string>& shard_names,
                std::size_t capacity_per_shard,
                std::size_t stripes_per_shard = 16,
                ClockFn clock = steady_now_ms);

    void put(const std::string& key, const std::string& value,
             int64_t ttl_ms = 0) override;
    std::optional<std::string> get(const std::string& key) override;
    bool erase(const std::string& key) override;
    bool contains(const std::string& key) const override;
    std::size_t size() const override;  // sum over shards

    // Which shard owns this key (the SHARD protocol command; also handy
    // in tests).
    const std::string& shard_for(const std::string& key) const;

    std::size_t shard_count() const { return shards_.size(); }

private:
    StripedCache& shard(const std::string& key);
    const StripedCache& shard(const std::string& key) const;

    HashRing ring_;
    std::unordered_map<std::string, std::unique_ptr<StripedCache>> shards_;
};

}  // namespace meridian
