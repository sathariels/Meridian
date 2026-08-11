# Meridian v1 Roadmap

This document defines the remaining scope for Meridian v1. Work proceeds one
phase at a time, with a review after each phase. Features outside this document
are deferred until after v1 and should not be added incidentally.

## Baseline

The v1 finish work starts from commit `8c5e2e1`.

Already implemented:

- O(1) LRU cache with lazy per-entry TTL expiration.
- Lock-striped concurrency and an in-process load generator.
- Non-blocking TCP server with a kqueue event loop and text protocol.
- Consistent hashing across three in-process shards.
- Span-annotated skip-list leaderboard.
- Append-only WAL recovery and leader-to-follower replication.
- Bounded WAL backlog streaming with TCP backpressure.

The repository defines eight test executables containing 52 named test
functions. They cover cache semantics, deterministic TTL behavior, concurrent
cache access, socket framing, sharding, leaderboard correctness, WAL recovery,
torn WAL tails, and leader/follower replication.

This baseline records the test inventory rather than claiming a fresh pass:
the current Windows workspace cannot build the POSIX server, and Linux support
is incomplete until Phase 1 adds epoll. Linux CI becomes the authoritative
test result once that phase lands.

The published performance baseline is an in-process cache benchmark on an
Apple M3 with eight worker threads and four million operations:

| Stripes | Throughput | Hit rate |
|---------|------------|----------|
| 1 | 838K ops/sec | 92.39% |
| 16 | 5.3M ops/sec | 92.39% |
| 64 | 10.3M ops/sec | 92.39% |

These are cache-operation results, not network or HTTP throughput. HTTP
benchmarks will be recorded separately in Phase 8.

Known gaps at the baseline:

- Linux builds reference an epoll source file that does not exist yet.
- The TCP server is coupled to newline-delimited protocol framing.
- There is no HTTP or JSON interface.
- WAL replay restores relative TTL durations instead of original deadlines.
- There are no state snapshots, health endpoint, metrics, Docker image, or CI.
- Learned eviction is not implemented and is outside the Meridian v1 scope.

## V1 Interface Contract

The existing text protocol remains available on the internal TCP port for
replication, debugging, and compatibility. HTTP runs on a separate configurable
port and exposes the public cache API.

### Cache API

`GET /v1/cache/{key}`

- `200 OK`: `{"key":"player:1","value":"..."}`
- `404 Not Found`: key is missing or expired.

`PUT /v1/cache/{key}`

- Request: `{"value":"...","ttl_ms":5000}`
- `ttl_ms` is optional and defaults to `0`, meaning no expiration.
- Values are strings; arbitrary JSON documents are not stored as native types.
- Success returns `204 No Content`.

`DELETE /v1/cache/{key}`

- `204 No Content`: key was removed.
- `404 Not Found`: key did not exist.

Errors use one stable JSON shape:

```json
{"error":{"code":"invalid_json","message":"request body is not valid JSON"}}
```

The HTTP implementation supports the required HTTP/1.1 subset: persistent
connections, pipelined requests, `Content-Length`, request fragmentation, and
percent-decoded keys. It enforces bounded headers, bodies, and keys. Ambiguous
framing, chunked request bodies, unsupported methods, and unsupported media
types are rejected explicitly.

### Operational API

- `GET /health` returns JSON readiness and role information.
- `GET /metrics` returns Prometheus text-format metrics.

### Persistence Contract

- The WAL remains the record of mutations between snapshots.
- Durable TTL records store absolute deadlines, so downtime does not restart a
  TTL countdown.
- A snapshot represents cache and leaderboard state at a precise WAL offset.
- Snapshots are versioned, checksummed, and installed with an atomic rename.
- Recovery loads the latest valid snapshot and then replays the WAL suffix.
- A partial or corrupt new snapshot never replaces the last valid snapshot.

## Implementation Phases

### Phase 0: Scope and Baseline

- Define the v1 API, guarantees, non-goals, baseline, and freeze criteria.
- Correct stale or unsupported claims in project documentation.

Status: complete (2026-08-11).

### Phase 1: Linux and Build Support

- Implement the epoll event-loop backend.
- Add CMake presets for Debug, Release, ASan/UBSan, and TSan.
- Add Linux CI that builds and runs CTest.

### Phase 2: Networking Boundaries

- Separate byte-oriented TCP transport from protocol framing.
- Rebuild the text protocol as an adapter without changing its behavior.
- Introduce typed server operations shared by text and HTTP adapters.

### Phase 3: HTTP REST API

- Implement the cache and JSON contract above on a separate HTTP port.
- Add strict parsing, size limits, status mapping, and socket integration tests.

### Phase 4: Durable TTL Semantics

- Persist absolute expiration deadlines in WAL and replication records.
- Skip already-expired data during recovery.
- Preserve compatibility with legacy WAL records where practical.

### Phase 5: Snapshots and Recovery

- Export and restore cache, LRU, expiration, and leaderboard state.
- Write versioned atomic snapshots and recover from snapshot plus WAL suffix.
- Test corruption, interrupted writes, and expiration across downtime.

### Phase 6: Metrics and Health

- Instrument requests, errors, cache outcomes, evictions, expirations,
  connections, WAL activity, snapshots, and replication.
- Expose `/metrics` and `/health` without high-cardinality labels.

### Phase 7: Docker Packaging

- Add a multi-stage Linux image, non-root runtime, persistent `/data` volume,
  health check, graceful shutdown, and one-command Compose startup.

### Phase 8: Final Validation and Benchmarks

- Add malformed-input, concurrency, snapshot, restart, and endurance coverage.
- Run CI, sanitizers, Docker smoke tests, and HTTP latency/throughput benchmarks.
- Record commands, hardware, workload, and median results.

### Phase 9: Documentation and Freeze

- Update architecture diagrams, API reference, operational instructions,
  persistence guarantees, benchmark results, README, and project pitch.
- Tag `v1.0.0` after the acceptance criteria pass.

## Explicit Non-Goals

- Consensus, automatic failover, elections, or dynamic cluster membership.
- TLS termination, authentication, authorization, or a public internet threat
  model. Deployments should place Meridian behind an appropriate proxy.
- HTTP/2, HTTP/3, chunked request bodies, WebSockets, or full RFC coverage.
- An embedded database or storage engine.
- Active background TTL sweeping; expiration remains lazy.
- Arbitrary JSON document semantics or secondary querying.
- A networking framework such as Boost.Asio or libuv.
- Learned eviction in Meridian v1.

## Freeze Criteria

Meridian v1 is complete when all of the following are true:

- Linux CI and required sanitizer jobs pass.
- The documented HTTP and operational endpoints pass integration tests.
- TTL deadlines remain correct across restart and replication.
- Atomic snapshot plus WAL-suffix recovery passes failure-path tests.
- `docker compose up --build` starts a healthy persistent instance.
- HTTP benchmarks are reproducible and clearly separated from cache benchmarks.
- README, architecture documentation, and project pitch match the shipped code.
- No known critical or high-severity correctness defects remain.

After the `v1.0.0` tag, Meridian receives correctness, security, portability,
and documentation fixes only. New product features belong in a different
project or a separately approved post-v1 roadmap.
