# Project: Regional Matchmaking & Leaderboard Cache

## What this is
An in-memory cache system modeled loosely on how live-service games (Dota 2's
matchmaking, specifically) track player MMR, live session state, and
leaderboards at scale. This is a portfolio/resume project, not a production
system — but it should behave like a real one under simulated load.

Core scenario: thousands of simulated players are queuing, playing matches,
and updating scores concurrently. The system needs to answer "who's online"
and "what's the current top 100" with sub-millisecond reads, survive a node
dying mid-match without losing data, and demonstrate that a learned eviction
policy beats plain LRU under realistic access patterns.

## Non-goals — do not implement these
- No real consensus protocol (no Raft/Paxos). Leader-follower replication is
  intentionally simplified. If asked to make it "more correct," push back and
  explain the simplification instead of silently expanding scope.
- No embedded storage engine (no RocksDB/LevelDB/SQLite). Durability is a
  flat append-only log, replayed on restart. Flat files are the honest
  answer for this project's scope — do not upgrade this without being asked.
- No deep learning for the eviction model. Gradient-boosted trees or logistic
  regression only — small, explainable, fast at inference.
- No framework networking libraries (no Boost.Asio, no libuv). Raw sockets +
  kqueue (macOS dev) / epoll (Linux, for later validation) behind a shared
  event-loop interface, written by hand.

## Tech stack
- C++17/20, CMake (matches the existing SDL2 engine / Shadow Index projects)
- kqueue on macOS for dev, epoll on Linux for later Docker validation
- Python (sklearn or XGBoost) for offline training of the eviction model,
  weights exported and loaded into plain C++ inference at runtime — training
  and serving are not the same codebase and should not be conflated

## Architecture
1. **Network layer** — TCP server, event loop abstracted over kqueue/epoll
2. **Consistent hashing router** — maps player ID -> region shard (NA/EU/Asia),
   virtual nodes for even load distribution
3. **Per-shard cache** — hash map + intrusive doubly linked list for O(1) LRU,
   TTL expiry via timer wheel or lazy check-on-access
4. **Learned eviction model** — offline-trained, informs eviction priority
   alongside/instead of pure recency
5. **Leaderboard** — hand-built skip list, O(log n) insert/update/rank query
6. **Replication** — leader writes an append-only log; follower streams and
   replays it. Same log doubles as the durability/crash-recovery mechanism.
7. **Concurrency** — lock striping across keyspace buckets, not one global
   mutex. Load generator simulates thousands of concurrent players as the
   stress test.

## Development phases — build in this order, do not skip ahead
1. Core cache, single-threaded, no networking (get the LRU+TTL mechanics
   right and understood before adding anything else)
2. Concurrency: lock striping + load generator/stress test
3. Networking: TCP server wired to the cache
4. Consistent hashing / sharding across regions
5. Leaderboard: skip list + top-N query
6. Replication: write-ahead log, leader-follower streaming, crash recovery
7. Learned eviction: synthetic data generation -> offline training -> C++
   inference integration -> benchmark vs. plain LRU
8. Validation: Docker/Linux epoll build, full stress test
9. README + resume framing

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
