// Leaderboard tests. The heavyweight one is the randomized cross-check:
// thousands of interleaved insert/re-score/remove ops, then every rank and
// the full top-N compared against a brute-force sorted vector. Skip-list
// span bookkeeping is exactly the kind of pointer arithmetic that can be
// subtly wrong while looking right — the reference model doesn't negotiate.

#include "leaderboard/leaderboard.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cluster/shard_router.h"
#include "server/command_handler.h"

namespace {

std::string player(int i) { return "p" + std::to_string(i); }

void test_basic_ordering() {
    meridian::Leaderboard board;
    board.update("alice", 3200);
    board.update("bob", 2800);
    board.update("cara", 4100);

    auto top = board.top(10);
    assert(top.size() == 3);
    assert(top[0].player == "cara" && top[0].score == 4100);
    assert(top[1].player == "alice" && top[1].score == 3200);
    assert(top[2].player == "bob" && top[2].score == 2800);

    assert(board.rank("cara") == 1u);
    assert(board.rank("alice") == 2u);
    assert(board.rank("bob") == 3u);
    assert(!board.rank("dave").has_value());
    assert(board.score("bob") == 2800);
}

void test_ties_break_by_name() {
    meridian::Leaderboard board;
    board.update("zed", 3000);
    board.update("amy", 3000);
    board.update("mia", 3000);

    auto top = board.top(3);
    // Same score: alphabetical, so the order is stable and predictable.
    assert(top[0].player == "amy");
    assert(top[1].player == "mia");
    assert(top[2].player == "zed");
    assert(board.rank("amy") == 1u);
    assert(board.rank("zed") == 3u);
}

void test_rescore_moves_player() {
    meridian::Leaderboard board;
    board.update("alice", 3000);
    board.update("bob", 2000);
    assert(board.rank("bob") == 2u);

    board.update("bob", 5000);  // bob climbs
    assert(board.rank("bob") == 1u);
    assert(board.rank("alice") == 2u);
    assert(board.size() == 2);  // re-score, not duplicate

    board.update("bob", 5000);  // same score: no-op
    assert(board.rank("bob") == 1u);
    assert(board.size() == 2);
}

void test_remove() {
    meridian::Leaderboard board;
    board.update("alice", 3000);
    board.update("bob", 2000);
    board.update("cara", 1000);

    assert(board.remove("bob"));
    assert(!board.remove("bob"));
    assert(board.size() == 2);
    assert(!board.rank("bob").has_value());
    assert(board.rank("cara") == 2u);  // moved up
}

void test_top_edge_cases() {
    meridian::Leaderboard board;
    assert(board.top(5).empty());
    board.update("solo", 100);
    assert(board.top(0).empty());
    assert(board.top(5).size() == 1);  // n > size: return what exists
}

void test_against_reference_model() {
    meridian::Leaderboard board;
    // Reference: the dumb-but-obviously-correct implementation.
    std::vector<std::pair<int64_t, std::string>> reference;

    auto ref_find = [&](const std::string& p) {
        return std::find_if(reference.begin(), reference.end(),
                            [&](const auto& e) { return e.second == p; });
    };

    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    constexpr int kOps = 20000;
    constexpr int kPlayers = 2000;
    for (int op = 0; op < kOps; ++op) {
        std::string p = player(static_cast<int>(next() % kPlayers));
        uint64_t roll = next() % 10;
        if (roll < 7) {  // 70% insert/re-score
            int64_t score = static_cast<int64_t>(next() % 5000);
            board.update(p, score);
            auto it = ref_find(p);
            if (it != reference.end()) {
                it->first = score;
            } else {
                reference.emplace_back(score, p);
            }
        } else {  // 30% remove
            bool removed = board.remove(p);
            auto it = ref_find(p);
            assert(removed == (it != reference.end()));
            if (it != reference.end()) {
                reference.erase(it);
            }
        }
    }

    // Same ordering rule as the skip list: score desc, name asc.
    std::sort(reference.begin(), reference.end(),
              [](const auto& a, const auto& b) {
                  return a.first != b.first ? a.first > b.first
                                            : a.second < b.second;
              });

    assert(board.size() == reference.size());

    // Every player's rank and score must match the reference exactly.
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto& [score, p] = reference[i];
        assert(board.rank(p) == i + 1);
        assert(board.score(p) == score);
    }

    // And the full ordered walk must be identical.
    auto top = board.top(reference.size());
    assert(top.size() == reference.size());
    for (std::size_t i = 0; i < reference.size(); ++i) {
        assert(top[i].player == reference[i].second);
        assert(top[i].score == reference[i].first);
    }

    std::cout << "  reference cross-check: " << reference.size()
              << " players survived " << kOps << " ops, all ranks exact\n";
}

void test_protocol_commands() {
    meridian::ShardRouter router({"na", "eu", "asia"}, 1024, 4);
    meridian::Leaderboard board;
    meridian::ServerContext ctx{.router = router, .leaderboard = board};

    assert(meridian::handle_command(ctx, "SCORE alice 3200") == "OK");
    assert(meridian::handle_command(ctx, "SCORE bob 2800") == "OK");
    assert(meridian::handle_command(ctx, "SCORE cara 4100") == "OK");

    assert(meridian::handle_command(ctx, "RANK alice") == "RANK 2 3200");
    assert(meridian::handle_command(ctx, "RANK ghost") == "NOT_FOUND");
    assert(meridian::handle_command(ctx, "TOP 2") ==
           "TOP cara:4100 alice:3200");
    assert(meridian::handle_command(ctx, "TOP 99") ==
           "TOP cara:4100 alice:3200 bob:2800");
    assert(meridian::handle_command(ctx, "UNRANK bob") == "REMOVED");
    assert(meridian::handle_command(ctx, "UNRANK bob") == "NOT_FOUND");

    // Negative scores parse (sadly, someone's rating floor).
    assert(meridian::handle_command(ctx, "SCORE tilted -50") == "OK");
    assert(meridian::handle_command(ctx, "RANK tilted") == "RANK 3 -50");

    // Errors.
    assert(meridian::handle_command(ctx, "SCORE alice") ==
           "ERR usage: SCORE <player> <score>");
    assert(meridian::handle_command(ctx, "SCORE alice 12x") ==
           "ERR usage: SCORE <player> <score>");
    assert(meridian::handle_command(ctx, "TOP nope") ==
           "ERR usage: TOP <n>");

    // KV and topology commands still flow through the context handler.
    assert(meridian::handle_command(ctx, "SET k 0 v") == "OK");
    assert(meridian::handle_command(ctx, "GET k") == "VALUE v");
    assert(meridian::handle_command(ctx, "PING") == "PONG");
    std::string shard_reply = meridian::handle_command(ctx, "SHARD k");
    assert(shard_reply.rfind("SHARD ", 0) == 0);
}

}  // namespace

int main() {
    test_basic_ordering();
    test_ties_break_by_name();
    test_rescore_moves_player();
    test_remove();
    test_top_edge_cases();
    test_against_reference_model();
    test_protocol_commands();

    std::cout << "all leaderboard tests passed\n";
    return 0;
}
