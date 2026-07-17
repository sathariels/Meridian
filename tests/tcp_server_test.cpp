// End-to-end integration test: real event loop, real TCP sockets on
// loopback, real protocol. The server runs on port 0 (OS-assigned) in a
// background thread; the test plays the role of clients with plain
// blocking sockets.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "cache/striped_cache.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"
#include "server/command_handler.h"

namespace {

struct TestServer {
    meridian::StripedCache cache{1024, 8};
    std::unique_ptr<meridian::EventLoop> loop = meridian::EventLoop::create();
    meridian::TcpServer server;
    std::thread loop_thread;

    TestServer()
        : server(*loop, "127.0.0.1", /*port=*/0,
                 [this](const std::string& line) {
                     return meridian::handle_command(cache, line);
                 }) {
        server.start();  // registers fds before the loop thread exists
        loop_thread = std::thread([this] { loop->run(); });
    }

    ~TestServer() {
        loop->stop();
        loop_thread.join();
    }
};

int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    // A receive timeout turns "test hangs forever" into "test fails with
    // an assert" if the server never responds.
    timeval tv{/*tv_sec=*/2, /*tv_usec=*/0};
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

// Byte-at-a-time is slow but exact: it can't over-read into the next
// response, so back-to-back calls stay aligned with the protocol.
std::string recv_line(int fd) {
    std::string line;
    char ch = 0;
    while (true) {
        ssize_t n = ::read(fd, &ch, 1);
        assert(n == 1);  // 0 = server hung up, -1 = timeout
        if (ch == '\n') {
            return line;
        }
        line.push_back(ch);
    }
}

void test_roundtrip(uint16_t port) {
    int fd = connect_to(port);
    send_all(fd, "PING\n");
    assert(recv_line(fd) == "PONG");

    send_all(fd, "SET player:1 0 mmr=3200 region=na\n");
    assert(recv_line(fd) == "OK");
    send_all(fd, "GET player:1\n");
    assert(recv_line(fd) == "VALUE mmr=3200 region=na");
    send_all(fd, "DEL player:1\n");
    assert(recv_line(fd) == "DELETED");
    send_all(fd, "GET player:1\n");
    assert(recv_line(fd) == "NOT_FOUND");
    ::close(fd);
}

void test_pipelined_commands(uint16_t port) {
    // Three commands in one TCP segment: the server must find all three
    // lines in a single read and answer each in order.
    int fd = connect_to(port);
    send_all(fd, "PING\nSET a 0 1\nGET a\n");
    assert(recv_line(fd) == "PONG");
    assert(recv_line(fd) == "OK");
    assert(recv_line(fd) == "VALUE 1");
    ::close(fd);
}

void test_command_split_across_writes(uint16_t port) {
    // The mirror image of pipelining: one command dribbling in as three
    // segments. The server must buffer until the newline arrives.
    int fd = connect_to(port);
    send_all(fd, "SET frag");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    send_all(fd, "ment 0 pie");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    send_all(fd, "ces\nGET fragment\n");
    assert(recv_line(fd) == "OK");
    assert(recv_line(fd) == "VALUE pieces");
    ::close(fd);
}

void test_two_clients_are_isolated(uint16_t port) {
    int a = connect_to(port);
    int b = connect_to(port);

    // Interleave: a's half-typed command must not bleed into b's stream.
    send_all(a, "SET shared 0 from-a");
    send_all(b, "PING\n");
    assert(recv_line(b) == "PONG");
    send_all(a, "\n");
    assert(recv_line(a) == "OK");

    send_all(b, "GET shared\n");  // but the cache itself IS shared
    assert(recv_line(b) == "VALUE from-a");

    ::close(a);
    ::close(b);
}

void test_ttl_over_the_wire(uint16_t port) {
    // Integration test uses the real clock, so this sleeps for real —
    // kept short. Exact TTL semantics are covered by the fake-clock
    // unit tests; this only proves TTL survives the network plumbing.
    int fd = connect_to(port);
    send_all(fd, "SET ephemeral 50 x\n");
    assert(recv_line(fd) == "OK");
    send_all(fd, "GET ephemeral\n");
    assert(recv_line(fd) == "VALUE x");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    send_all(fd, "GET ephemeral\n");
    assert(recv_line(fd) == "NOT_FOUND");
    ::close(fd);
}

void test_abrupt_disconnect_does_not_kill_server(uint16_t port) {
    int fd = connect_to(port);
    send_all(fd, "SET doomed 0 v\n");
    ::close(fd);  // vanish without reading the response

    // Server must still be alive for the next client.
    int fd2 = connect_to(port);
    send_all(fd2, "GET doomed\n");
    assert(recv_line(fd2) == "VALUE v");
    ::close(fd2);
}

}  // namespace

int main() {
    // The test writes to sockets a test case may have just closed;
    // without this the process dies with SIGPIPE instead of failing
    // an assert. (meridian_server does the same for the same reason.)
    std::signal(SIGPIPE, SIG_IGN);

    TestServer ts;
    uint16_t port = ts.server.port();

    test_roundtrip(port);
    test_pipelined_commands(port);
    test_command_split_across_writes(port);
    test_two_clients_are_isolated(port);
    test_ttl_over_the_wire(port);
    test_abrupt_disconnect_does_not_kill_server(port);

    std::cout << "all tcp_server tests passed\n";
    return 0;
}
