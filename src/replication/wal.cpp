#include "replication/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace meridian {

namespace {

// A crash can leave a torn final line (write() interrupted mid-way). If we
// appended after it, the torn bytes would fuse with the next command into
// one corrupt line. Truncating back to the last complete line loses at
// most one un-acknowledged write and keeps the log parseable forever.
// (Same policy as Redis's aof-load-truncated.)
void truncate_partial_tail(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;  // no file yet — nothing to repair
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    in.close();

    if (content.empty() || content.back() == '\n') {
        return;
    }
    std::size_t last_newline = content.rfind('\n');
    std::size_t keep = (last_newline == std::string::npos)
                           ? 0
                           : last_newline + 1;
    ::truncate(path.c_str(), static_cast<off_t>(keep));
}

}  // namespace

Wal::Wal(std::string path, bool fsync_each)
    : path_(std::move(path)), fsync_each_(fsync_each) {
    truncate_partial_tail(path_);
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("wal: cannot open " + path_);
    }
}

Wal::~Wal() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void Wal::append(const std::string& line) {
    assert(line.find('\n') == std::string::npos);
    std::string record = line + "\n";
    std::size_t written = 0;
    while (written < record.size()) {
        ssize_t n = ::write(fd_, record.data() + written,
                            record.size() - written);
        if (n <= 0) {
            // Disk full / IO error. A real system would panic or shed
            // writes; we log it and keep serving from memory.
            std::fprintf(stderr, "wal: append failed on %s\n",
                         path_.c_str());
            return;
        }
        written += static_cast<std::size_t>(n);
    }
    if (fsync_each_) {
        ::fsync(fd_);
    }
}

std::size_t Wal::replay(
    const std::function<void(const std::string&)>& apply) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return 0;
    }
    std::size_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        apply(line);
        ++count;
    }
    return count;
}

std::string Wal::read_all() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace meridian
