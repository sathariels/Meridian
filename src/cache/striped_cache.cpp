#include "cache/striped_cache.h"

#include <cassert>
#include <functional>

namespace meridian {

namespace {

// splitmix64 finalizer. std::hash<std::string> is decent, but we take its
// result % num_stripes for stripe selection while the stripe's inner
// unordered_map ALSO buckets by (same hash) % bucket_count. Without
// remixing, every key landing in stripe k shares the property
// hash ≡ k (mod num_stripes), which correlates with the inner map's bucket
// choice and can cluster keys into few buckets. Remixing decorrelates the
// two uses of the hash.
uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

}  // namespace

StripedCache::StripedCache(std::size_t total_capacity,
                           std::size_t num_stripes, ClockFn clock) {
    assert(total_capacity >= 1);
    assert(num_stripes >= 1);

    // Round per-stripe capacity up: every stripe must hold at least one
    // entry or a put into it would evict itself.
    std::size_t per_stripe =
        (total_capacity + num_stripes - 1) / num_stripes;

    stripes_.reserve(num_stripes);
    for (std::size_t i = 0; i < num_stripes; ++i) {
        stripes_.push_back(std::make_unique<Stripe>(per_stripe, clock));
    }
}

void StripedCache::put(const std::string& key, const std::string& value,
                       int64_t ttl_ms) {
    Stripe& stripe = *stripes_[stripe_for(key)];
    std::lock_guard<std::mutex> lock(stripe.mu);
    stripe.cache.put(key, value, ttl_ms);
}

std::optional<std::string> StripedCache::get(const std::string& key) {
    Stripe& stripe = *stripes_[stripe_for(key)];
    std::lock_guard<std::mutex> lock(stripe.mu);
    return stripe.cache.get(key);
}

bool StripedCache::erase(const std::string& key) {
    Stripe& stripe = *stripes_[stripe_for(key)];
    std::lock_guard<std::mutex> lock(stripe.mu);
    return stripe.cache.erase(key);
}

bool StripedCache::contains(const std::string& key) const {
    const Stripe& stripe = *stripes_[stripe_for(key)];
    std::lock_guard<std::mutex> lock(stripe.mu);
    return stripe.cache.contains(key);
}

std::size_t StripedCache::size() const {
    std::size_t total = 0;
    for (const auto& stripe : stripes_) {
        std::lock_guard<std::mutex> lock(stripe->mu);
        total += stripe->cache.size();
    }
    return total;
}

std::size_t StripedCache::stripe_for(const std::string& key) const {
    return mix64(std::hash<std::string>{}(key)) % stripes_.size();
}

}  // namespace meridian
