#pragma once

#include <string>
#include <unordered_set>

#include "cache/kv_store.h"

namespace meridian {

class Leaderboard;
class ShardRouter;

// Parses one protocol line and applies it to the store. Pure function of
// (store, line) with no socket knowledge — deliberately, so the protocol
// is unit-testable without a running server.
//
// Protocol (newline-delimited text, memcached-style):
//   PING                        -> PONG
//   GET <key>                   -> VALUE <value> | NOT_FOUND
//   SET <key> <ttl_ms> <value>  -> OK             (ttl_ms 0 = no expiry;
//                                                  value may contain spaces)
//   DEL <key>                   -> DELETED | NOT_FOUND
//   anything else               -> ERR <reason>
std::string handle_command(KvStore& store, const std::string& line);

// Same protocol plus one observability command that only makes sense
// when there's a topology to observe:
//   SHARD <key>                 -> SHARD <shard-name>
std::string handle_command(ShardRouter& router, const std::string& line);

class TcpServer;
class Wal;

// Everything the server exposes, bundled so the TcpServer handler stays
// one lambda as subsystems accumulate. The replication fields are
// optional: null wal = no persistence, is_follower = reject writes,
// replicas = live SYNC subscribers (leader only).
struct ServerContext {
    ShardRouter& router;
    Leaderboard& leaderboard;
    Wal* wal = nullptr;
    TcpServer* server = nullptr;
    bool is_follower = false;
    std::unordered_set<int> replicas;
};

// Full protocol: KV commands + SHARD + leaderboard commands:
//   SCORE <player> <score>      -> OK              (insert or re-score)
//   RANK <player>               -> RANK <r> <score> | NOT_FOUND
//   TOP <n>                     -> TOP <p1>:<s1> <p2>:<s2> ...
//   UNRANK <player>             -> REMOVED | NOT_FOUND
//
// This overload is the pure "apply to local state" path: no role checks,
// no WAL writes, no replication fan-out. WAL replay and the follower
// stream call it directly — replaying history must never re-log it or
// re-forward it.
std::string handle_command(ServerContext& ctx, const std::string& line);

// The full server entry point: everything above plus
//   SYNC   -> converts the connection into a replication stream (leader
//             pushes the WAL backlog, then every new mutation, to it)
// and the replication side effects — on a leader, successful mutations
// are appended to the WAL and streamed to every replica; on a follower,
// client mutations are rejected with "ERR read-only replica".
std::string handle_client_command(ServerContext& ctx, int client_id,
                                  const std::string& line);

// Follower path for one line arriving from the leader: apply locally and
// append to the follower's own WAL (so a follower crash-recovers too).
void apply_from_leader(ServerContext& ctx, const std::string& line);

// Replay the context's WAL into local state. Returns ops recovered.
std::size_t recover_from_wal(ServerContext& ctx);

}  // namespace meridian
