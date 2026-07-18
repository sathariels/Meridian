#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meridian {

// Consistent hash ring with virtual nodes.
//
// The problem it solves: with naive `hash(key) % N` sharding, changing N
// remaps almost every key (going 3 -> 4 shards moves ~75% of them) — a
// cluster-wide cache-miss storm exactly when a node just died. On a
// consistent hash ring, membership changes only remap the keys the
// affected node actually owned (~1/N): keys and nodes both hash onto a
// circle of 2^64 points, each key belongs to the first node point at or
// after it clockwise, so removing a node only reassigns keys whose next
// point was that node's.
//
// Virtual nodes: each real node is placed on the ring many times. With a
// single point per node, the arc sizes — and thus the load split — are
// wildly uneven (it's a random cut of the circle). Many points per node
// average the arcs out, and their count doubles as a capacity weight if
// nodes are ever heterogeneous.
//
// Not thread-safe: in this system the ring is built at startup and read
// from the event-loop thread; membership changes are rare and explicit.
class HashRing {
public:
    explicit HashRing(std::size_t vnodes_per_node = 128);

    void add_node(const std::string& name);
    void remove_node(const std::string& name);

    // Which node owns this key. Precondition: at least one node.
    const std::string& lookup(const std::string& key) const;

    std::size_t node_count() const { return node_count_; }
    bool empty() const { return ring_.empty(); }

private:
    struct VNode {
        uint64_t point;
        std::string node;
    };

    std::size_t vnodes_per_node_;
    std::size_t node_count_ = 0;
    std::vector<VNode> ring_;  // sorted by point; rebuilt on membership change
};

}  // namespace meridian
