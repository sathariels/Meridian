#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "net/event_loop.h"

namespace {

void test_readable_interest_and_cross_thread_stop() {
    int fds[2];
    assert(pipe(fds) == 0);
    int flags = fcntl(fds[0], F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == 0);

    auto loop = meridian::EventLoop::create();
    std::atomic<bool> observed{false};

    // Register with no interest first. Enabling readability separately
    // exercises the backend's inactive-to-active transition.
    loop->add_fd(fds[0], 0, [&](uint32_t events) {
        assert(events & meridian::IoEvent::kReadable);
        char byte = 0;
        assert(read(fds[0], &byte, 1) == 1);
        assert(byte == 'x');
        observed.store(true, std::memory_order_release);
    });
    loop->set_interest(fds[0], meridian::IoEvent::kReadable);

    std::thread runner([&] { loop->run(); });
    const char byte = 'x';
    assert(write(fds[1], &byte, 1) == 1);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
    while (!observed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    // stop() runs from this thread while the event loop is blocked again.
    loop->stop();
    runner.join();
    assert(observed.load(std::memory_order_acquire));

    loop->remove_fd(fds[0]);
    close(fds[0]);
    close(fds[1]);
}

}  // namespace

int main() {
    test_readable_interest_and_cross_thread_stop();
    std::cout << "all event_loop tests passed\n";
    return 0;
}
