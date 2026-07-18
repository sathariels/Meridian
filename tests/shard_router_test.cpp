#include "cluster/shard_router.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

#include "server/command_handler.h"

namespace {

std::string key_for(int i) { return "player:" + std::to_string(i); }

void test_basic_kv_semantics_survive_routing() {
    meridian::ShardRouter router({"na", "eu", "asia"}, 1024, 4);

    router.put(key_for(1), "mmr=3000", 0);
    assert(router.get(key_for(1)) == "mmr=3000");
    assert(!router.get(key_for(2)).has_value());
    assert(router.contains(key_for(1)));
    assert(router.erase(key_for(1)));
    assert(!router.erase(key_for(1)));
}

void test_keys_actually_spread_across_shards() {
    meridian::ShardRouter router({"na", "eu", "asia"}, 100000, 4);

    std::set<std::string> shards_used;
    for (int i = 0; i < 1000; ++i) {
        router.put(key_for(i), "v", 0);
        shards_used.insert(router.shard_for(key_for(i)));
    }
    // With 1000 keys, all three shards must see traffic.
    assert(shards_used.size() == 3);
    // Nothing lost in routing: every key retrievable, total size adds up.
    for (int i = 0; i < 1000; ++i) {
        assert(router.get(key_for(i)) == "v");
    }
    assert(router.size() == 1000);
}

void test_shard_assignment_is_stable() {
    // Same shard names, fresh router -> identical assignment. This is
    // what lets a restarted node agree with the cluster about ownership.
    meridian::ShardRouter a({"na", "eu", "asia"}, 1024, 4);
    meridian::ShardRouter b({"na", "eu", "asia"}, 1024, 4);
    for (int i = 0; i < 500; ++i) {
        assert(a.shard_for(key_for(i)) == b.shard_for(key_for(i)));
    }
}

void test_ttl_survives_routing() {
    int64_t now_ms = 0;
    meridian::ShardRouter router({"na", "eu"}, 64, 2,
                                 [&now_ms] { return now_ms; });
    router.put("session", "in-match", 500);
    assert(router.get("session") == "in-match");
    now_ms = 500;
    assert(!router.get("session").has_value());
}

void test_shard_command_over_protocol() {
    meridian::ShardRouter router({"na", "eu", "asia"}, 1024, 4);

    std::string reply = meridian::handle_command(router, "SHARD player:1");
    assert(reply == "SHARD na" || reply == "SHARD eu" ||
           reply == "SHARD asia");
    // And it reports the same shard the router actually uses.
    assert(reply == "SHARD " + router.shard_for("player:1"));

    // Non-topology commands still work through the router overload.
    assert(meridian::handle_command(router, "SET k 0 v") == "OK");
    assert(meridian::handle_command(router, "GET k") == "VALUE v");
    assert(meridian::handle_command(router, "SHARD") ==
           "ERR usage: SHARD <key>");
}

}  // namespace

int main() {
    test_basic_kv_semantics_survive_routing();
    test_keys_actually_spread_across_shards();
    test_shard_assignment_is_stable();
    test_ttl_survives_routing();
    test_shard_command_over_protocol();

    std::cout << "all shard_router tests passed\n";
    return 0;
}
