#include "server/command_handler.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <vector>

#include "cluster/shard_router.h"
#include "leaderboard/leaderboard.h"
#include "net/tcp_server.h"
#include "replication/wal.h"

namespace meridian {

namespace {

// Pops the next space-delimited token off the front of sv. string_view
// slicing means no allocation until we actually keep something.
std::string_view next_token(std::string_view& sv) {
    std::size_t start = sv.find_first_not_of(' ');
    if (start == std::string_view::npos) {
        sv = {};
        return {};
    }
    std::size_t end = sv.find(' ', start);
    std::string_view token = sv.substr(start, end - start);
    sv = (end == std::string_view::npos) ? std::string_view{}
                                         : sv.substr(end + 1);
    return token;
}

// Signed integer parse for scores (a rating can drop below zero in some
// systems; TTLs stay non-negative and use parse_ttl).
bool parse_int64(std::string_view token, int64_t& out) {
    bool negative = false;
    if (!token.empty() && token.front() == '-') {
        negative = true;
        token.remove_prefix(1);
    }
    if (token.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool parse_ttl(std::string_view token, int64_t& out) {
    if (token.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;  // negative or garbage TTLs are protocol errors
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

std::string handle_command(KvStore& cache, const std::string& line) {
    std::string_view rest = line;
    std::string_view cmd = next_token(rest);

    if (cmd.empty()) {
        return "ERR empty command";
    }

    if (cmd == "PING") {
        return "PONG";
    }

    if (cmd == "GET") {
        std::string_view key = next_token(rest);
        if (key.empty() || !rest.empty()) {
            return "ERR usage: GET <key>";
        }
        auto value = cache.get(std::string(key));
        return value.has_value() ? "VALUE " + *value : "NOT_FOUND";
    }

    if (cmd == "SET") {
        std::string_view key = next_token(rest);
        std::string_view ttl_token = next_token(rest);
        int64_t ttl_ms = 0;
        if (key.empty() || !parse_ttl(ttl_token, ttl_ms)) {
            return "ERR usage: SET <key> <ttl_ms> <value>";
        }
        // `rest` is everything after the ttl — the value, spaces and all.
        cache.put(std::string(key), std::string(rest), ttl_ms);
        return "OK";
    }

    if (cmd == "DEL") {
        std::string_view key = next_token(rest);
        if (key.empty() || !rest.empty()) {
            return "ERR usage: DEL <key>";
        }
        return cache.erase(std::string(key)) ? "DELETED" : "NOT_FOUND";
    }

    return "ERR unknown command '" + std::string(cmd) + "'";
}

std::string handle_command(ShardRouter& router, const std::string& line) {
    std::string_view rest = line;
    std::string_view cmd = next_token(rest);

    if (cmd == "SHARD") {
        std::string_view key = next_token(rest);
        if (key.empty() || !rest.empty()) {
            return "ERR usage: SHARD <key>";
        }
        return "SHARD " + router.shard_for(std::string(key));
    }

    // Everything else is topology-agnostic; delegate to the KvStore
    // version so the two overloads can never drift apart.
    return handle_command(static_cast<KvStore&>(router), line);
}

std::string handle_command(ServerContext& ctx, const std::string& line) {
    std::string_view rest = line;
    std::string_view cmd = next_token(rest);

    if (cmd == "SCORE") {
        std::string_view player = next_token(rest);
        std::string_view score_token = next_token(rest);
        int64_t score = 0;
        if (player.empty() || !parse_int64(score_token, score) ||
            !rest.empty()) {
            return "ERR usage: SCORE <player> <score>";
        }
        ctx.leaderboard.update(std::string(player), score);
        return "OK";
    }

    if (cmd == "RANK") {
        std::string_view player = next_token(rest);
        if (player.empty() || !rest.empty()) {
            return "ERR usage: RANK <player>";
        }
        std::string key(player);
        auto r = ctx.leaderboard.rank(key);
        if (!r.has_value()) {
            return "NOT_FOUND";
        }
        // rank() found the player, so score() can't miss.
        return "RANK " + std::to_string(*r) + " " +
               std::to_string(*ctx.leaderboard.score(key));
    }

    if (cmd == "TOP") {
        std::string_view n_token = next_token(rest);
        int64_t n = 0;
        if (!parse_int64(n_token, n) || n < 0 || !rest.empty()) {
            return "ERR usage: TOP <n>";
        }
        std::string reply = "TOP";
        for (const auto& entry :
             ctx.leaderboard.top(static_cast<std::size_t>(n))) {
            reply += " " + entry.player + ":" + std::to_string(entry.score);
        }
        return reply;
    }

    if (cmd == "UNRANK") {
        std::string_view player = next_token(rest);
        if (player.empty() || !rest.empty()) {
            return "ERR usage: UNRANK <player>";
        }
        return ctx.leaderboard.remove(std::string(player)) ? "REMOVED"
                                                           : "NOT_FOUND";
    }

    // Not a leaderboard command: hand off to the router overload (which
    // itself hands off KV commands further down).
    return handle_command(ctx.router, line);
}

namespace {

// The commands that change state — exactly the set that must be logged
// and replicated. Reads never touch the WAL.
bool is_mutation(std::string_view cmd) {
    return cmd == "SET" || cmd == "DEL" || cmd == "SCORE" ||
           cmd == "UNRANK";
}

bool is_error(const std::string& response) {
    return response.rfind("ERR", 0) == 0;
}

}  // namespace

std::string handle_client_command(ServerContext& ctx, int client_id,
                                  const std::string& line) {
    std::string_view rest = line;
    std::string_view cmd = next_token(rest);

    if (cmd == "SYNC") {
        if (ctx.is_follower) {
            return "ERR not a leader";  // no chained replication
        }
        ctx.replicas.insert(client_id);
        // Ship all history first; the live stream continues from there.
        // Push order guarantees the replica sees history before any
        // mutation that happens after this moment.
        if (ctx.server != nullptr && ctx.wal != nullptr) {
            std::string backlog = ctx.wal->read_all();
            if (!backlog.empty()) {
                ctx.server->push(client_id, backlog);
            }
        }
        return "";  // silent: this connection is a one-way stream now
    }

    if (is_mutation(cmd)) {
        if (ctx.is_follower) {
            // Single-writer discipline: all writes go through the leader,
            // or leader and follower state would silently diverge.
            return "ERR read-only replica";
        }
        std::string response = handle_command(ctx, line);
        if (!is_error(response)) {
            // Log-after-apply: an entry only reaches the WAL/replicas if
            // it parsed and executed. (NOT_FOUND deletes still log —
            // replaying them is a no-op, which is harmless.)
            if (ctx.wal != nullptr) {
                ctx.wal->append(line);
            }
            if (ctx.server != nullptr && !ctx.replicas.empty()) {
                // push() can drop a dead replica mid-loop (write error ->
                // disconnect handler -> ctx.replicas.erase), so iterate
                // over a copy.
                std::vector<int> ids(ctx.replicas.begin(),
                                     ctx.replicas.end());
                for (int id : ids) {
                    ctx.server->push(id, line + "\n");
                }
            }
        }
        return response;
    }

    return handle_command(ctx, line);
}

void apply_from_leader(ServerContext& ctx, const std::string& line) {
    std::string response = handle_command(ctx, line);
    // The follower keeps its own WAL so it can crash-recover without a
    // full re-SYNC from the leader.
    if (ctx.wal != nullptr && !is_error(response)) {
        ctx.wal->append(line);
    }
}

std::size_t recover_from_wal(ServerContext& ctx) {
    if (ctx.wal == nullptr) {
        return 0;
    }
    // Replay through the pure-apply overload: recovery must not re-append
    // every line to the very log being read, nor forward it anywhere.
    return ctx.wal->replay(
        [&ctx](const std::string& line) { (void)handle_command(ctx, line); });
}

}  // namespace meridian
