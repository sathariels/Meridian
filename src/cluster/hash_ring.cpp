#include "cluster/hash_ring.h"

#include <algorithm>
#include <cassert>

#include "common/hash.h"

namespace meridian {

namespace {

uint64_t point_for_key(const std::string& key) {
    return mix64(fnv1a_64(key));
}

uint64_t point_for_vnode(const std::string& node, std::size_t index) {
    // "node#i" gives every virtual node its own stable ring position.
    return point_for_key(node + "#" + std::to_string(index));
}

}  // namespace

HashRing::HashRing(std::size_t vnodes_per_node)
    : vnodes_per_node_(vnodes_per_node) {
    assert(vnodes_per_node_ >= 1);
}

void HashRing::add_node(const std::string& name) {
    for (std::size_t i = 0; i < vnodes_per_node_; ++i) {
        ring_.push_back(VNode{point_for_vnode(name, i), name});
    }
    // Sorting the whole ring on every add is O(V log V), which is fine:
    // membership changes are startup-time or failover-time events, not
    // per-request ones. Lookups are what must be fast.
    // Tie-break by name so a (cosmically unlikely) point collision still
    // orders deterministically on every machine.
    std::sort(ring_.begin(), ring_.end(),
              [](const VNode& a, const VNode& b) {
                  return a.point != b.point ? a.point < b.point
                                            : a.node < b.node;
              });
    ++node_count_;
}

void HashRing::remove_node(const std::string& name) {
    auto removed = std::remove_if(
        ring_.begin(), ring_.end(),
        [&name](const VNode& v) { return v.node == name; });
    if (removed != ring_.end()) {
        ring_.erase(removed, ring_.end());
        --node_count_;
    }
    // Everything else keeps its exact position — that's the entire trick.
}

const std::string& HashRing::lookup(const std::string& key) const {
    assert(!ring_.empty());
    uint64_t point = point_for_key(key);

    // First virtual node at or after the key's point, clockwise...
    auto it = std::lower_bound(
        ring_.begin(), ring_.end(), point,
        [](const VNode& v, uint64_t p) { return v.point < p; });

    // ...wrapping past 2^64-1 back to the start of the circle.
    if (it == ring_.end()) {
        it = ring_.begin();
    }
    return it->node;
}

}  // namespace meridian
