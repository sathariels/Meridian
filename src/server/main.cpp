// meridian_server: the cache behind a TCP port.
//
//   ./meridian_server --port 7070 --capacity 100000 --stripes 16
//   printf 'SET player:1 0 mmr=3200\nGET player:1\n' | nc 127.0.0.1 7070

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "cache/striped_cache.h"
#include "net/event_loop.h"
#include "net/tcp_server.h"
#include "server/command_handler.h"

namespace {

// Signal handlers can't capture state, so the loop pointer must be global.
// EventLoop::stop() is async-signal-safe by design (atomic store + pipe
// write), which is what makes this handler legal.
meridian::EventLoop* g_loop = nullptr;

void on_signal(int) {
    if (g_loop != nullptr) {
        g_loop->stop();
    }
}

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 7070;
    std::size_t capacity = 100000;
    std::size_t stripes = 16;
};

Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc - 1; ++i) {
        std::string flag = argv[i];
        if (flag == "--host") opt.host = argv[i + 1];
        else if (flag == "--port")
            opt.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        else if (flag == "--capacity")
            opt.capacity = static_cast<std::size_t>(std::atoll(argv[i + 1]));
        else if (flag == "--stripes")
            opt.stripes = static_cast<std::size_t>(std::atoll(argv[i + 1]));
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);

    // A client that disconnects mid-response would otherwise kill the
    // whole process with SIGPIPE on our next write() to it. Ignore the
    // signal and let write() return EPIPE instead, which the server
    // handles as a normal connection close.
    std::signal(SIGPIPE, SIG_IGN);

    meridian::StripedCache cache(opt.capacity, opt.stripes);
    auto loop = meridian::EventLoop::create();

    meridian::TcpServer server(
        *loop, opt.host, opt.port, [&cache](const std::string& line) {
            return meridian::handle_command(cache, line);
        });

    try {
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    g_loop = loop.get();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "meridian listening on " << opt.host << ":" << server.port()
              << " (capacity=" << opt.capacity
              << ", stripes=" << opt.stripes << ")\n";

    loop->run();
    std::cout << "shutting down\n";
    return 0;
}
