#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace meridian {

// Monotonic time source, milliseconds. Injectable so tests can control the
// clock instead of sleeping. Uses steady_clock, not system_clock: TTLs must
// not misfire when the wall clock jumps (NTP sync, DST).
using ClockFn = std::function<int64_t()>;

int64_t steady_now_ms();

// Single-threaded LRU cache with per-entry TTL.
//
// Layout: unordered_map<key, Node*> for O(1) lookup, plus a hand-rolled
// doubly linked list threaded through the Nodes ordering them by recency
// (head = most recent, tail = least recent). The map points at the node
// rather than owning a copy, so a get() is: one hash lookup, then O(1)
// pointer surgery to move the node to the head. No allocation, no copies.
//
// Expiry is lazy (checked on access) rather than via a background sweep or
// timer wheel — see CLAUDE.md phase 1 scope. Consequence: size() counts
// entries that have expired but not yet been touched.
class LruCache {
public:
    // capacity must be >= 1. Entries beyond capacity evict the LRU entry.
    explicit LruCache(std::size_t capacity, ClockFn clock = steady_now_ms);
    ~LruCache();

    // The map stores raw Node pointers; copying would double-free.
    // Moving is not needed yet, so rule of five collapses to "none of it".
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;
    LruCache(LruCache&&) = delete;
    LruCache& operator=(LruCache&&) = delete;

    // Insert or overwrite. ttl_ms <= 0 means the entry never expires.
    // Both paths mark the key most-recently-used.
    void put(const std::string& key, const std::string& value,
             int64_t ttl_ms = 0);

    // Returns the value and marks the key most-recently-used.
    // An expired entry is erased on the spot and reported as a miss.
    std::optional<std::string> get(const std::string& key);

    // Returns true if the key was present (expired-but-uncollected counts:
    // the caller asked to remove it and it was removed either way).
    bool erase(const std::string& key);

    bool contains(const std::string& key) const;

    // Live entry count, including expired entries not yet lazily collected.
    std::size_t size() const { return map_.size(); }
    std::size_t capacity() const { return capacity_; }

private:
    struct Node {
        std::string key;      // stored so eviction can erase the map entry
        std::string value;
        int64_t expires_at_ms;  // 0 = never expires
        Node* prev = nullptr;
        Node* next = nullptr;
    };

    bool is_expired(const Node& node) const;

    // Recency-list surgery. detach() leaves the node's own pointers dangling;
    // callers either push_front() it again or delete it immediately.
    void detach(Node* node);
    void push_front(Node* node);

    // Removes from both map and list, frees the node.
    void remove_node(Node* node);

    std::size_t capacity_;
    ClockFn clock_;
    std::unordered_map<std::string, Node*> map_;
    Node* head_ = nullptr;  // most recently used
    Node* tail_ = nullptr;  // least recently used, next eviction victim
};

}  // namespace meridian
