#include "leaderboard/leaderboard.h"

#include <array>
#include <cassert>

namespace meridian {

Leaderboard::Leaderboard() : head_("", 0, kMaxLevel) {}

Leaderboard::~Leaderboard() {
    Node* node = head_.links[0].next;
    while (node != nullptr) {
        Node* next = node->links[0].next;
        delete node;
        node = next;
    }
}

bool Leaderboard::precedes(int64_t score_a, const std::string& player_a,
                           int64_t score_b, const std::string& player_b) {
    if (score_a != score_b) {
        return score_a > score_b;  // higher score ranks first
    }
    return player_a < player_b;
}

bool Leaderboard::node_precedes(const Node* node, int64_t score,
                                const std::string& player) {
    return precedes(node->score, node->player, score, player);
}

int Leaderboard::random_level() {
    // xorshift64; fixed seed makes every run's tower heights identical,
    // which keeps failures reproducible. Randomness here only affects
    // speed, never correctness or ordering.
    auto next = [this] {
        rng_state_ ^= rng_state_ << 13;
        rng_state_ ^= rng_state_ >> 7;
        rng_state_ ^= rng_state_ << 17;
        return rng_state_;
    };
    // P(level >= k+1 | level >= k) = 1/4: each extra level is a coin flip
    // with two heads required. E[pointers/node] ≈ 1.33.
    int level = 1;
    while (level < kMaxLevel && (next() & 0x3) == 0) {
        ++level;
    }
    return level;
}

void Leaderboard::update(const std::string& player, int64_t score) {
    auto it = by_player_.find(player);
    if (it != by_player_.end()) {
        if (it->second->score == score) {
            return;  // no movement possible; skip the churn
        }
        // Re-score = remove + reinsert. Redis special-cases "position
        // didn't change" to update in place; both are O(log n), so we
        // take the version with one code path.
        unlink(it->second);
        delete it->second;
        by_player_.erase(it);
    }
    insert(player, score);
}

bool Leaderboard::remove(const std::string& player) {
    auto it = by_player_.find(player);
    if (it == by_player_.end()) {
        return false;
    }
    unlink(it->second);
    delete it->second;
    by_player_.erase(it);
    return true;
}

void Leaderboard::insert(const std::string& player, int64_t score) {
    // update[i] = rightmost node at level i that precedes the new entry;
    // rank_at[i] = how many level-0 steps from head to update[i].
    // Collecting ranks during the ordinary search is the entire trick
    // that keeps spans maintainable in O(log n).
    std::array<Node*, kMaxLevel> update;
    std::array<std::size_t, kMaxLevel> rank_at;

    Node* x = &head_;
    for (int i = level_ - 1; i >= 0; --i) {
        rank_at[i] = (i == level_ - 1) ? 0 : rank_at[i + 1];
        while (x->links[i].next != nullptr &&
               node_precedes(x->links[i].next, score, player)) {
            rank_at[i] += x->links[i].span;
            x = x->links[i].next;
        }
        update[i] = x;
    }

    int new_level = random_level();
    if (new_level > level_) {
        for (int i = level_; i < new_level; ++i) {
            rank_at[i] = 0;
            update[i] = &head_;
            // Until now this level didn't exist; its head link must span
            // the whole list so the arithmetic below stays consistent.
            head_.links[i].span = by_player_.size();
        }
        level_ = new_level;
    }

    Node* node = new Node(player, score, new_level);
    std::size_t insert_rank = rank_at[0];  // 0-based rank of the new node

    for (int i = 0; i < new_level; ++i) {
        node->links[i].next = update[i]->links[i].next;
        update[i]->links[i].next = node;

        // Split update[i]'s old span at the insertion point:
        // (rank_at[0] - rank_at[i]) level-0 steps lie between update[i]
        // and the new node.
        std::size_t steps_before = insert_rank - rank_at[i];
        node->links[i].span = update[i]->links[i].span - steps_before;
        update[i]->links[i].span = steps_before + 1;
    }

    // Levels above the new node's height: their links now jump over one
    // more node than before.
    for (int i = new_level; i < level_; ++i) {
        update[i]->links[i].span += 1;
    }

    by_player_.emplace(player, node);
}

void Leaderboard::unlink(Node* node) {
    std::array<Node*, kMaxLevel> update;

    Node* x = &head_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->links[i].next != nullptr &&
               node_precedes(x->links[i].next, node->score, node->player)) {
            x = x->links[i].next;
        }
        update[i] = x;
    }

    for (int i = 0; i < level_; ++i) {
        if (update[i]->links[i].next == node) {
            // Absorb the removed node's jump into its predecessor's.
            update[i]->links[i].span += node->links[i].span - 1;
            update[i]->links[i].next = node->links[i].next;
        } else {
            // This level jumps over the node; its jump just got shorter.
            update[i]->links[i].span -= 1;
        }
    }

    while (level_ > 1 && head_.links[level_ - 1].next == nullptr) {
        --level_;
    }
}

std::optional<std::size_t> Leaderboard::rank(
    const std::string& player) const {
    auto it = by_player_.find(player);
    if (it == by_player_.end()) {
        return std::nullopt;
    }
    const Node* target = it->second;

    // Ordinary search, but summing spans as we go: when the walk lands
    // on the target, the sum IS its 1-based rank.
    std::size_t r = 0;
    const Node* x = &head_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (x->links[i].next != nullptr &&
               !node_precedes(target, x->links[i].next->score,
                              x->links[i].next->player)) {
            r += x->links[i].span;
            x = x->links[i].next;
        }
        if (x == target) {
            return r;
        }
    }
    assert(false && "player in map but not reachable in skip list");
    return std::nullopt;
}

std::optional<int64_t> Leaderboard::score(const std::string& player) const {
    auto it = by_player_.find(player);
    if (it == by_player_.end()) {
        return std::nullopt;
    }
    return it->second->score;
}

std::vector<Leaderboard::Entry> Leaderboard::top(std::size_t n) const {
    std::vector<Entry> out;
    out.reserve(n < by_player_.size() ? n : by_player_.size());
    const Node* node = head_.links[0].next;
    while (node != nullptr && out.size() < n) {
        out.push_back(Entry{node->player, node->score});
        node = node->links[0].next;
    }
    return out;
}

}  // namespace meridian
