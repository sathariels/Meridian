#include "server/command_handler.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace meridian {

namespace {

// Pops the next space-delimited token off the front of sv. string_view
// slicing means no allocation until we actually keep something.
std::string_view next_token(std::string_view& sv) {
    std::size_t start = sv.find_first_not_of(' ');
    if (start == std::string_view::npos) {
        sv = {};
        return {};
    }
    std::size_t end = sv.find(' ', start);
    std::string_view token = sv.substr(start, end - start);
    sv = (end == std::string_view::npos) ? std::string_view{}
                                         : sv.substr(end + 1);
    return token;
}

bool parse_ttl(std::string_view token, int64_t& out) {
    if (token.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return false;  // negative or garbage TTLs are protocol errors
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

std::string handle_command(StripedCache& cache, const std::string& line) {
    std::string_view rest = line;
    std::string_view cmd = next_token(rest);

    if (cmd.empty()) {
        return "ERR empty command";
    }

    if (cmd == "PING") {
        return "PONG";
    }

    if (cmd == "GET") {
        std::string_view key = next_token(rest);
        if (key.empty() || !rest.empty()) {
            return "ERR usage: GET <key>";
        }
        auto value = cache.get(std::string(key));
        return value.has_value() ? "VALUE " + *value : "NOT_FOUND";
    }

    if (cmd == "SET") {
        std::string_view key = next_token(rest);
        std::string_view ttl_token = next_token(rest);
        int64_t ttl_ms = 0;
        if (key.empty() || !parse_ttl(ttl_token, ttl_ms)) {
            return "ERR usage: SET <key> <ttl_ms> <value>";
        }
        // `rest` is everything after the ttl — the value, spaces and all.
        cache.put(std::string(key), std::string(rest), ttl_ms);
        return "OK";
    }

    if (cmd == "DEL") {
        std::string_view key = next_token(rest);
        if (key.empty() || !rest.empty()) {
            return "ERR usage: DEL <key>";
        }
        return cache.erase(std::string(key)) ? "DELETED" : "NOT_FOUND";
    }

    return "ERR unknown command '" + std::string(cmd) + "'";
}

}  // namespace meridian
