#pragma once

#include <string>

#include "cache/striped_cache.h"

namespace meridian {

// Parses one protocol line and applies it to the cache. Pure function of
// (cache, line) with no socket knowledge — deliberately, so the protocol
// is unit-testable without a running server.
//
// Protocol (newline-delimited text, memcached-style):
//   PING                        -> PONG
//   GET <key>                   -> VALUE <value> | NOT_FOUND
//   SET <key> <ttl_ms> <value>  -> OK             (ttl_ms 0 = no expiry;
//                                                  value may contain spaces)
//   DEL <key>                   -> DELETED | NOT_FOUND
//   anything else               -> ERR <reason>
std::string handle_command(StripedCache& cache, const std::string& line);

}  // namespace meridian
