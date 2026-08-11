# Project: Regional Matchmaking & Leaderboard Cache

## What this is
An in-memory cache system modeled loosely on how live-service games (Dota 2's
matchmaking, specifically) track player MMR, live session state, and
leaderboards at scale. This is a portfolio/resume project, not a production
system — but it should behave like a real one under simulated load.

Core scenario: thousands of simulated players are queuing, playing matches,
and updating scores concurrently. The system needs to answer "who's online"
and "what's the current top 100" with sub-millisecond reads, survive a node
dying mid-match without losing durable data, and expose enough operational
surface to run and evaluate the system outside a developer checkout.

The original learned-eviction capstone is not implemented and is deferred
outside Meridian v1. The authoritative v1 finish plan is ROADMAP.md: complete
the operational story, freeze the project, and move to the next major project.

## Non-goals — do not implement these
- No real consensus protocol (no Raft/Paxos). Leader-follower replication is
  intentionally simplified. If asked to make it "more correct," push back and
  explain the simplification instead of silently expanding scope.
- No embedded storage engine (no RocksDB/LevelDB/SQLite). Durability is a
  project-owned flat WAL plus atomic snapshot files. Flat files are the honest
  answer for this project's scope — do not upgrade this without being asked.
- No framework networking libraries (no Boost.Asio, no libuv). Raw sockets +
  kqueue (macOS) / epoll (Linux) behind a shared
  event-loop interface, written by hand.
- No automatic failover, dynamic cluster membership, TLS termination,
  authentication, HTTP/2, or HTTP/3 in v1.
- No active background TTL sweeper. Lazy expiration is an intentional design.
- No learned eviction in v1. Do not restore the old phase based only on stale
  documentation or the earlier README status claim.

## Tech stack
- C++20 and CMake
- kqueue on macOS, epoll on Linux, selected behind one event-loop interface
- A pinned JSON parser is allowed; networking frameworks are not

## Architecture
1. **Network layer** — TCP server, event loop abstracted over kqueue/epoll
2. **Consistent hashing router** — maps player ID -> region shard (NA/EU/Asia),
   virtual nodes for even load distribution
3. **Per-shard cache** — hash map + intrusive doubly linked list for O(1) LRU,
   TTL expiry via lazy check-on-access
4. **Leaderboard** — hand-built skip list, O(log n) insert/update/rank query
5. **Replication** — leader writes an append-only log; follower streams and
   replays it. Same log doubles as the durability/crash-recovery mechanism.
6. **Concurrency** — lock striping across keyspace buckets, not one global
   mutex. Load generator simulates thousands of concurrent players as the
   stress test.
7. **Public interface** — a bounded HTTP/1.1 and JSON cache API
8. **Operations** — snapshots, durable TTLs, metrics, health, and Docker

## V1 finish phases — build in this order, do not skip ahead
0. Scope and baseline documentation
1. Linux epoll backend, build presets, and CI
2. Byte-oriented TCP transport and shared typed server operations
3. HTTP/1.1 REST and JSON cache API
4. Durable TTL deadlines across recovery and replication
5. Atomic snapshots plus WAL-suffix recovery
6. Prometheus metrics and JSON health endpoint
7. Docker and one-command Compose startup
8. Integration, sanitizer, endurance, and HTTP benchmark validation
9. Final documentation and v1 freeze

The exact contracts, acceptance criteria, and freeze rules live in ROADMAP.md.

## Working agreement — this is the important part
The point of this project is that I (the human) can defend every design
decision in a live interview. Optimizing for "working code fast" over "code
I understand" defeats the purpose.

- After drafting any non-trivial module, stop and explain in plain language
  *why* this approach was chosen over the obvious alternative (e.g., why a
  skip list over a sorted vector, why lock striping over a global mutex).
- Do not silently chain multiple phases together in one pass. One phase,
  then pause for review, then proceed.
- Comment the tricky parts inline — not restating what the code does, but
  why it does it that way.
- If I ask you to add something outside the phase currently in progress,
  flag that it's scope creep before doing it.

## Code style
- Match the CMake structure and conventions from the existing SDL2
  engine project
- Prefer explicit, readable code over clever one-liners — this needs to be
  explainable out loud
