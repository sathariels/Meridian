# Meridian

An in-memory cache server built from scratch in C++20, modeled on the
backend of a live-service game (think Dota 2's matchmaking): player MMR,
live session state, and leaderboards under heavy concurrent load.

No frameworks. The data structures, locking, and networking are all
hand-written — the point of the project is being able to defend every
design decision, not shipping fast. Design rationale lives in the commit
messages and [PITCH.md](PITCH.md).

## Status

| Phase | What | Status |
|-------|------|--------|
| 1 | Core LRU+TTL cache, O(1) operations | done |
| 2 | Lock-striped concurrency + load generator | done |
| 3 | TCP server, kqueue event loop, text protocol | done |
| 4 | Consistent hashing across region shards | next |
| 5 | Skip-list leaderboard | planned |
| 6 | Write-ahead-log replication, crash recovery | planned |
| 7 | Learned eviction (GBT) benchmarked vs. LRU | planned |
| 8 | Linux/epoll build + Docker validation | planned |

## Architecture so far

```
client ──TCP──▶ event loop (kqueue) ──▶ command handler ──▶ StripedCache
                single thread,                              N stripes,
                non-blocking IO                             mutex each
                                                              │
                                                        LruCache per stripe
                                                        hash map + intrusive
                                                        list, lazy TTL
```

- **LruCache** — hash map + hand-rolled intrusive doubly linked list;
  get/put/evict all O(1). TTL expiry is lazy (checked on access). The
  clock is injectable, so expiry tests are deterministic and never sleep.
- **StripedCache** — keyspace split across N independent stripes, each its
  own `LruCache` behind its own mutex. Reader-writer locks don't help
  here: every read moves an entry in the recency list, so reads are
  writes. Striping is what scales.
- **TcpServer / EventLoop** — non-blocking sockets behind a readiness
  loop (kqueue on macOS; epoll planned for Linux in phase 8). Handles
  pipelined commands, commands split across packets, backpressure, and
  TCP half-close.

## Numbers (Apple M3, 8 cores, 8 threads, 4M ops, hot-key workload)

| Stripes | Throughput | Hit rate |
|---------|-----------|----------|
| 1 (global mutex) | 838K ops/sec | 92.39% |
| 16 | 5.3M ops/sec | 92.39% |
| 64 | 10.3M ops/sec | 92.39% |

Concurrency is verified with ThreadSanitizer, not just tests that happened
to pass. Reproduce with `./build-release/load_gen --threads 8 --stripes N`.

## Build & test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer build (recommended after touching anything concurrent):

```sh
cmake -S . -B build-tsan -DMERIDIAN_SANITIZE=thread
cmake --build build-tsan
./build-tsan/striped_cache_test && ./build-tsan/tcp_server_test
```

## Run it

```sh
./build/meridian_server --port 7070 --capacity 100000 --stripes 16
```

Talk to it with netcat:

```
$ nc 127.0.0.1 7070
PING
PONG
SET player:99 0 mmr=4100 region=eu
OK
GET player:99
VALUE mmr=4100 region=eu
DEL player:99
DELETED
```

Protocol: newline-delimited text. `SET <key> <ttl_ms> <value>` (ttl 0 =
never expires, value may contain spaces), `GET <key>`, `DEL <key>`,
`PING`.

## Layout

```
src/cache/    LruCache, StripedCache
src/net/      EventLoop interface, kqueue impl, TcpServer
src/server/   protocol parsing, meridian_server main
bench/        load generator
tests/        one suite per component; tcp_server_test is a real
              socket-level integration test
```
