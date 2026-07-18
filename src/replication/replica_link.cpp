#include "replication/replica_link.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace meridian {

ReplicaLink::ReplicaLink(EventLoop& loop, std::string leader_host,
                         uint16_t leader_port, ApplyFn apply)
    : loop_(loop),
      host_(std::move(leader_host)),
      port_(leader_port),
      apply_(std::move(apply)) {}

ReplicaLink::~ReplicaLink() {
    if (fd_ >= 0) {
        loop_.remove_fd(fd_);
        ::close(fd_);
    }
}

void ReplicaLink::start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("replica: socket() failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("replica: bad leader address " + host_);
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        throw std::runtime_error(
            std::string("replica: cannot reach leader: ") +
            std::strerror(errno));
    }

    const char sync_cmd[] = "SYNC\n";
    if (::write(fd_, sync_cmd, sizeof(sync_cmd) - 1) !=
        static_cast<ssize_t>(sizeof(sync_cmd) - 1)) {
        throw std::runtime_error("replica: SYNC handshake failed");
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    loop_.add_fd(fd_, IoEvent::kReadable,
                 [this](uint32_t) { handle_readable(); });
}

void ReplicaLink::handle_readable() {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            inbuf_.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            std::fprintf(stderr,
                         "replica: leader closed the connection; "
                         "shutting down (no failover by design)\n");
            loop_.stop();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        std::fprintf(stderr, "replica: stream error: %s\n",
                     std::strerror(errno));
        loop_.stop();
        return;
    }

    std::size_t newline;
    while ((newline = inbuf_.find('\n')) != std::string::npos) {
        std::string line = inbuf_.substr(0, newline);
        inbuf_.erase(0, newline + 1);
        if (!line.empty()) {
            apply_(line);
        }
    }
}

}  // namespace meridian
