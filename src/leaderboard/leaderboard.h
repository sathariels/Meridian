#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace meridian {

// Leaderboard: hand-built skip list ordered by (score desc, player asc),
// paired with a hash map from player -> node. The same pairing Redis uses
// for sorted sets, and for the same reason: the map gives O(1) "where is
// this player", the skip list gives O(log n) ordered operations.
//
// Every forward link carries a *span* — how many level-0 steps it jumps.
// Summing spans along a search path yields a node's position, which is
// what makes rank() O(log n) instead of "walk the whole list counting".
//
// Why a skip list over the obvious alternatives:
//  - sorted vector: top-N is trivial but every score update is O(n)
//    memmove; players update scores constantly, so writes dominate.
//  - balanced BST (std::map): O(log n) too, but rank needs subtree-size
//    augmentation std::map doesn't expose, and we'd be hand-rolling
//    red-black rebalancing to add it. Skip list gives ordered + indexable
//    with ~40 lines of pointer logic and no rotation cases.
//
// NOT thread-safe, on purpose: it's owned by the single event-loop thread
// (single-writer design, same as Redis). If multiple loop threads ever
// land, this gets a lock or per-region boards — decide then, not now.
class Leaderboard {
public:
    struct Entry {
        std::string player;
        int64_t score;
    };

    Leaderboard();
    ~Leaderboard();
    Leaderboard(const Leaderboard&) = delete;
    Leaderboard& operator=(const Leaderboard&) = delete;

    // Insert, or re-score if the player exists (no-op if score unchanged).
    void update(const std::string& player, int64_t score);

    bool remove(const std::string& player);

    // 1-based; rank 1 = best score. nullopt if the player isn't ranked.
    std::optional<std::size_t> rank(const std::string& player) const;

    std::optional<int64_t> score(const std::string& player) const;

    // Best n entries in order; fewer if the board is smaller.
    std::vector<Entry> top(std::size_t n) const;

    std::size_t size() const { return by_player_.size(); }

private:
    // With p = 1/4, 32 levels index ~4^32 entries — the Redis constants.
    static constexpr int kMaxLevel = 32;

    struct Node {
        struct Link {
            Node* next = nullptr;
            std::size_t span = 0;  // level-0 steps this link jumps
        };
        std::string player;
        int64_t score;
        std::vector<Link> links;  // size == this node's level

        Node(std::string p, int64_t s, int level)
            : player(std::move(p)), score(s), links(level) {}
    };

    // Strict ordering: better score first, ties broken by name so equal
    // scores still rank deterministically (and identically on every run).
    static bool precedes(int64_t score_a, const std::string& player_a,
                         int64_t score_b, const std::string& player_b);
    static bool node_precedes(const Node* node, int64_t score,
                              const std::string& player);

    int random_level();
    void insert(const std::string& player, int64_t score);
    void unlink(Node* node);

    Node head_;      // sentinel, kMaxLevel links, holds no entry
    int level_ = 1;  // highest level currently in use
    uint64_t rng_state_ = 0x2545F4914F6CDD1DULL;
    std::unordered_map<std::string, Node*> by_player_;
};

}  // namespace meridian
