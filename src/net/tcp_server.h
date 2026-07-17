#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "net/event_loop.h"

namespace meridian {

// Non-blocking TCP server speaking a newline-delimited text protocol.
// All sockets are owned by the event loop's single thread; the server has
// no locks because nothing here is ever touched by two threads at once.
// (The cache behind the handler is thread-safe anyway, which keeps the
// door open for multiple loop threads later.)
class TcpServer {
public:
    // Called once per complete input line (without the trailing newline);
    // the returned string is sent back followed by '\n'.
    using LineHandler = std::function<std::string(const std::string&)>;

    // port 0 = let the OS pick (tests use this); the real port is
    // available from port() after start().
    TcpServer(EventLoop& loop, std::string host, uint16_t port,
              LineHandler on_line);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Binds and starts accepting. Throws std::runtime_error on failure
    // (port in use, bad host, ...).
    void start();

    uint16_t port() const { return port_; }
    std::size_t connection_count() const { return conns_.size(); }

private:
    struct Connection {
        std::string inbuf;   // bytes received, not yet a complete line
        std::string outbuf;  // bytes the socket wasn't ready to take yet
        // Peer sent FIN (EOF on read). It may have only half-closed —
        // netcat does this after stdin runs dry — so it can still receive
        // our responses. We finish writing outbuf, then close.
        bool peer_half_closed = false;
    };

    void handle_accept();
    void handle_io(int fd, uint32_t events);
    // Writes as much of outbuf as the socket accepts; keeps kWritable
    // interest on only while a partial write is pending. Returns false if
    // the connection died (the Connection& is invalid after that).
    bool flush(int fd, Connection& conn);
    void close_connection(int fd);

    // A client that sends this much without a newline is broken or
    // hostile; we hang up rather than buffer without bound.
    static constexpr std::size_t kMaxLineBytes = 64 * 1024;

    EventLoop& loop_;
    std::string host_;
    uint16_t port_;
    LineHandler on_line_;
    int listen_fd_ = -1;
    std::unordered_map<int, Connection> conns_;
};

}  // namespace meridian
