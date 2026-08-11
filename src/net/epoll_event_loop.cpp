// epoll implementation of EventLoop (Linux). It deliberately uses
// level-triggered readiness to match the kqueue backend's behavior.

#include "net/event_loop.h"

#ifdef __linux__

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace meridian {

namespace {

class EpollEventLoop final : public EventLoop {
public:
    EpollEventLoop() {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "epoll_create1");
        }

        if (pipe2(wake_pipe_, O_NONBLOCK | O_CLOEXEC) != 0) {
            int error = errno;
            close(epoll_fd_);
            epoll_fd_ = -1;
            throw std::system_error(error, std::generic_category(),
                                    "pipe2");
        }

        struct epoll_event event {};
        event.events = EPOLLIN;
        event.data.fd = wake_pipe_[0];
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_pipe_[0], &event) !=
            0) {
            int error = errno;
            close(wake_pipe_[0]);
            close(wake_pipe_[1]);
            close(epoll_fd_);
            wake_pipe_[0] = -1;
            wake_pipe_[1] = -1;
            epoll_fd_ = -1;
            throw std::system_error(error, std::generic_category(),
                                    "epoll_ctl wake pipe");
        }
    }

    ~EpollEventLoop() override {
        close(wake_pipe_[0]);
        close(wake_pipe_[1]);
        close(epoll_fd_);
    }

    void add_fd(int fd, uint32_t interest, IoCallback cb) override {
        auto [it, inserted] = handlers_.emplace(
            fd, Handler{.cb = std::move(cb), .interest = 0});
        if (!inserted) {
            throw std::logic_error("epoll: fd is already registered");
        }

        try {
            apply_interest_change(fd, it->second, interest);
        } catch (...) {
            handlers_.erase(it);
            throw;
        }
    }

    void set_interest(int fd, uint32_t interest) override {
        auto it = handlers_.find(fd);
        if (it == handlers_.end() || it->second.interest == interest) {
            return;
        }
        apply_interest_change(fd, it->second, interest);
    }

    void remove_fd(int fd) override {
        auto it = handlers_.find(fd);
        if (it == handlers_.end()) {
            return;
        }
        if (it->second.interest != 0) {
            // Closing a socket can race with cleanup. The descriptor is
            // leaving our ownership either way, so DEL is best-effort.
            (void)epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        handlers_.erase(it);
    }

    void run() override {
        running_.store(true, std::memory_order_release);
        std::array<struct epoll_event, 64> events;

        while (running_.load(std::memory_order_acquire)) {
            int n = epoll_wait(epoll_fd_, events.data(),
                               static_cast<int>(events.size()), -1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == wake_pipe_[0]) {
                    drain_wake_pipe();
                    continue;
                }

                // An earlier callback in this batch may have removed fd.
                auto it = handlers_.find(fd);
                if (it == handlers_.end()) {
                    continue;
                }

                uint32_t ready = 0;
                uint32_t native = events[i].events;
                if (native & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    ready |= IoEvent::kReadable;
                }
                if (native & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
                    ready |= IoEvent::kWritable;
                }

                // The callback may unregister itself, which destroys the
                // function stored in the map. Keep this invocation alive.
                IoCallback cb = it->second.cb;
                cb(ready);
            }
        }
    }

    void stop() override {
        running_.store(false, std::memory_order_release);
        char byte = 1;
        // Both pipe ends are nonblocking. EAGAIN means a wakeup is already
        // pending, so dropping this byte is safe and signal-handler friendly.
        (void)write(wake_pipe_[1], &byte, 1);
    }

private:
    struct Handler {
        IoCallback cb;
        uint32_t interest = 0;
    };

    static uint32_t native_interest(uint32_t interest) {
        uint32_t native = 0;
        if (interest & IoEvent::kReadable) {
            native |= EPOLLIN | EPOLLRDHUP;
        }
        if (interest & IoEvent::kWritable) {
            native |= EPOLLOUT;
        }
        return native;
    }

    void apply_interest_change(int fd, Handler& handler,
                               uint32_t next_interest) {
        if (handler.interest == 0 && next_interest == 0) {
            return;
        }

        int operation;
        if (handler.interest == 0) {
            operation = EPOLL_CTL_ADD;
        } else if (next_interest == 0) {
            operation = EPOLL_CTL_DEL;
        } else {
            operation = EPOLL_CTL_MOD;
        }

        int rc;
        if (operation == EPOLL_CTL_DEL) {
            rc = epoll_ctl(epoll_fd_, operation, fd, nullptr);
        } else {
            struct epoll_event event {};
            event.events = native_interest(next_interest);
            event.data.fd = fd;
            rc = epoll_ctl(epoll_fd_, operation, fd, &event);
        }
        if (rc != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "epoll_ctl interest change");
        }
        handler.interest = next_interest;
    }

    void drain_wake_pipe() {
        char buffer[64];
        while (read(wake_pipe_[0], buffer, sizeof(buffer)) > 0) {
        }
    }

    int epoll_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::unordered_map<int, Handler> handlers_;
};

}  // namespace

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<EpollEventLoop>();
}

}  // namespace meridian

#endif  // __linux__
