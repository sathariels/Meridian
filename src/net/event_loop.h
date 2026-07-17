#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace meridian {

// IO-readiness bits, both for registering interest and for what a callback
// receives. Deliberately our own constants rather than kqueue's or epoll's:
// this header is the seam where the two platforms meet.
struct IoEvent {
    static constexpr uint32_t kReadable = 1u << 0;
    static constexpr uint32_t kWritable = 1u << 1;
};

// Readiness-notification event loop ("tell me when this fd is
// readable/writable, then I'll do the read myself"). kqueue on macOS,
// epoll on Linux (phase 8) — same interface, chosen at build time.
//
// Threading model: single-threaded. All methods except stop() must be
// called from the loop's own thread (or before run() starts). stop() is
// async-signal-safe and may be called from anywhere.
class EventLoop {
public:
    using IoCallback = std::function<void(uint32_t events)>;

    virtual ~EventLoop() = default;

    // Registers fd. cb fires with an IoEvent bitmask whenever fd is ready
    // for something in `interest`. The fd must already be non-blocking.
    virtual void add_fd(int fd, uint32_t interest, IoCallback cb) = 0;

    // Replaces the interest set (e.g. enable kWritable while a partial
    // write is pending, drop it once the buffer drains).
    virtual void set_interest(int fd, uint32_t interest) = 0;

    // Unregisters fd. Safe to call from inside fd's own callback.
    virtual void remove_fd(int fd) = 0;

    // Blocks dispatching events until stop() is called.
    virtual void run() = 0;
    virtual void stop() = 0;

    // Builds the platform's implementation (kqueue here).
    static std::unique_ptr<EventLoop> create();
};

}  // namespace meridian
