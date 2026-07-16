#include "cache/lru_cache.h"

#include <cassert>
#include <chrono>

namespace meridian {

int64_t steady_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

LruCache::LruCache(std::size_t capacity, ClockFn clock)
    : capacity_(capacity), clock_(std::move(clock)) {
    assert(capacity_ >= 1);
}

LruCache::~LruCache() {
    Node* node = head_;
    while (node != nullptr) {
        Node* next = node->next;
        delete node;
        node = next;
    }
}

void LruCache::put(const std::string& key, const std::string& value,
                   int64_t ttl_ms) {
    int64_t expires_at = ttl_ms > 0 ? clock_() + ttl_ms : 0;

    auto it = map_.find(key);
    if (it != map_.end()) {
        // Overwrite in place: reuse the node so the map entry stays valid.
        Node* node = it->second;
        node->value = value;
        node->expires_at_ms = expires_at;
        detach(node);
        push_front(node);
        return;
    }

    if (map_.size() >= capacity_) {
        // Evict strictly by recency. We do not hunt for expired entries
        // first — that would be an O(n) scan on the hot write path, and the
        // LRU victim is a reasonable proxy anyway (expired entries stop
        // being touched, so they drift toward the tail on their own).
        remove_node(tail_);
    }

    Node* node = new Node{key, value, expires_at};
    push_front(node);
    map_.emplace(key, node);
}

std::optional<std::string> LruCache::get(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
        return std::nullopt;
    }

    Node* node = it->second;
    if (is_expired(*node)) {
        remove_node(node);
        return std::nullopt;
    }

    detach(node);
    push_front(node);
    return node->value;
}

bool LruCache::erase(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }
    remove_node(it->second);
    return true;
}

bool LruCache::contains(const std::string& key) const {
    auto it = map_.find(key);
    return it != map_.end() && !is_expired(*it->second);
}

bool LruCache::is_expired(const Node& node) const {
    return node.expires_at_ms != 0 && clock_() >= node.expires_at_ms;
}

void LruCache::detach(Node* node) {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        head_ = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    } else {
        tail_ = node->prev;
    }
}

void LruCache::push_front(Node* node) {
    node->prev = nullptr;
    node->next = head_;
    if (head_ != nullptr) {
        head_->prev = node;
    }
    head_ = node;
    if (tail_ == nullptr) {
        tail_ = node;
    }
}

void LruCache::remove_node(Node* node) {
    detach(node);
    map_.erase(node->key);
    delete node;
}

}  // namespace meridian
