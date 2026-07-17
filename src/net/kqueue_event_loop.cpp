// kqueue implementation of EventLoop (macOS/BSD). The Linux twin
// (epoll_event_loop.cpp) arrives in phase 8; CMake picks one per platform.

#include "net/event_loop.h"

#ifdef __APPLE__

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <unordered_map>

namespace meridian {

namespace {

class KqueueEventLoop final : public EventLoop {
public:
    KqueueEventLoop() {
        kq_ = kqueue();
        assert(kq_ >= 0);

        // Self-pipe: stop() writes one byte, which wakes the blocking
        // kevent() call. This is the classic way to interrupt a poller
        // from another thread (or a signal handler) — write() is
        // async-signal-safe, and no lock is needed.
        int rc = pipe(wake_pipe_);
        assert(rc == 0);
        fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);

        struct kevent ev;
        EV_SET(&ev, wake_pipe_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

    ~KqueueEventLoop() override {
        close(wake_pipe_[0]);
        close(wake_pipe_[1]);
        close(kq_);
    }

    void add_fd(int fd, uint32_t interest, IoCallback cb) override {
        handlers_[fd] = Handler{std::move(cb), 0};
        apply_interest_change(fd, /*old=*/0, /*next=*/interest);
        handlers_[fd].interest = interest;
    }

    void set_interest(int fd, uint32_t interest) override {
        auto it = handlers_.find(fd);
        if (it == handlers_.end()) {
            return;
        }
        apply_interest_change(fd, it->second.interest, interest);
        it->second.interest = interest;
    }

    void remove_fd(int fd) override {
        auto it = handlers_.find(fd);
        if (it == handlers_.end()) {
            return;
        }
        apply_interest_change(fd, it->second.interest, 0);
        handlers_.erase(it);
    }

    void run() override {
        running_.store(true, std::memory_order_release);
        std::array<struct kevent, 64> events;

        while (running_.load(std::memory_order_acquire)) {
            int n = kevent(kq_, nullptr, 0, events.data(),
                           static_cast<int>(events.size()), nullptr);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;  // interrupted by a signal; not an error
                }
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = static_cast<int>(events[i].ident);

                if (fd == wake_pipe_[0]) {
                    drain_wake_pipe();
                    continue;
                }

                // A callback earlier in this same batch may have removed
                // this fd (e.g. closed the connection), so re-check.
                auto it = handlers_.find(fd);
                if (it == handlers_.end()) {
                    continue;
                }

                uint32_t ready = 0;
                if (events[i].filter == EVFILT_READ) {
                    // EV_EOF (peer hung up) is folded into "readable":
                    // the owner's read() will return 0 and it can close.
                    ready |= IoEvent::kReadable;
                }
                if (events[i].filter == EVFILT_WRITE) {
                    ready |= IoEvent::kWritable;
                }

                // Copy the callback before invoking: if it calls
                // remove_fd on itself, the map entry — and the
                // std::function stored there — is destroyed while we're
                // inside it. Destroying a std::function mid-call is UB;
                // the copy keeps the code we're executing alive.
                IoCallback cb = it->second.cb;
                cb(ready);
            }
        }
    }

    void stop() override {
        running_.store(false, std::memory_order_release);
        char byte = 1;
        // Best-effort: if the pipe is somehow full, the loop is already
        // due to wake up anyway.
        (void)write(wake_pipe_[1], &byte, 1);
    }

private:
    struct Handler {
        IoCallback cb;
        uint32_t interest = 0;
    };

    // kqueue registers read and write interest as two separate filters,
    // so an interest change becomes an add/delete per filter that flipped.
    void apply_interest_change(int fd, uint32_t old_interest,
                               uint32_t next_interest) {
        uint32_t added = next_interest & ~old_interest;
        uint32_t removed = old_interest & ~next_interest;

        struct kevent changes[2];
        int n = 0;
        if (added & IoEvent::kReadable) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
        }
        if (removed & IoEvent::kReadable) {
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }
        if (n > 0) {
            kevent(kq_, changes, n, nullptr, 0, nullptr);
        }

        n = 0;
        if (added & IoEvent::kWritable) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
        }
        if (removed & IoEvent::kWritable) {
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }
        if (n > 0) {
            // Errors (e.g. deleting a filter on an fd the kernel already
            // dropped) are ignored: the fd is on its way out either way.
            kevent(kq_, changes, n, nullptr, 0, nullptr);
        }
    }

    void drain_wake_pipe() {
        char buf[64];
        while (read(wake_pipe_[0], buf, sizeof(buf)) > 0) {
        }
    }

    int kq_ = -1;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::unordered_map<int, Handler> handlers_;
};

}  // namespace

std::unique_ptr<EventLoop> EventLoop::create() {
    return std::make_unique<KqueueEventLoop>();
}

}  // namespace meridian

#endif  // __APPLE__
