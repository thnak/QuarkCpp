# Quark Engine

**A high-performance C++23 actor engine for building highly concurrent, distributed systems.**
The runtime owns optimization; developers express only intent.

Quark gives you actors — units of state and sequential behavior addressed by id — with a
zero-cost, header-first C++23 core ([`include/quark/`](https://github.com/thnak/QuarkCpp/tree/master/include/quark)):
a work-stealing scheduler, hybrid sync/async handlers, point-to-point and streaming messaging,
cluster distribution to 10³–10⁴ nodes, durable persistence, and failure supervision, all
expressed through compile-time CRTP policies instead of runtime configuration.

It's backed by a **153-test** correctness suite ([`tests/`](https://github.com/thnak/QuarkCpp/tree/master/tests))
verified clean under ASan/UBSan/TSan, a benchmark harness ([`bench/`](https://github.com/thnak/QuarkCpp/tree/master/bench))
that turns every hot-path performance claim into a pass/fail gate, and **16 runnable samples**
([`samples/`](https://github.com/thnak/QuarkCpp/tree/master/samples), see [Samples](Samples)) from
a single local actor to multi-node TCP clusters. Every subsystem is backed by a written design
and, where it's hot-path or safety-critical, by an executed proof — see
[Design & verification docs](#design--verification-docs) below.

## Status

Every subsystem — actors, scheduler, messaging, streaming, clustering, persistence,
supervision, security — is **implemented and covered by the test suite**, and the whole engine
is **verified clean under ASan, UBSan, and TSan** on every push. **Linux/x86-64 is the primary
supported and verified target today**; ARM64 already runs the full correctness matrix in CI on
real hardware, and Windows/macOS are designed-for behind the existing Platform Abstraction Layer
seam — extending support means filling in a PAL backend, not redesigning the engine.

See **[Project Status](Project-Status)** for the full per-subsystem ADR-by-ADR narrative.

## Features

- **Header-first, std-only C++23 core** — `std::expected` results, coroutine async handlers,
  `std::stop_token` cancellation, `std::pmr` shard allocators, concepts + deducing-this. No
  RTTI/reflection on the hot path.
- **Hybrid handler execution** — synchronous by default (zero-cost, drained inline); an actor
  opts into coroutine handlers (`quark::task<>`) per message type for async I/O.
- **Zero-cost intent declaration** — CRTP policy types (`Sequential`, `Priority<P>`,
  `Placement<…>`, …) as template parameters, resolved to metadata at startup. No attributes, no
  reflection.
- **Work-stealing scheduler** with priority bands and per-actor mailbox FIFO ordering.
- **Point-to-point and fan-out messaging** — `tell`/`ask`, credit-controlled streaming replies
  (`ask_stream`), best-effort at-most-once broadcast (`Topic<M>`).
- **Inbound stream ingestion** — per-stream credit-ring, zero-copy, backpressure instead of
  shedding.
- **Cluster distribution at scale** — HRW/VirtualBins placement, SWIM membership, bounded
  partial-view + DHT-relay for 10³–10⁴-node topologies.
- **Durable persistence and reminders** — snapshot & event-sourced durability, at-least-once
  wall-clock scheduled wake-ups that flatten mass-due waves to a steady drain rate.
- **Failure supervision** — zero-cost guarded handler core, restart/resume/stop/escalate
  policies.
- **Resource governance** — rate limiting, deadline-aware load shedding, circuit breaking.
- **Deterministic simulation testing** (014) for fault injection without real time or threads.
- **Cross-platform by design** via a thin Platform Abstraction Layer (PAL); verified today on
  Linux/x86-64, CI-covered on Linux/ARM64.

## Quick start

Requires CMake ≥ 3.24 and a C++23 compiler (verified: **g++ 14.2**, **clang 20.1**).

```bash
# Build + run the full correctness gate (Release)
cmake -S . -B build
cmake --build build -j4                          # -j4, never -j$(nproc) — see note
ctest --test-dir build -j4 --output-on-failure   # 153 / 153

# Sanitizers (the same suite, minus by-design exclusions — counts in VERIFICATION.md)
cmake -S . -B build-asan -DQUARK_SANITIZE="address;undefined"   # ASan + UBSan
cmake -S . -B build-tsan -DQUARK_SANITIZE="thread"              # ThreadSanitizer (build -j1)

# Benchmarks (default ON) and the runnable samples (default OFF)
cmake -S . -B build -DQUARK_BUILD_SAMPLES=ON
taskset -c 0-3 build/samples/01_hello_counter    # prints OK / exit 0

# Opt-in persistence backends (off by default; std-only core needs neither)
cmake -S . -B build -DQUARK_WITH_SQLITE=ON -DQUARK_WITH_ROCKSDB=ON
```

The run-and-result record for correctness (test counts, sanitizer deltas, reproduce steps) is
**[VERIFICATION](VERIFICATION)**; measured speed against the 023 budgets is
**[PERFORMANCE](PERFORMANCE)**.

> **Machine-safety note.** A build/run that saturates all cores can hang or power off a
> constrained dev box. Build with `-j4` (the TSan build with `-j1`), and run binaries under
> `taskset -c 0-3` — **never** `-j$(nproc)`.

## Usage

The smallest complete Quark program: one actor, driven by `tell` (fire-and-forget) and `ask`
(request/reply) over the real engine. See **[How To: Write Your First Actor](How-To-Write-Your-First-Actor)**
for the full step-by-step walkthrough, or [`samples/01_hello_counter`](https://github.com/thnak/QuarkCpp/blob/master/samples/01_hello_counter/main.cpp)
for the runnable source.

```cpp
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

struct Add { int amount; };
struct GetTotal {};

// Policies in the CRTP base ARE the actor's metadata (band, budget, reentrancy).
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    void handle(const Add& a) noexcept { total_ += a.amount; }
    void handle(const Ask<GetTotal, int>& m) noexcept { m.respond(total_); }

private:
    int total_ = 0;
};

int main() {
    detail::MessagePool pool(1024);
    Counter counter;
    auto activation = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    register_actor<Counter>(eng, /*key*/ 42, *activation);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> counter_ref = router.get<Counter>(42);
    eng.start();

    for (int i = 1; i <= 100; ++i) counter_ref.tell(Add{i});         // fire-and-forget
    result<int> total = block_on(counter_ref.ask<int>(GetTotal{}));  // request/reply

    eng.stop();
}
```

For more, browse **[Samples](Samples)** — 16 runnable programs from a single actor up to
multi-node TCP clusters.

## Performance (measured)

Every hot-path performance claim is a **verdict a benchmark prints**, not an assertion —
[`bench/`](https://github.com/thnak/QuarkCpp/tree/master/bench) checks each one against the 023
budget table, and the full code-and-result report lives in **[PERFORMANCE](PERFORMANCE)**
(machine-of-record, per-feature code + numbers, reproduce steps; see also **[Benchmarks](Benchmarks)**
for the harness itself). Headline figures — **release + `-march=native`, single core pinned** on
a *virtualized Xeon Silver 4208 @ 2.1 GHz* (a modest reference machine, so these are regression
tripwires, not a best-case stamp):

| Feature (spec) | Metric | Measured | 023 budget | |
|---|---|---|---|---|
| `tell` — mailbox (003) | enqueue→dequeue p50 | **59 ns** | ≤ 100 ns | `[goal]` |
| `tell` — scheduler (002) | full-lifecycle throughput | **11.0 M/s** | ≥ 10 M/s | `[goal]` |
| priority (002) | `UniformFIFO` vs raw MPSC | **+0.45 ns** | within noise | `[free]` |
| `ask` (006) | engine-overhead p50 / p99 | **147 / 226 ns** | p50 ≤ 1 µs | `[goal]` |
| streaming (024) | sustained ingest / per-frame | **140.8 M/s / 7.1 ns** | ≥ 10 M/s / ≤ 100 ns | `[goal]` |
| streaming (024) | ingest vs discrete `tell` | **5.0× cheaper** | ≥ 3× | `[goal]` |
| activate/deactivate (001) | cold activation p50 / cycle | **111 ns / 14.8 M/s** | ≤ 10 µs / ≥ 10 M/s | `[goal]` |
| idle density (003) | activations / GB | **1.95 M/GB** | ≥ 1 M/GB | `[goal]` |
| serialize (016) | tagless wire encode p99 | **50 ns** | ≤ 200 ns | `[goal]` |
| placement (010/026) | VirtualBins lookup, N-indep. | **12.5 ns (0.99×)** | ≤ 20 ns | `[goal]` |
| supervision (007) | guarded vs no-guard success path | **~1.0×** | ≤ noise | `[free]` |

The machine-independent **invariant** gates — descriptor ≤ 64 B, **0 hot-path allocations**,
**0 cross-core RMW on the drain path**, and the objdump zero-cost parity checks — are pass/fail
CTest gates in [`tests/`](https://github.com/thnak/QuarkCpp/tree/master/tests), not
noise-sensitive benchmarks; see PERFORMANCE.md §"What this document is not".

## Repository layout

```
include/quark/core/     header-first engine core (hot path lives in headers)
include/quark/net/      default TCP transport + wire codec (010/019/021)
include/quark/adapters/ opt-in persistence/reminder backends (SQLite, RocksDB)
include/quark/detail/   internals (message pool, reply cell, hashing)
pal/                    Platform Abstraction Layer — the single OS seam (019)
src/                    non-template translation units
tests/                  153-test correctness gate (CTest)
bench/                  benchmark harness — the 023 budget verdicts
samples/                16 runnable programs over the public developer surface
decisions/              ADRs — the design → red-team → prove → judge records
NNN-*.md                the 27 RFC specification documents
```

The core is **std-only C++23** and **header-first** — the hot path (mailbox, scheduler,
dispatch, streams) lives in [`include/quark/core/`](https://github.com/thnak/QuarkCpp/tree/master/include/quark/core);
`src/` holds only non-template units.

## Design & verification docs

Quark's implementation is backed by a full written design and, for every hot-path or
safety-critical choice, an executed proof rather than argument. If you're **building a product
with Quark**, the [glossary](#glossary) below and the [Architecture Overview](ActorEngineSpecification)
are usually all the design context you need. If you're **working on Quark itself**, the full RFC
spec set, the ADR proof record, and the process docs live on **[Contributing to Quark](Contributing)**.

A few decisions are locked project-wide: **C++23** (no RTTI/reflection on the hot path), a
**hybrid handler model** (sync by default, opt-in `quark::task<>` coroutines for async I/O),
**CRTP policy types** for zero-cost intent declaration (no attributes, no reflection), and a
**cross-platform target** (Linux/Windows/macOS, x86-64 + ARM64) behind the PAL — with
**Linux/x86-64 as the currently supported and verified target**. There is no .NET /
managed-runtime vocabulary anywhere in the design — see the glossary below.

### Glossary

<details>
<summary><strong>The vocabulary this project uses</strong></summary>

| Term | Meaning | (Replaces the managed-runtime notion of) |
|---|---|---|
| **Actor** | Unit of state + sequential behavior, addressed by id | actor / grain |
| **Activation** | The *right to execute* an actor; at most one exists per actor | grain activation |
| **Worker** | A transient execution lane (thread) that borrows activations | thread-pool thread |
| **Shard** | Owner of activation queues, an allocator, and metrics; selected by `ActorId → hash` | partition |
| **Mailbox** | Intrusive Vyukov MPSC queue owning message *ordering only* (FIFO); the queue node *is* the descriptor (003, ADR-002) | mailbox |
| **MessageHandle / Descriptor / Payload** | `{Descriptor*, generation}` handle → fixed-size metadata (intrusively linked) → separately-stored payload | message envelope |
| **Policy** | A CRTP template parameter expressing intent (`Sequential`, `Placement<…>`, …) | attribute |
| **Resource** | A dependency with a lifetime scope, resolved at activation or by factory | injected service |
| **MessageContext** | Ambient per-message values: `std::stop_token`, deadline, trace id, headers | cancellation token / ambient scope |
| **`quark::task<>`** | The coroutine return type for async handlers | `Task` |
| **`ActorRef<A>`** | Typed handle used to `tell`/`ask` an actor | typed grain reference |

</details>

## Dependency posture

The engine **core is std-only** (C++23), with all OS-specific facilities behind a thin
**Platform Abstraction Layer** (Linux/Windows/macOS backends — sockets + event loop, durable
file flush, thread affinity/NUMA). Every subsystem that would otherwise pull a heavy dependency
is expressed as a **seam** with a self-contained default, and heavier backends are optional
adapters that are never linked into a minimal build:

| Subsystem | Std-only default | Optional adapter (behind a seam) |
|---|---|---|
| Transport (010) | TCP + length-prefixed frames; per-OS event loop (epoll/io_uring · kqueue · IOCP) via the PAL | gRPC/QUIC/RDMA |
| Serialization (016) | canonical tagged TLV from one `QUARK_SERIALIZE` per type + negotiated tagless wire fast path | protobuf / FlatBuffers / Cap'n Proto |
| Membership (010) | in-house SWIM gossip | etcd / Consul |
| Persistence (012) | `InMemoryStore` (reference) + `FileStore` (append-only WAL + `fdatasync`, crash-durable) — see [PersistenceAdapters](PersistenceAdapters) | `SqliteStore` / `RocksStore` (opt-in: `QUARK_WITH_SQLITE`/`QUARK_WITH_ROCKSDB`); Postgres/object-store behind the same `Store` seam |
| Metrics/Trace (009) | snapshot API + Prometheus text | OpenTelemetry / OTLP |
| Config (013) | programmatic `EngineConfig` + env vars | TOML/JSON file loader |
| Governance (022) | per-node token-bucket rate limits + bounded queues + circuit breakers | distributed exact-limit coordinator (Redis/etcd) |
| Benchmark harness (023) | in-house timing loop over the PAL clock (dev tooling) | google-benchmark (dev-only, never linked) |
| Inbound streaming (024) | pre-allocated per-stream SPSC credit-ring + shard-`pmr` arena; copy into inline slots | transport-registered zero-copy RX buffers (io_uring/RDMA) via the PAL |
| Large-scale topology (026) | in-house VirtualBins + bounded partial-view (SWIM) + Kademlia relay, coordinator-free | external coordinator (etcd/Consul) behind the `Membership` seam |

Remaining cross-cutting design questions are tracked in [OpenQuestions](OpenQuestions).

## Contributing

Want to work on Quark itself rather than build a product with it? Start at
**[Contributing to Quark](Contributing)** — the RFC specs, the ADR proof record, and the
process docs that govern a change to this repository.

## License

[MIT](https://github.com/thnak/QuarkCpp/blob/master/LICENSE)
