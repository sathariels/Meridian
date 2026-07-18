#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "net/event_loop.h"

namespace meridian {

// Follower's connection to its leader. On start(): connect, send "SYNC",
// then sit in the event loop applying every line the leader streams —
// first the log backlog (history), then live mutations as they happen.
// The follower can't tell where history ends and live begins, and doesn't
// need to: replaying is applying.
//
// Simplification, on purpose (see CLAUDE.md non-goals): if the leader
// dies, the follower stops its loop and exits rather than retrying or
// holding an election. Promotion/failover is consensus territory.
class ReplicaLink {
public:
    using ApplyFn = std::function<void(const std::string& line)>;

    ReplicaLink(EventLoop& loop, std::string leader_host,
                uint16_t leader_port, ApplyFn apply);
    ~ReplicaLink();

    ReplicaLink(const ReplicaLink&) = delete;
    ReplicaLink& operator=(const ReplicaLink&) = delete;

    // Blocking connect + SYNC handshake (startup-time, before the loop
    // runs, so blocking is fine). Throws std::runtime_error on failure.
    void start();

private:
    void handle_readable();

    EventLoop& loop_;
    std::string host_;
    uint16_t port_;
    ApplyFn apply_;
    int fd_ = -1;
    std::string inbuf_;
};

}  // namespace meridian
