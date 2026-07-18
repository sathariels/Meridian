// Phase 6 tests: WAL durability, crash recovery, and leader-follower
// replication over real sockets. The crash tests genuinely destroy the
// server object and build a fresh one from the log — the only state that
// survives between "incarnations" is the file on disk, exactly like a
// process restart.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "cluster/shard_router.h"
#include "leaderboard/leaderboard.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"
#include "replication/replica_link.h"
#include "replication/wal.h"
#include "server/command_handler.h"

namespace {

std::string temp_wal_path(const std::string& tag) {
    return "/tmp/meridian_test_" + tag + "_" + std::to_string(getpid()) +
           ".wal";
}

// A full in-process server node: cache, leaderboard, WAL, TCP endpoint,
// its own event-loop thread. Optionally a follower of another node.
struct TestNode {
    meridian::ShardRouter router{{"na", "eu", "asia"}, 1024, 4};
    meridian::Leaderboard leaderboard;
    meridian::ServerContext ctx{.router = router, .leaderboard = leaderboard};
    std::unique_ptr<meridian::Wal> wal;
    std::unique_ptr<meridian::EventLoop> loop =
        meridian::EventLoop::create();
    std::unique_ptr<meridian::TcpServer> server;
    std::unique_ptr<meridian::ReplicaLink> link;
    std::thread loop_thread;
    std::size_t recovered = 0;

    TestNode(const std::string& wal_path, uint16_t leader_port = 0) {
        if (!wal_path.empty()) {
            wal = std::make_unique<meridian::Wal>(wal_path);
            ctx.wal = wal.get();
            recovered = meridian::recover_from_wal(ctx);
        }
        ctx.is_follower = (leader_port != 0);

        server = std::make_unique<meridian::TcpServer>(
            *loop, "127.0.0.1", 0,
            [this](int client_id, const std::string& line) {
                return meridian::handle_client_command(ctx, client_id,
                                                       line);
            });
        ctx.server = server.get();
        server->set_disconnect_handler(
            [this](int client_id) { ctx.replicas.erase(client_id); });
        server->start();

        if (leader_port != 0) {
            link = std::make_unique<meridian::ReplicaLink>(
                *loop, "127.0.0.1", leader_port,
                [this](const std::string& line) {
                    meridian::apply_from_leader(ctx, line);
                });
            link->start();
        }
        loop_thread = std::thread([this] { loop->run(); });
    }

    ~TestNode() {
        loop->stop();
        loop_thread.join();
    }

    uint16_t port() const { return server->port(); }
};

int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(rc == 0);
    return fd;
}

void send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        assert(n > 0);
        sent += static_cast<std::size_t>(n);
    }
}

std::string recv_line(int fd) {
    std::string line;
    char ch = 0;
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        assert(n == 1);
        if (ch == '\n') return line;
        line.push_back(ch);
    }
}

std::string request(uint16_t port, const std::string& cmd) {
    int fd = connect_to(port);
    send_all(fd, cmd + "\n");
    std::string reply = recv_line(fd);
    ::close(fd);
    return reply;
}

