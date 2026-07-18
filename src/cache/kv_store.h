#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace meridian {

// What the protocol layer needs from "a place that stores keys" — nothing
// more. StripedCache implements it (single node) and ShardRouter
// implements it (fans out across shards), so the command handler works
// against either without knowing the topology.
//
// Note: no default argument on put()'s ttl_ms here. Default arguments on
// virtual functions bind statically (to the declared type, not the
// dynamic one), which is a classic C++ footgun; concrete classes may add
// the convenience default on their own overrides.
class KvStore {
public:
    virtual ~KvStore() = default;

    virtual void put(const std::string& key, const std::string& value,
                     int64_t ttl_ms) = 0;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool erase(const std::string& key) = 0;
    virtual bool contains(const std::string& key) const = 0;
    virtual std::size_t size() const = 0;
};

}  // namespace meridian
