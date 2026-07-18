#pragma once

#include <cstdint>
#include <string_view>

namespace meridian {

// FNV-1a, 64-bit. Chosen over std::hash for ring/stripe placement because
// std::hash's algorithm is implementation-defined — FNV gives the same
// ring layout on every platform, which matters once shard assignment is
// supposed to be stable across builds (and, later, across machines).
inline uint64_t fnv1a_64(std::string_view data) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// splitmix64 finalizer: spreads entropy across all 64 bits. FNV-1a's low
// bits are weak for short keys, and both the stripe index and the ring
// position are derived from modulo/range operations that lean on exactly
// those bits.
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

}  // namespace meridian
