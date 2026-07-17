#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace meridian {

namespace {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

TcpServer::TcpServer(EventLoop& loop, std::string host, uint16_t port,
                     LineHandler on_line)
    : loop_(loop),
      host_(std::move(host)),
      port_(port),
      on_line_(std::move(on_line)) {}

TcpServer::~TcpServer() {
    for (const auto& [fd, conn] : conns_) {
        loop_.remove_fd(fd);
        ::close(fd);
    }
    if (listen_fd_ >= 0) {
        loop_.remove_fd(listen_fd_);
        ::close(listen_fd_);
    }
}

void TcpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket() failed");
    }

    // Without SO_REUSEADDR, restarting the server right after a shutdown
    // fails with EADDRINUSE while old connections sit in TIME_WAIT.
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("bad listen address: " + host_);
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        throw std::runtime_error(std::string("bind failed: ") +
                                 std::strerror(errno));
    }
    if (listen(listen_fd_, /*backlog=*/128) != 0) {
        throw std::runtime_error(std::string("listen failed: ") +
                                 std::strerror(errno));
    }
    set_nonblocking(listen_fd_);

    if (port_ == 0) {
        sockaddr_in actual{};
        socklen_t len = sizeof(actual);
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &len);
        port_ = ntohs(actual.sin_port);
    }

    loop_.add_fd(listen_fd_, IoEvent::kReadable,
                 [this](uint32_t) { handle_accept(); });
}

void TcpServer::handle_accept() {
    // Accept until EAGAIN: several clients may have connected behind one
    // readiness notification.
    while (true) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            // EAGAIN = drained. Anything else (e.g. ECONNABORTED, a client
            // that gave up while queued) is per-connection and transient;
            // the listen socket itself is fine either way.
            return;
        }
        set_nonblocking(fd);
        conns_.emplace(fd, Connection{});
        loop_.add_fd(fd, IoEvent::kReadable,
                     [this, fd](uint32_t events) { handle_io(fd, events); });
    }
}

void TcpServer::handle_io(int fd, uint32_t events) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) {
        return;
    }
    Connection& conn = it->second;

    if (events & IoEvent::kReadable) {
        char buf[4096];
        while (true) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn.inbuf.append(buf, static_cast<std::size_t>(n));
                if (conn.inbuf.size() > kMaxLineBytes) {
                    close_connection(fd);
                    return;
                }
                continue;
            }
            if (n == 0) {
                // FIN from the peer — but bytes that arrived before it
                // are still in inbuf, so fall through and execute them.
                conn.peer_half_closed = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // drained everything currently available
            }
            close_connection(fd);
            return;
        }

        // The read may have completed zero, one, or many lines — TCP is a
        // byte stream and owes us nothing about message boundaries.
        std::size_t newline;
        while ((newline = conn.inbuf.find('\n')) != std::string::npos) {
            std::string line = conn.inbuf.substr(0, newline);
            conn.inbuf.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();  // be telnet-friendly: accept \r\n
            }
            conn.outbuf += on_line_(line);
            conn.outbuf += '\n';
        }
    }

    if (!flush(fd, conn)) {
        return;  // connection closed inside flush
    }

    // Half-closed and nothing left to send: our side's turn to hang up.
    // (If outbuf still has bytes, the next writable event re-enters
    // handle_io, drains it, and lands on this check again.)
    if (conn.peer_half_closed && conn.outbuf.empty()) {
        close_connection(fd);
    }
}

bool TcpServer::flush(int fd, Connection& conn) {
    while (!conn.outbuf.empty()) {
        ssize_t n = ::write(fd, conn.outbuf.data(), conn.outbuf.size());
        if (n > 0) {
            conn.outbuf.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // socket buffer full — the peer isn't keeping up
        }
        close_connection(fd);
        return false;
    }

    // Only subscribe to "writable" while bytes are actually stuck. A TCP
    // socket is writable almost always; leaving the subscription on would
    // spin the loop at 100% CPU doing nothing (level-triggered polling).
    // Similarly, drop "readable" after the peer's FIN: with level-
    // triggered polling, EOF stays permanently readable and would also
    // spin the loop.
    uint32_t interest = conn.peer_half_closed ? 0u : IoEvent::kReadable;
    if (!conn.outbuf.empty()) {
        interest |= IoEvent::kWritable;
    }
    loop_.set_interest(fd, interest);
    return true;
}

void TcpServer::close_connection(int fd) {
    loop_.remove_fd(fd);
    ::close(fd);
    conns_.erase(fd);
}

}  // namespace meridian
