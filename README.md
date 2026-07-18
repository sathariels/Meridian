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
| 4 | Consistent hashing across region shards | done |
| 5 | Skip-list leaderboard | done |
| 6 | Write-ahead-log replication, crash recovery | done |
| 7 | Learned eviction (GBT) benchmarked vs. LRU | next |
| 8 | Linux/epoll build + Docker validation | planned |

## Architecture so far

```
client ──TCP──▶ event loop (kqueue) ──▶ command handler ──▶ ShardRouter
                single thread,              │               hash ring,
                non-blocking IO             ▼               128 vnodes/shard
                                       Leaderboard
                                       (skip list, span links)
                                                              │
                                                 ┌────────────┼────────────┐
                                                 ▼            ▼            ▼
                                             na shard     eu shard    asia shard
                                            StripedCache StripedCache StripedCache
                                             N stripes, mutex each
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
- **HashRing / ShardRouter** — consistent hashing with 128 virtual nodes
  per shard routes each key to a region shard (na/eu/asia). Removing a
  node remaps only that node's ~1/N of the keyspace — keys on surviving
  shards provably don't move (`hash_ring_test` asserts this exactly, not
  statistically). `SHARD <key>` on the wire shows the routing live.
- **Leaderboard** — hand-built skip list with span-annotated links (the
  Redis sorted-set design): O(log n) insert/re-score/remove *and* O(log n)
  rank queries, O(n) top-N walk. Verified against a brute-force reference
  model over 20K randomized ops — every rank exact. Single-writer by
  design (event-loop thread owns it).
- **Wal / ReplicaLink** — durability and replication share one mechanism:
  every successful mutation is appended to a flat append-only log *as its
  protocol line* (`cat` the WAL and you can read it). Crash recovery
  replays the log through the same command handler that executed it live;
  a torn final line from a mid-write crash is detected and truncated on
  reopen. A follower sends `SYNC`, receives the full backlog, then the
  live mutation stream; it keeps its own WAL and rejects client writes.
  Tested with real kill-and-rebuild cycles, including a torn-tail crash
  simulation. No consensus/failover by design — see CLAUDE.md non-goals.

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

With durability, plus a read-only follower replicating from it:

```sh
./build/meridian_server --wal leader.wal
./build/meridian_server --port 7071 --wal follower.wal --replica-of 127.0.0.1:7070
```

Kill the leader with `kill -9`, restart it with the same `--wal`, and it
replays the log ("recovered N ops") with all data intact. `--fsync` makes
every write hit disk before being acknowledged (slower, stricter).

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

Protocol also supports:

- `SHARD <key>` → which region shard owns the key
- `SCORE <player> <score>` / `RANK <player>` / `TOP <n>` /
  `UNRANK <player>` → the leaderboard

```
$ nc 127.0.0.1 7070
SCORE alice 3200
OK
SCORE cara 4100
OK
TOP 2
TOP cara:4100 alice:3200
RANK alice
RANK 2 3200
```

```
src/cache/       KvStore interface, LruCache, StripedCache
src/cluster/     HashRing, ShardRouter
src/common/      hash utilities (FNV-1a, splitmix64)
src/leaderboard/ skip-list Leaderboard
src/net/         EventLoop interface, kqueue impl, TcpServer
src/replication/ Wal (append-only log), ReplicaLink (follower side)
src/server/      protocol parsing, meridian_server main
bench/           load generator
tests/           one suite per component; tcp_server_test is a real
                 socket-level integration test
```
