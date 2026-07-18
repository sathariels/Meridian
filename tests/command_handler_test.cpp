// Protocol parsing tests — no sockets involved, which is the point of
// keeping handle_command a pure function.

#include "server/command_handler.h"

#include "cache/striped_cache.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

void test_ping() {
    meridian::StripedCache cache(16, 2);
    assert(meridian::handle_command(cache, "PING") == "PONG");
}

void test_set_get_del_roundtrip() {
    meridian::StripedCache cache(16, 2);
    assert(meridian::handle_command(cache, "SET player:1 0 mmr=3200") ==
           "OK");
    assert(meridian::handle_command(cache, "GET player:1") ==
           "VALUE mmr=3200");
    assert(meridian::handle_command(cache, "DEL player:1") == "DELETED");
    assert(meridian::handle_command(cache, "GET player:1") == "NOT_FOUND");
    assert(meridian::handle_command(cache, "DEL player:1") == "NOT_FOUND");
}

void test_value_keeps_spaces() {
    meridian::StripedCache cache(16, 2);
    assert(meridian::handle_command(
               cache, "SET p 0 mmr=3200 region=na state=in-match") == "OK");
    assert(meridian::handle_command(cache, "GET p") ==
           "VALUE mmr=3200 region=na state=in-match");
}

void test_ttl_via_protocol() {
    int64_t now_ms = 0;
    meridian::StripedCache cache(16, 2, [&now_ms] { return now_ms; });

    assert(meridian::handle_command(cache, "SET s 500 in-match") == "OK");
    assert(meridian::handle_command(cache, "GET s") == "VALUE in-match");
    now_ms = 500;
    assert(meridian::handle_command(cache, "GET s") == "NOT_FOUND");
}

void test_errors() {
    meridian::StripedCache cache(16, 2);
    assert(meridian::handle_command(cache, "") == "ERR empty command");
    assert(meridian::handle_command(cache, "   ") == "ERR empty command");
    assert(meridian::handle_command(cache, "NUKE everything") ==
           "ERR unknown command 'NUKE'");
    assert(meridian::handle_command(cache, "GET") == "ERR usage: GET <key>");
    assert(meridian::handle_command(cache, "GET a b") ==
           "ERR usage: GET <key>");
    assert(meridian::handle_command(cache, "SET k") ==
           "ERR usage: SET <key> <ttl_ms> <value>");
    assert(meridian::handle_command(cache, "SET k -5 v") ==
           "ERR usage: SET <key> <ttl_ms> <value>");
    assert(meridian::handle_command(cache, "SET k abc v") ==
           "ERR usage: SET <key> <ttl_ms> <value>");
    // Commands are case-sensitive by design; "get" is not "GET".
    assert(meridian::handle_command(cache, "get x") ==
           "ERR unknown command 'get'");
}

void test_empty_value_is_allowed() {
    meridian::StripedCache cache(16, 2);
    assert(meridian::handle_command(cache, "SET k 0 ") == "OK");
    assert(meridian::handle_command(cache, "GET k") == "VALUE ");
}

}  // namespace

int main() {
    test_ping();
    test_set_get_del_roundtrip();
    test_value_keeps_spaces();
    test_ttl_via_protocol();
    test_errors();
    test_empty_value_is_allowed();

    std::cout << "all command_handler tests passed\n";
    return 0;
}
