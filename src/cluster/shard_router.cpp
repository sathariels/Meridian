#include "cluster/shard_router.h"

#include <cassert>

namespace meridian {

ShardRouter::ShardRouter(const std::vector<std::string>& shard_names,
                         std::size_t capacity_per_shard,
                         std::size_t stripes_per_shard, ClockFn clock) {
    assert(!shard_names.empty());
    for (const auto& name : shard_names) {
        ring_.add_node(name);
        shards_.emplace(name,
                        std::make_unique<StripedCache>(
                            capacity_per_shard, stripes_per_shard, clock));
    }
}

void ShardRouter::put(const std::string& key, const std::string& value,
                      int64_t ttl_ms) {
    shard(key).put(key, value, ttl_ms);
}

std::optional<std::string> ShardRouter::get(const std::string& key) {
    return shard(key).get(key);
}

bool ShardRouter::erase(const std::string& key) {
    return shard(key).erase(key);
}

bool ShardRouter::contains(const std::string& key) const {
    return shard(key).contains(key);
}

std::size_t ShardRouter::size() const {
    std::size_t total = 0;
    for (const auto& [name, cache] : shards_) {
        total += cache->size();
    }
    return total;
}

const std::string& ShardRouter::shard_for(const std::string& key) const {
    return ring_.lookup(key);
}

StripedCache& ShardRouter::shard(const std::string& key) {
    return *shards_.at(ring_.lookup(key));
}

const StripedCache& ShardRouter::shard(const std::string& key) const {
    return *shards_.at(ring_.lookup(key));
}

}  // namespace meridian
