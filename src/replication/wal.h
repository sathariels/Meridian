#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace meridian {

// Write-ahead log: a flat append-only file of protocol lines ("SET k 0 v",
// "SCORE alice 3200", ...). Recovery = replay every line through the same
// command handler that executed it live. One format, three jobs: durability,
// crash recovery, and the replication backlog a fresh follower receives.
//
// Logging the *command* (logical logging) rather than memory state
// (physical logging) costs replay-time CPU but keeps the log human-readable
// — `cat meridian.wal` IS the debugging tool — and reuses the parser that
// is already tested, instead of inventing a second serialization format.
class Wal {
public:
    // Opens (creating if needed) for append. If the file ends mid-line —
    // the signature of a crash during a write — the partial tail is
    // truncated first, so a new append can never fuse with torn bytes.
    // fsync_each: fsync() after every append. Off = fast, and a power
    // loss can eat recent writes still in the kernel page cache; on =
    // every acknowledged write survives, at ~100x the latency. (This is
    // Redis appendfsync everysec-vs-always. And on macOS even fsync()
    // only reaches the disk's own cache; F_FULLFSYNC is the real barrier
    // — noted, not used, because this is a portfolio server.)
    explicit Wal(std::string path, bool fsync_each = false);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // line must not contain '\n'; the Wal adds it.
    void append(const std::string& line);

    // Feed every complete line to apply, in order. Returns lines applied.
    std::size_t replay(
        const std::function<void(const std::string&)>& apply) const;

    // Entire log as raw bytes (newline-terminated lines) — the SYNC dump
    // a new follower receives before live streaming starts.
    std::string read_all() const;

    const std::string& path() const { return path_; }

private:
    std::string path_;
    int fd_ = -1;
    bool fsync_each_;
};

}  // namespace meridian
