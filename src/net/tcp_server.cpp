#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
        if (conn.file_transfer.has_value()) {
            ::close(conn.file_transfer->fd);
        }
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
                std::size_t newline = conn.inbuf.find('\n', conn.in_offset);
                if (newline == std::string::npos &&
                    conn.inbuf.size() - conn.in_offset > kMaxLineBytes) {
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
        while ((newline = conn.inbuf.find('\n', conn.in_offset)) !=
               std::string::npos) {
            if (newline - conn.in_offset > kMaxLineBytes) {
                close_connection(fd);
                return;
            }
            std::string line = conn.inbuf.substr(conn.in_offset,
                                                 newline - conn.in_offset);
            conn.in_offset = newline + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();  // be telnet-friendly: accept \r\n
            }
            std::string response = on_line_(fd, line);

            // The handler may have called push(), and push() may have
            // closed THIS connection on a write error — in which case
            // `conn` is a dangling reference. Re-look-up before touching
            // it again.
            it = conns_.find(fd);
            if (it == conns_.end()) {
                return;
            }
            Connection& conn_now = it->second;
            if (!response.empty()) {  // "" = deliberately silent
                response += '\n';
                queue_output(conn_now, response);
            }
        }
        if (conn.inbuf.size() - conn.in_offset > kMaxLineBytes) {
            close_connection(fd);
            return;
        }
        compact_input(conn);
    }

    if (!flush(fd, conn)) {
        return;  // connection closed inside flush
    }

    // Half-closed and nothing left to send: our side's turn to hang up.
    // (If output remains, the next writable event re-enters
    // handle_io, drains it, and lands on this check again.)
    if (conn.peer_half_closed && !has_pending_output(conn)) {
        close_connection(fd);
    }
}

bool TcpServer::flush(int fd, Connection& conn) {
    bool blocked = false;
    while (!blocked) {
        while (conn.out_offset < conn.outbuf.size()) {
            ssize_t n = ::write(fd, conn.outbuf.data() + conn.out_offset,
                                conn.outbuf.size() - conn.out_offset);
            if (n > 0) {
                conn.out_offset += static_cast<std::size_t>(n);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                blocked = true;
                break;
            }
            close_connection(fd);
            return false;
        }
        if (blocked) {
            break;
        }

        conn.outbuf.clear();
        conn.out_offset = 0;
        if (!conn.file_transfer.has_value()) {
            break;
        }

        FileTransfer& transfer = *conn.file_transfer;
        if (transfer.offset == transfer.buffer.size()) {
            transfer.buffer.clear();
            transfer.offset = 0;
            if (transfer.remaining == 0) {
                ::close(transfer.fd);
                conn.file_transfer.reset();
                conn.outbuf.swap(conn.after_file);
                continue;
            }

            std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(transfer.remaining,
                                        kFileChunkBytes));
            transfer.buffer.resize(chunk);
            ssize_t n;
            do {
                n = ::read(transfer.fd, transfer.buffer.data(), chunk);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) {
                close_connection(fd);
                return false;
            }
            transfer.buffer.resize(static_cast<std::size_t>(n));
            transfer.remaining -= static_cast<std::uint64_t>(n);
        }

        while (transfer.offset < transfer.buffer.size()) {
            ssize_t n = ::write(fd,
                                transfer.buffer.data() + transfer.offset,
                                transfer.buffer.size() - transfer.offset);
            if (n > 0) {
                transfer.offset += static_cast<std::size_t>(n);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                blocked = true;
                break;
            }
            close_connection(fd);
            return false;
        }
    }

    compact_output(conn);

    // Only subscribe to "writable" while bytes are actually stuck. A TCP
    // socket is writable almost always; leaving the subscription on would
    // spin the loop at 100% CPU doing nothing (level-triggered polling).
    // Similarly, drop "readable" after the peer's FIN: with level-
    // triggered polling, EOF stays permanently readable and would also
    // spin the loop.
    uint32_t interest = conn.peer_half_closed ? 0u : IoEvent::kReadable;
    if (has_pending_output(conn)) {
        interest |= IoEvent::kWritable;
    }
    loop_.set_interest(fd, interest);
    return true;
}

void TcpServer::push(int client_id, const std::string& data) {
    auto it = conns_.find(client_id);
    if (it == conns_.end()) {
        return;  // connection already gone; drop silently
    }
    queue_output(it->second, data);
    flush(client_id, it->second);
}

bool TcpServer::push_file(int client_id, const std::string& path,
                          std::uint64_t byte_count) {
    auto it = conns_.find(client_id);
    if (it == conns_.end()) {
        return false;
    }
    if (byte_count == 0) {
        return true;
    }
    Connection& conn = it->second;
    if (conn.file_transfer.has_value()) {
        return false;
    }

    int file_fd = ::open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        return false;
    }
    conn.file_transfer.emplace();
    conn.file_transfer->fd = file_fd;
    conn.file_transfer->remaining = byte_count;
    return flush(client_id, conn);
}

void TcpServer::queue_output(Connection& conn, const std::string& data) {
    if (conn.file_transfer.has_value()) {
        conn.after_file += data;
    } else {
        conn.outbuf += data;
    }
}

bool TcpServer::has_pending_output(const Connection& conn) {
    return conn.out_offset < conn.outbuf.size() ||
           conn.file_transfer.has_value() || !conn.after_file.empty();
}

void TcpServer::compact_input(Connection& conn) {
    if (conn.in_offset == conn.inbuf.size()) {
        conn.inbuf.clear();
        conn.in_offset = 0;
        return;
    }
    if (conn.in_offset >= 4096 &&
        conn.in_offset >= conn.inbuf.size() / 2) {
        conn.inbuf.erase(0, conn.in_offset);
        conn.in_offset = 0;
    }
}

void TcpServer::compact_output(Connection& conn) {
    if (conn.out_offset >= 4096 &&
        conn.out_offset >= conn.outbuf.size() / 2) {
        conn.outbuf.erase(0, conn.out_offset);
        conn.out_offset = 0;
    }
}

void TcpServer::set_disconnect_handler(DisconnectHandler on_disconnect) {
    on_disconnect_ = std::move(on_disconnect);
}

void TcpServer::close_connection(int fd) {
    auto it = conns_.find(fd);
    if (it != conns_.end() && it->second.file_transfer.has_value()) {
        ::close(it->second.file_transfer->fd);
    }
    loop_.remove_fd(fd);
    ::close(fd);
    conns_.erase(fd);
    if (on_disconnect_) {
        on_disconnect_(fd);
    }
}

}  // namespace meridian
