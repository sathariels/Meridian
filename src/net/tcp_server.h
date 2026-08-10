#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "net/event_loop.h"

namespace meridian {

// Non-blocking TCP server speaking a newline-delimited text protocol.
// All sockets are owned by the event loop's single thread; the server has
// no locks because nothing here is ever touched by two threads at once.
// (The cache behind the handler is thread-safe anyway, which keeps the
// door open for multiple loop threads later.)
class TcpServer {
public:
    // Called once per complete input line (without the trailing newline).
    // client_id identifies the connection (it's the fd, but treat it as
    // opaque — valid for push() until on_disconnect fires for it). The
    // returned string is sent back followed by '\n'; return "" to send
    // nothing (a connection being converted to a push stream, e.g. SYNC).
    using LineHandler =
        std::function<std::string(int client_id, const std::string&)>;
    using DisconnectHandler = std::function<void(int client_id)>;

    // port 0 = let the OS pick (tests use this); the real port is
    // available from port() after start().
    TcpServer(EventLoop& loop, std::string host, uint16_t port,
              LineHandler on_line);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Binds and starts accepting. Throws std::runtime_error on failure
    // (port in use, bad host, ...).
    void start();

    uint16_t port() const { return port_; }
    std::size_t connection_count() const { return conns_.size(); }

    // Queue raw bytes (caller supplies newlines) to a live connection,
    // outside the request/response cycle — how the leader streams log
    // entries to replicas. Unknown/closed ids are ignored. Loop thread
    // only. May close the connection on write error, which fires the
    // disconnect handler reentrantly — callers iterating a set of ids
    // must iterate a copy.
    void push(int client_id, const std::string& data);

    // Queue the first byte_count bytes of a file without loading the whole
    // file into memory. Bytes already queued stay ahead of the file; later
    // push() calls stay behind it. Returns false if the connection is gone
    // or the file cannot be opened. Loop thread only.
    bool push_file(int client_id, const std::string& path,
                   std::uint64_t byte_count);

    // Called after a live connection dies (peer hangup or write error;
    // not during server teardown) — the hook that lets the replication
    // layer drop dead replicas.
    void set_disconnect_handler(DisconnectHandler on_disconnect);

private:
    struct FileTransfer {
        int fd = -1;
        std::uint64_t remaining = 0;
        std::string buffer;
        std::size_t offset = 0;
    };

    struct Connection {
        std::string inbuf;
        std::size_t in_offset = 0;

        // Output is ordered as outbuf, file_transfer, then after_file.
        // Offsets avoid moving remaining bytes after every partial IO.
        std::string outbuf;
        std::size_t out_offset = 0;
        std::optional<FileTransfer> file_transfer;
        std::string after_file;
        // Peer sent FIN (EOF on read). It may have only half-closed —
        // netcat does this after stdin runs dry — so it can still receive
        // our responses. We finish writing pending output, then close.
        bool peer_half_closed = false;
    };

    void handle_accept();
    void handle_io(int fd, uint32_t events);
    // Writes as much pending output as the socket accepts; keeps kWritable
    // interest on only while a partial write is pending. Returns false if
    // the connection died (the Connection& is invalid after that).
    bool flush(int fd, Connection& conn);
    void queue_output(Connection& conn, const std::string& data);
    static bool has_pending_output(const Connection& conn);
    static void compact_input(Connection& conn);
    static void compact_output(Connection& conn);
    void close_connection(int fd);

    // A client that sends this much without a newline is broken or
    // hostile; we hang up rather than buffer without bound.
    static constexpr std::size_t kMaxLineBytes = 64 * 1024;
    static constexpr std::size_t kFileChunkBytes = 64 * 1024;

    EventLoop& loop_;
    std::string host_;
    uint16_t port_;
    LineHandler on_line_;
    DisconnectHandler on_disconnect_;
    int listen_fd_ = -1;
    std::unordered_map<int, Connection> conns_;
};

}  // namespace meridian
