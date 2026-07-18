// meridian_server: the cache behind a TCP port.
//
//   ./meridian_server --port 7070 --capacity 100000 --stripes 16
//   printf 'SET player:1 0 mmr=3200\nGET player:1\n' | nc 127.0.0.1 7070
//
// Durability + replication (phase 6):
//   ./meridian_server --wal leader.wal                     # leader
//   ./meridian_server --port 7071 --wal follower.wal \
//                     --replica-of 127.0.0.1:7070          # follower

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <memory>

#include "cluster/shard_router.h"
#include "leaderboard/leaderboard.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"
#include "replication/replica_link.h"
#include "replication/wal.h"
#include "server/command_handler.h"

namespace {

// Signal handlers can't capture state, so the loop pointer must be global.
// EventLoop::stop() is async-signal-safe by design (atomic store + pipe
// write), which is what makes this handler legal.
meridian::EventLoop* g_loop = nullptr;

void on_signal(int) {
    if (g_loop != nullptr) {
        g_loop->stop();
    }
}

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 7070;
    std::size_t capacity = 100000;
    std::size_t stripes = 16;
    std::string wal_path;     // empty = no persistence
    std::string leader_host;  // set = run as follower
    uint16_t leader_port = 0;
    bool fsync_each = false;
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--fsync") {
            opt.fsync_each = true;
            continue;
        }
        if (i + 1 >= argc) break;
        if (flag == "--host") opt.host = argv[i + 1];
        else if (flag == "--port")
            opt.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        else if (flag == "--capacity")
            opt.capacity = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        else if (flag == "--stripes")
            opt.stripes = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        else if (flag == "--wal")
            opt.wal_path = argv[i + 1];
        else if (flag == "--replica-of") {
            std::string target = argv[i + 1];
            std::size_t colon = target.find(':');
            if (colon != std::string::npos) {
                opt.leader_host = target.substr(0, colon);
                opt.leader_port = static_cast<uint16_t>(
                    std::atoi(target.c_str() + colon + 1));
            }
        }
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);

    // A client that disconnects mid-response would otherwise kill the
    // whole process with SIGPIPE on our next write() to it. Ignore the
    // signal and let write() return EPIPE instead, which the server
    // handles as a normal connection close.
    std::signal(SIGPIPE, SIG_IGN);

    // Region shards per CLAUDE.md. --capacity is the total budget, split
    // evenly; the ring decides which shard owns which key.
    const std::vector<std::string> shard_names = {"na", "eu", "asia"};
    std::size_t capacity_per_shard =
        (opt.capacity + shard_names.size() - 1) / shard_names.size();
    meridian::ShardRouter router(shard_names, capacity_per_shard,
                                 opt.stripes);
    // Global leaderboard, owned by (and only touched from) the event-loop
    // thread — see the thread-safety note in leaderboard.h.
    meridian::Leaderboard leaderboard;
    meridian::ServerContext ctx{.router = router, .leaderboard = leaderboard};
    ctx.is_follower = !opt.leader_host.empty();

    std::unique_ptr<meridian::Wal> wal;
    if (!opt.wal_path.empty()) {
        wal = std::make_unique<meridian::Wal>(opt.wal_path, opt.fsync_each);
        ctx.wal = wal.get();
        std::size_t recovered = meridian::recover_from_wal(ctx);
        if (recovered > 0) {
            std::cout << "recovered " << recovered << " ops from "
                      << opt.wal_path << "\n";
        }
    }

    auto loop = meridian::EventLoop::create();

    meridian::TcpServer server(
        *loop, opt.host, opt.port,
        [&ctx](int client_id, const std::string& line) {
            return meridian::handle_client_command(ctx, client_id, line);
        });
    ctx.server = &server;
    server.set_disconnect_handler(
        [&ctx](int client_id) { ctx.replicas.erase(client_id); });

    std::unique_ptr<meridian::ReplicaLink> link;
    try {
        server.start();
        if (ctx.is_follower) {
            link = std::make_unique<meridian::ReplicaLink>(
                *loop, opt.leader_host, opt.leader_port,
                [&ctx](const std::string& line) {
                    meridian::apply_from_leader(ctx, line);
                });
            link->start();
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    g_loop = loop.get();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "meridian listening on " << opt.host << ":" << server.port()
              << " (shards=na,eu,asia, capacity=" << opt.capacity
              << ", stripes=" << opt.stripes << " per shard"
              << (ctx.is_follower ? ", follower" : "")
              << (ctx.wal != nullptr ? ", wal=" + opt.wal_path : "")
              << ")\n";

    loop->run();
    std::cout << "shutting down\n";
    return 0;
}
