// HashRing tests. The interesting ones prove the two properties that are
// consistent hashing's entire reason to exist:
//   1. load spreads roughly evenly across nodes (virtual nodes at work)
//   2. membership changes remap only ~1/N of keys, and every remapped key
//      moves to/from the changed node — never between two survivors
// All assertions are deterministic: the hash is fixed (FNV+mix, not
// std::hash), so if this passes once it passes everywhere, every time.

#include "cluster/hash_ring.h"

#include <cassert>
#include <iostream>
#include <map>
#include <string>

namespace {

std::string key_for(int i) { return "player:" + std::to_string(i); }

void test_lookup_is_stable() {
    meridian::HashRing ring;
    ring.add_node("na");
    ring.add_node("eu");
    ring.add_node("asia");

    for (int i = 0; i < 100; ++i) {
        assert(ring.lookup(key_for(i)) == ring.lookup(key_for(i)));
    }
}

void test_distribution_is_roughly_even() {
    meridian::HashRing ring;
    ring.add_node("na");
    ring.add_node("eu");
    ring.add_node("asia");

    constexpr int kKeys = 30000;
    std::map<std::string, int> counts;
    for (int i = 0; i < kKeys; ++i) {
        ++counts[ring.lookup(key_for(i))];
    }

    assert(counts.size() == 3);
    // Perfect would be 10000 each. With 128 vnodes/node, arc-length
    // variance typically lands within a few percent; the bound here is
    // deliberately loose (±30%) so it only fails on real brokenness, not
    // on hash luck.
    const double expected = kKeys / 3.0;
    for (const auto& [node, count] : counts) {
        assert(count > expected * 0.7);
        assert(count < expected * 1.3);
    }
}

void test_remove_node_remaps_only_its_own_keys() {
    meridian::HashRing ring;
    ring.add_node("na");
    ring.add_node("eu");
    ring.add_node("asia");
    ring.add_node("sa");

    constexpr int kKeys = 20000;
    std::map<std::string, std::string> before;
    for (int i = 0; i < kKeys; ++i) {
        before[key_for(i)] = ring.lookup(key_for(i));
    }

    ring.remove_node("sa");

    int remapped = 0;
    for (int i = 0; i < kKeys; ++i) {
        const std::string& owner_before = before[key_for(i)];
        const std::string& owner_after = ring.lookup(key_for(i));
        if (owner_before == "sa") {
            // Orphaned keys must land on a survivor.
            assert(owner_after != "sa");
            ++remapped;
        } else {
            // THE consistent-hashing guarantee: keys on surviving nodes
            // do not move at all. Exact, not statistical.
            assert(owner_after == owner_before);
        }
    }

    // The removed node owned ~1/4 of the keys; only those moved.
    assert(remapped > kKeys * 0.15);
    assert(remapped < kKeys * 0.35);
}

void test_add_node_steals_only_for_itself() {
    meridian::HashRing ring;
    ring.add_node("na");
    ring.add_node("eu");
    ring.add_node("asia");

    constexpr int kKeys = 20000;
    std::map<std::string, std::string> before;
    for (int i = 0; i < kKeys; ++i) {
        before[key_for(i)] = ring.lookup(key_for(i));
    }

    ring.add_node("sa");

    int remapped = 0;
    for (int i = 0; i < kKeys; ++i) {
        const std::string& owner_after = ring.lookup(key_for(i));
        if (owner_after != before[key_for(i)]) {
            // Mirror-image guarantee: a key that moved can only have
            // moved TO the new node.
            assert(owner_after == "sa");
            ++remapped;
        }
    }

    // The new node takes ~1/4 of the keyspace and nothing else changes.
    assert(remapped > kKeys * 0.15);
    assert(remapped < kKeys * 0.35);

    // Contrast with hash(key) % N sharding, where this test would show
    // ~75% of keys remapped instead of ~25%.
}

void test_single_node_owns_everything() {
    meridian::HashRing ring;
    ring.add_node("solo");
    for (int i = 0; i < 1000; ++i) {
        assert(ring.lookup(key_for(i)) == "solo");
    }
}

}  // namespace

int main() {
    test_lookup_is_stable();
    test_distribution_is_roughly_even();
    test_remove_node_remaps_only_its_own_keys();
    test_add_node_steals_only_for_itself();
    test_single_node_owns_everything();

    std::cout << "all hash_ring tests passed\n";
    return 0;
}
