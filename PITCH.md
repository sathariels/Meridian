# How to talk about this project

Layered pitches, shortest first. Don't recite — re-derive each claim before
using it live, because the follow-up question is always one level deeper.

## The one-liner (anyone)

> "I'm building a mini version of the server system that games like Dota 2
> use to track player rankings and live matches — from scratch, in C++."

## The 30-second version (recruiter)

> "It's an in-memory cache server, like a small Redis, built for a
> game-backend scenario: thousands of players queuing and playing at once,
> and the system has to answer 'what's this player's rating' or 'who's
> online' in under a millisecond. I wrote everything by hand — no
> frameworks: the data structures, the thread-safe locking, and the TCP
> networking layer with a kqueue event loop. The in-process cache reaches
> 10 million operations a second across 8 threads. It also has consistent
> hashing, a skip-list leaderboard, append-only crash recovery, and live
> leader-to-follower replication. I'm finishing it with a small HTTP API,
> atomic snapshots, metrics, and a reproducible Linux container."

Keywords doing the work: **C++, systems programming, concurrency, from
scratch, persistence, replication, benchmarked.**

## The 2-minute version (engineer / interview)

Structure: problem → four layers → finish line.

> "The problem: a live-service game needs player state — rating, session,
> leaderboard — readable in microseconds, and a database can't do that at
> that rate. So you put a cache in front. I'm building that cache as a real
> server, phase by phase, and the rule I set for myself is that I have to
> be able to defend every design decision.
>
> **Layer 1 is the data structure**: an LRU cache where every operation is
> O(1). That takes two structures woven together — a hash map for lookup,
> and a doubly linked list threaded through the same nodes for recency
> order, which I wrote by hand rather than using std::list. Entries also
> have TTLs, expired lazily on access, and I made the clock injectable so
> expiry tests are deterministic instead of sleeping.
>
> **Layer 2 is concurrency**: instead of one global mutex, the keyspace is
> split across 16 independent stripes, each with its own lock. The
> interesting insight is that reader-writer locks *don't* help an LRU
> cache — every read moves an entry to the front of the recency list, so
> reads are writes. Striping took it from 840K to over 10 million ops/sec
> on 8 threads, hit rate unchanged, and it's ThreadSanitizer-clean.
>
> **Layer 3 is networking**: a single-threaded event loop on kqueue with
> non-blocking sockets — the C10K pattern, written raw, no Boost.Asio. It
> speaks a memcached-style text protocol, so I can literally telnet in and
> talk to it. The best bugs so far were both in connection teardown: a
> client that sends a command and immediately disconnects, and TCP
> half-close, where netcat says 'done talking' but is still listening — my
> integration tests caught both.
>
> **Layer 4 is state and recovery**: consistent hashing routes keys across
> regional shards, a span-annotated skip list answers leaderboard rank in
> O(log n), and every successful mutation is written to a human-readable
> append-only log. A follower receives the log backlog and then live writes,
> while restart recovery replays the same commands through the normal apply
> path.
>
> The finish line is deliberately narrow: Linux support, HTTP and JSON,
> atomic snapshots, operational metrics, Docker, and final integration
> benchmarks. After that I will freeze the project instead of continually
> adding unrelated features."

## Back-pocket answers

- **"Why not just use Redis?"** — "Because the point is to understand what
  Redis does under the hood. I can point at my event loop and my eviction
  path and explain every line; that's the value."
- **"Hardest bug?"** — The TCP half-close story: the netcat smoke test came
  back empty because the server treated 'done talking' as 'gone', and the
  fix had its own trap (after a FIN, the socket reads as ready forever
  under level-triggered polling — unsubscribe or spin at 100% CPU).
- **"What would you do differently at real scale?"** — Multiple event-loop
  threads (the cache is already thread-safe for it), a binary protocol
  instead of text, real cluster membership instead of the simplified
  leader-follower design.
- **"Why no learned eviction?"** — "It was an early capstone idea, but it
  would widen the project without completing its operational story. I chose
  an honest, tested LRU baseline and a clear v1 finish line instead."

## Numbers to memorize

- O(1) get / put / evict
- 838K → 5.3M → 10.3M in-process ops/sec
  (1 → 16 → 64 stripes, 8 threads, 4M ops)
- Hit rate 92.4%, unchanged by striping — per-stripe LRU costs nothing
  measurable
- A thread costs ~512KB of stack; an idle connection in the event loop
  costs ~100 bytes