// Replication is asynchronous: retry a read until it matches or times out.
bool eventually(uint16_t port, const std::string& cmd,
                const std::string& want, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (request(port, cmd) == want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void test_wal_replay_rebuilds_state() {
    std::string path = temp_wal_path("replay");
    std::remove(path.c_str());
    {
        meridian::Wal wal(path);
        wal.append("SET player:1 0 mmr=3200");
        wal.append("SET player:2 0 mmr=2800");
        wal.append("DEL player:2");
        wal.append("SCORE alice 4100");
    }  // destroyed: only the file remains

    meridian::ShardRouter router({"na", "eu", "asia"}, 1024, 4);
    meridian::Leaderboard board;
    meridian::ServerContext ctx{.router = router, .leaderboard = board};
    meridian::Wal wal(path);
    ctx.wal = &wal;
    std::size_t n = meridian::recover_from_wal(ctx);

    assert(n == 4);
    assert(router.get("player:1") == "mmr=3200");
    assert(!router.get("player:2").has_value());  // the DEL replayed too
    assert(board.rank("alice") == 1u);
    std::remove(path.c_str());
}

void test_torn_tail_is_truncated() {
    std::string path = temp_wal_path("torn");
    std::remove(path.c_str());
    {
        meridian::Wal wal(path);
        wal.append("SET good 0 v");
    }
    // Simulate a crash mid-write: valid line, then a torn fragment.
    FILE* f = std::fopen(path.c_str(), "ab");
    std::fputs("SET torn 0 partial-value-with-no-newl", f);
    std::fclose(f);

    // Reopening repairs the tail; replay sees only complete lines.
    meridian::Wal wal(path);
    int count = 0;
    wal.replay([&count](const std::string& line) {
        assert(line == "SET good 0 v");
        ++count;
    });
    assert(count == 1);

    // And a post-crash append must not fuse with torn bytes.
    wal.append("SET after 0 w");
    int lines = 0;
    wal.replay([&lines](const std::string&) { ++lines; });
    assert(lines == 2);
    std::remove(path.c_str());
}

void test_crash_recovery_end_to_end() {
    std::string path = temp_wal_path("crash");
    std::remove(path.c_str());

    {
        TestNode node(path);
        assert(request(node.port(), "SET player:7 0 mmr=5000") == "OK");
        assert(request(node.port(), "SCORE alice 3200") == "OK");
        assert(request(node.port(), "SCORE bob 2800") == "OK");
        assert(request(node.port(), "DEL missing") == "NOT_FOUND");
    }  // "crash": node destroyed, memory gone

    TestNode reborn(path);
    assert(reborn.recovered >= 3);
    assert(request(reborn.port(), "GET player:7") == "VALUE mmr=5000");
    assert(request(reborn.port(), "TOP 2") == "TOP alice:3200 bob:2800");
    assert(request(reborn.port(), "RANK bob") == "RANK 2 2800");
    std::remove(path.c_str());
}

void test_follower_receives_live_stream() {
    TestNode leader("");  // replication works even without a leader WAL
    TestNode follower("", leader.port());

    assert(request(leader.port(), "SET player:1 0 mmr=3000") == "OK");
    assert(request(leader.port(), "SCORE alice 4100") == "OK");

    assert(eventually(follower.port(), "GET player:1", "VALUE mmr=3000"));
    assert(eventually(follower.port(), "RANK alice", "RANK 1 4100"));
}

void test_follower_catches_up_on_backlog() {
    std::string path = temp_wal_path("backlog");
    std::remove(path.c_str());
    TestNode leader(path);

    // History written BEFORE the follower exists.
    assert(request(leader.port(), "SET old:1 0 a") == "OK");
    assert(request(leader.port(), "SCORE veteran 9000") == "OK");

    TestNode follower("", leader.port());
    // SYNC must deliver the backlog...
    assert(eventually(follower.port(), "GET old:1", "VALUE a"));
    assert(eventually(follower.port(), "RANK veteran", "RANK 1 9000"));

    // ...and the live stream continues seamlessly after it.
    assert(request(leader.port(), "SET new:1 0 b") == "OK");
    assert(eventually(follower.port(), "GET new:1", "VALUE b"));
    std::remove(path.c_str());
}

void test_follower_rejects_writes() {
    TestNode leader("");
    TestNode follower("", leader.port());

    assert(request(follower.port(), "SET k 0 v") ==
           "ERR read-only replica");
    assert(request(follower.port(), "SCORE x 1") ==
           "ERR read-only replica");
    assert(request(follower.port(), "PING") == "PONG");  // reads still fine
}

void test_follower_own_wal_survives_its_crash() {
    std::string follower_path = temp_wal_path("fwal");
    std::remove(follower_path.c_str());
    TestNode leader("");

    {
        TestNode follower(follower_path, leader.port());
        assert(request(leader.port(), "SET replicated 0 yes") == "OK");
        assert(eventually(follower.port(), "GET replicated", "VALUE yes"));
    }  // follower crashes

    // Reborn follower recovers from ITS OWN wal — before even contacting
    // the leader. (It also re-SYNCs, which harmlessly re-applies.)
    TestNode reborn(follower_path, leader.port());
    assert(request(reborn.port(), "GET replicated") == "VALUE yes");
    std::remove(follower_path.c_str());
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    test_wal_replay_rebuilds_state();
    test_torn_tail_is_truncated();
    test_crash_recovery_end_to_end();
    test_follower_receives_live_stream();
    test_follower_catches_up_on_backlog();
    test_follower_rejects_writes();
    test_follower_own_wal_survives_its_crash();

    std::cout << "all replication tests passed\n";
    return 0;
}
