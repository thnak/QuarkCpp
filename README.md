# Quark Engine

**A high-performance C++23 actor engine for building highly concurrent, distributed systems.**
The runtime owns optimization; developers express only intent.

[![CI](https://github.com/thnak/QuarkCpp/actions/workflows/ci.yml/badge.svg)](https://github.com/thnak/QuarkCpp/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)](CONVENTIONS.md#target--scope)

Quark gives you actors — units of state and sequential behavior addressed by id — with a
zero-cost, header-first C++23 core ([`include/quark/`](include/quark)): a work-stealing
scheduler, hybrid sync/async handlers, point-to-point and streaming messaging, cluster
distribution to 10³–10⁴ nodes, durable persistence, and failure supervision, all expressed
through compile-time CRTP policies instead of runtime configuration.

It's backed by a **153-test** correctness suite ([`tests/`](tests)) verified clean under
ASan/UBSan/TSan, a benchmark harness ([`bench/`](bench)) that turns every hot-path performance
claim into a pass/fail gate, and **16 runnable samples** ([`samples/`](samples)) from a single
local actor to multi-node TCP clusters. Every subsystem is backed by a written design and, where
it's hot-path or safety-critical, by an executed proof — see
[Design & verification docs](#design--verification-docs).

## Table of contents

- [Status](#status)
- [Features](#features)
- [Quick start](#quick-start)
- [Usage](#usage)
- [Performance](#performance-measured)
- [Repository layout](#repository-layout)
- [Design & verification docs](#design--verification-docs)
- [Dependency posture](#dependency-posture)
- [Contributing](#contributing)
- [License](#license)

## Status

Every subsystem — actors, scheduler, messaging, streaming, clustering, persistence,
supervision, security — is **implemented and covered by the test suite**, and the whole engine
is **verified clean under ASan, UBSan, and TSan** on every push. **Linux/x86-64 is the primary
supported and verified target today**; ARM64 already runs the full correctness matrix in CI on
real hardware, and Windows/macOS are designed-for behind the existing Platform Abstraction Layer
seam — extending support means filling in a PAL backend, not redesigning the engine.

<details>
<summary>Full status detail (per-subsystem gating, ADR references)</summary>

> The mailbox hot path (001, 002, 003, 015) is proven under
> [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md); the developer-facing surface
> (004, 005, 006, 008, 013) under
> [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)/[008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md)/[010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md);
> and the surrounding subsystems (009, 011, 012, 014, 017, 018, 020, 021, 022, 027) are Accepted
> as **design-settled with their load-bearing mechanisms proven by the cited ADRs** and their
> open questions resolved or deferred. **007 is Accepted (x86-64, core)** via
> [ADR-009](decisions/ADR-009-failure-supervision-and-recovery-policy-model.md) — the D1
> zero-cost guard core is proven while the grafted `Supervision<Tree>` / `OnRestartAsk<Retry>`
> / `OnResourceFailure<Degrade>` knobs stay Draft pending re-gating against that guard.
>
> The cluster data path is now proven too:
> [ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md) executed the
> named gates — **FIFO-under-relay CORRECT** promotes **026 → Accepted** and **010 → Accepted
> (x86-64, core)** (its cross-node backpressure design question is the named residual), and the
> **Stateless<N> pool** is proven exactly-once + faster than hand-rolled. **025 is now Accepted
> (x86-64)** too: its Weighted-HRW distribution gate, after ADR-011 caught a real ADR-006
> formula/threshold defect and ADR-012 exposed a mis-specified band, was re-gated **CORRECT**
> under a like-for-like preregistered p99-vs-p99 band against an independent multinomial
> ([ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md)) — both halves (Stateless
> + Weighted-distribution) now proven.
>
> **027 (Reminders)** is newly **Accepted (x86-64)** via
> [ADR-017](decisions/ADR-017-durable-reminder-mass-due-scale-gate.md): a `design → debate → prove`
> loop ran 3 competing designs through red-team + executed C++23, and the winning **SEGSTREAM**
> durable-reminder design proves the mass-due gate — a 10⁶-at-9 PM wave flattens to `peak == fire_rate`
> (re-measured from a clean build here) with zero committed-reminder loss across crash
> ([`reminder_service.hpp`](include/quark/core/reminder_service.hpp) + test + bench + sample 14).
>
> **006 (Messaging)** grew two proven fan-out axes on top of point-to-point `tell`/`ask`:
> **outbound streaming replies** — an `ask_stream` that returns a bounded, credit-controlled
> reply stream (the 024 inbound ring with producer/consumer roles flipped, *no shared counter*)
> via [ADR-018](decisions/ADR-018-outbound-streaming-replies.md)
> ([`reply_stream.hpp`](include/quark/core/reply_stream.hpp)); and a **best-effort at-most-once
> broadcast** primitive `Topic<M>` — one immutable refcounted payload fanned as N thin descriptors
> onto each subscriber's verbatim ADR-002 mailbox, publisher never stalls, slow/dead subscribers
> dropped-and-counted — via [ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)
> ([`topic.hpp`](include/quark/core/topic.hpp), **Accepted (x86-64) for local fan-out**, cross-node
> Draft on GATE 7).
>
> **Still Draft (2):** **019 (PAL)** and **023 (budgets)** are hardware-blocked — the PAL's
> whole point is the multi-OS/ARM64 backends, and 023's numbers are provisional pending a
> Zen4/SPR reference re-baseline and the ARM64 ratio. *Accepted (x86-64)* means the design is
> settled and backed on the current support target; ARM64 promotion waits on the weak-memory
> proof ([OpenQuestions.md](OpenQuestions.md)).

</details>

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
**[VERIFICATION.md](VERIFICATION.md)**; measured speed against the 023 budgets is
**[PERFORMANCE.md](PERFORMANCE.md)**.

> **Machine-safety note.** A build/run that saturates all cores can hang or power off a
> constrained dev box. Build with `-j4` (the TSan build with `-j1`), and run binaries under
> `taskset -c 0-3` — **never** `-j$(nproc)`.

## Usage

The smallest complete Quark program: one actor, driven by `tell` (fire-and-forget) and `ask`
(request/reply) over the real engine. See [`samples/01_hello_counter`](samples/01_hello_counter)
for the full, buildable version.

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

## Performance (measured)

Every hot-path performance claim is a **verdict a benchmark prints**, not an assertion —
[`bench/`](bench/) checks each one against the 023 budget table, and the full code-and-result
report lives in **[PERFORMANCE.md](PERFORMANCE.md)** (machine-of-record, per-feature code +
numbers, reproduce steps). Headline figures — **release + `-march=native`, single core pinned**
on a *virtualized Xeon Silver 4208 @ 2.1 GHz* (a modest reference machine, so these are
regression tripwires, not a best-case stamp):

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
CTest gates in [`tests/`](tests/), not noise-sensitive benchmarks; see PERFORMANCE.md
§"What this document is not".

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
dispatch, streams) lives in [`include/quark/core/`](include/quark/core); `src/` holds only
non-template units.

## Design & verification docs

Quark's implementation is backed by a full written design and, for every hot-path or
safety-critical choice, an executed proof rather than argument. Start here if you want the
rationale behind an API or a guarantee:

- **[ActorEngineSpecification.md](ActorEngineSpecification.md)** — vision, principles, core
  invariants, glossary.
- **[CONVENTIONS.md](CONVENTIONS.md)** — the coding contract every change must follow.
- **[VERIFICATION.md](VERIFICATION.md)** — correctness record (test counts, sanitizer deltas).
- **[PERFORMANCE.md](PERFORMANCE.md)** — full benchmark report and reproduce steps.
- **[OpenQuestions.md](OpenQuestions.md)** — remaining cross-cutting design questions.

A few decisions are locked project-wide: **C++23** (no RTTI/reflection on the hot path), a
**hybrid handler model** (sync by default, opt-in `quark::task<>` coroutines for async I/O),
**CRTP policy types** for zero-cost intent declaration (no attributes, no reflection), and a
**cross-platform target** (Linux/Windows/macOS, x86-64 + ARM64) behind the PAL — with
**Linux/x86-64 as the currently supported and verified target**. There is no .NET /
managed-runtime vocabulary anywhere in the design — see the glossary below.

<details>
<summary><strong>Reading order — the 27 RFC specification documents</strong></summary>

| # | Document | Covers | Maturity |
|---|---|---|---|
| — | [ActorEngineSpecification.md](ActorEngineSpecification.md) | Vision, principles, core invariants, glossary | **Accepted** (x86-64) |
| 001 | [001-Actor-Execution-Model.md](001-Actor-Execution-Model.md) | Lifecycle, activation, single-executor invariant, hybrid handlers | **Accepted** (x86-64) |
| 002 | [002-Scheduler.md](002-Scheduler.md) | Workers, shards, work-stealing, wakeup, fairness | **Accepted** (x86-64) |
| 003 | [003-Memory.md](003-Memory.md) | Descriptors, payloads, allocators, ownership | **Accepted** (x86-64) |
| 004 | [004-Resources.md](004-Resources.md) | Resource lifetimes, resolution, message context | **Accepted** (x86-64) |
| 005 | [005-Developer-Model.md](005-Developer-Model.md) | Actor API, CRTP policies, registration | **Accepted** (x86-64) |
| 006 | [006-Messaging-and-Addressing.md](006-Messaging-and-Addressing.md) | `ActorRef`, `tell`/`ask`, identity, streaming replies (`ask_stream`), best-effort broadcast (`Topic<M>`) | **Accepted** (x86-64; cross-node broadcast Draft) |
| 007 | [007-Failure-and-Supervision.md](007-Failure-and-Supervision.md) | Error model, restart/resume/stop/escalate | **Accepted** (x86-64, core) |
| 008 | [008-Metadata-and-Startup.md](008-Metadata-and-Startup.md) | Discovery, validation, type identity, metadata compilation | **Accepted** (x86-64) |
| 009 | [009-Observability.md](009-Observability.md) | Metrics, tracing, deadline accounting, dead-letters | **Accepted** (x86-64) |
| 010 | [010-Distribution.md](010-Distribution.md) | Node placement (HRW), SWIM membership, transport/serialization seams | **Accepted** (x86-64, core) |
| 011 | [011-Timers-and-Scheduled-Work.md](011-Timers-and-Scheduled-Work.md) | Timing wheel, delayed/periodic sends, deadlines | **Accepted** (x86-64) |
| 012 | [012-Persistence.md](012-Persistence.md) | Snapshot & event-sourced durability, recovery, fencing | **Accepted** (x86-64) |
| 013 | [013-Configuration.md](013-Configuration.md) | Policy-vs-config boundary, `EngineConfig`, overrides | **Accepted** (x86-64) |
| 014 | [014-Testing-Model.md](014-Testing-Model.md) | Deterministic simulation, fault injection, `TestKit` | **Accepted** (x86-64) |
| 015 | [015-Reentrancy-and-Quiescence.md](015-Reentrancy-and-Quiescence.md) | Reentrancy model, admission control, the quiescence primitive (cross-cutting) | **Accepted** (x86-64) |
| 016 | [016-Serialization.md](016-Serialization.md) | One `describe` per type; canonical tagged encoding; schema evolution & migrations (cross-cutting) | **Accepted** (x86-64) |
| 017 | [017-Delivery-Guarantees.md](017-Delivery-Guarantees.md) | At-most/at-least/effectively-once; transactional outbox; partition proof (cross-cutting) | **Accepted** (x86-64) |
| 018 | [018-Clocks-and-Deadlines.md](018-Clocks-and-Deadlines.md) | Monotonic deadlines; cross-node remaining-duration propagation; inheritance (cross-cutting) | **Accepted** (x86-64) |
| 019 | [019-Platform-Abstraction-Layer.md](019-Platform-Abstraction-Layer.md) | The single OS seam: event loop, sockets, affinity/NUMA, durable flush, clock; sim backend | Draft |
| 020 | [020-Security.md](020-Security.md) | Trust model, node identity & admission, transport security, authorization & principal propagation, secrets, at-rest (cross-cutting) | **Accepted** (x86-64) |
| 021 | [021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md) | Trust-root bootstrap, discovery/seeds, connection dial & dedup, elastic scale-up/down, drain & fenced hand-off, rolling upgrade (cross-cutting) | **Accepted** (x86-64) |
| 022 | [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md) | Bounded resources, rate limiting, deadline-aware load shedding, circuit breaking, fair sharing (cross-cutting) | **Accepted** (x86-64) |
| 023 | [023-Performance-Targets-and-Budgets.md](023-Performance-Targets-and-Budgets.md) | Quantified latency/throughput/footprint budgets, reference machine, benchmark & regression harness (cross-cutting) | Draft |
| 024 | [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md) | Inbound stream ingestion: per-stream credit-ring, derived credit, batch drain, backpressure-not-shedding, zero-copy (cross-cutting) | **Accepted** (x86-64) |
| 025 | [025-Placement-Policies-and-Stateless-Workers.md](025-Placement-Policies-and-Stateless-Workers.md) | Node capabilities, capability/affinity/weighted placement modifiers, stateless worker pools (cross-cutting) | **Accepted** (x86-64) |
| 026 | [026-Large-Scale-Cluster-Topology.md](026-Large-Scale-Cluster-Topology.md) | Scaling to 10³–10⁴ nodes: VirtualBins O(1) placement, bounded partial-view, DHT-relay; configurable topology/connection/cache axes (cross-cutting) | **Accepted** (x86-64) |
| 027 | [027-Reminders.md](027-Reminders.md) | Durable, wall-clock, at-least-once scheduled wake-ups on the 012 `Store` seam; SEGSTREAM token-bucket drain flattens mass-due (10⁶-at-9PM) to `peak == fire_rate` (cross-cutting) | **Accepted** (x86-64) |

</details>

<details>
<summary><strong>Proven decisions — the ADR record</strong></summary>

Where a hot-path or safety-critical design choice needs more than argument, it is settled by the
**design → red-team → prove → judge** loop: competing designs are implemented in real C++23,
compiled under GCC + Clang, run under ASan/UBSan/TSan, and benchmarked (percentiles, not means)
before a judge picks a winner. The durable records live in [`decisions/`](decisions/):

| ADR | Question | Outcome |
|---|---|---|
| [ADR-001](decisions/ADR-001-mailbox-mpsc-hot-path.md) | Mailbox MPSC hot path (round 1) | Superseded by ADR-002 |
| [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md) | Mailbox MPSC hot path (round 2, post-fix) | **Intrusive Vyukov MPSC** — exchange-published, single-consumer, tombstone-lazy. 8/8 claims proven; binds 001/002/003/015. |
| [ADR-003](decisions/ADR-003-mailbox-mpsc-hot-path-r3.md) | Mailbox MPSC hot path (round 3, REX challenge) | Winner **unchanged**; challenger rejected. Corrected three load-bearing details now in the specs: `seq_cst` wakeup rendezvous (Dekker), `acq_rel` enqueue, portable compile guard (Clang), packed-CAS cancellation. |
| [ADR-004](decisions/ADR-004-mailbox-mpsc-hot-path-r4.md) | Mailbox MPSC hot path (round 4, SEG-FAA/REX-CAS challenge) | Winner **unchanged** (4th round). Refinements now in the specs: 48-bit generation (u32 wraps in ~24h @ 50M/s), symmetric store+`seq_cst`-fence+load close-out (48% magnitude corrected to ~0.05–0.09%), tombstone-skips charged to drain budget, stub on its own cache line, claim-CAS-failure→tombstone. |
| [ADR-005](decisions/ADR-005-inbound-stream-ingestion-hot-path.md) | Inbound stream-ingestion hot path | **StreamChannel** credit-ring — one pre-allocated per-stream SPSC ring, one reusable descriptor per arm-edge (not per frame), derived credit (no shared RMW), split `disp`/`tail` cursors for async-suspend exactly-once. 8/8 proven; 30–57 M frames/s, 0 alloc, 0 drain-RMW. Spec: 024. |
| [ADR-006](decisions/ADR-006-large-scale-cluster-topology.md) | Large-scale cluster topology | **VirtualBins + Bounded Partial-View + DHT-Relay** — O(1) N-independent placement (5–6 ns), O(log N) sockets/gossip, content-addressed determinism, ≤⌈log₂N⌉ relay hops. Three configurable axes; flat clusters pay nothing. D2 Partitioned kept for >10⁴ nodes. Spec: 026 (FIFO-under-relay is the Draft→Accepted gate). |
| [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md) | Actor-authoring & handler-dispatch API | **JumpTable-Dispatch** (D1) — dense per-actor `.rodata` jump-table keyed by `consteval slot_of<A,M>` over `using protocol = Protocol<…>`; one indexed indirect call, ≈260 B/actor, no RTTI/vtable, beats the 008 scan and ties a hand-switch (uniform+skew). Async-only `ask` (no `ask_sync`), always-typed `ActorRef<A>`, member-field resources, pooled-`ReplyCell` reply ordering. 27/1 proven/disproven; sync tell p99 62 ns, ask p99 130 ns, 0 alloc. Closes 005/006/001 open questions; binds 004/008/023. |
| [ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md) | Configuration + activation-lifecycle policy | **Frozen-Core + Hot-Leaf** (D3) — every knob declares an override scope (defaults < engine < node < type < instance, resolved once) and a reconfig class (BuildOnly fail-fast vs Live). Live operational read-set packs into one 8-byte atomic word per `(shard × type_index)`: hot read = single `mov + mask`, 0 RMW, no tear; live publish = single relaxed store (67–73 ns, 0 alloc, can't stall drain). Guarded `add_actor_type<T>()` (incremental Validation + release table swap, pre-sized to `max_types`). Idle deactivation rides the 011 wheel on the actor's own lane. Closes 013/008 open questions; binds 005/011/023. |
| [ADR-009](decisions/ADR-009-failure-supervision-and-recovery-policy-model.md) | Failure, supervision & recovery policy | **Minimal / Assert-Intact** (D1) + D3's proven knobs — zero-cost Itanium `try/catch` handler guard (p99 54.4 ns guarded vs 53.5 ns control, 0 alloc); `Resume` assert-intact by default, opt-in Sequential-only `Transactional<>`; `Supervision<Node|PerType|Tree>` bounded by depth + `escalation_ttl` + 022 rate limiter; `OnRestartAsk<Fail|Retry<N,IdempotencyKey>>` (default Fail); deadline/cancel carved out of the restart decision; `PerMessage` factory failure fails the message (checked pre-handler); EventSourced staging fence. 13/0 proven/disproven. Closes 007/004 open questions; binds 015/012/023. |
| [ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md) | Priority & fairness scheduling policy | **K-band per-shard run-queue** (D1) — `Priority<P>` becomes `std::array<ActivationMpsc, K>` bands; `UniformFIFO` (K=1) default objdumps **byte-identical** to today's single MPSC (zero-cost when uniform). Enqueue = compile-time band subscript on the same `tail_.exchange` (0 added RMW); O(K≤8) relaxed top-band probe. Per-actor mailbox FIFO inviolable; high-band p99 ~316× lower. Anti-starvation is a knob: `RotatingReserve<M>` (default, bound `(d+1)·K·M`) or `WeightedDRR<w…>`. EDF-banding evaluated and deferred (degrades below FIFO under overload). 7/0 proven/disproven. Closes the 002 priority open question; binds 005/011/023. |
| [ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md) | Cluster relay & placement gate verification | **Verification record** (not a redesign). **FIFO-under-relay CORRECT** — path-pinning + drain-boundary promotion holds per-`(S,A)` FIFO across a mid-stream variable-hop path change (0 inversions / 100×10⁶ arrivals, unpinned control inverts 88–96%) → **026 Accepted, 010 Accepted (core)**, 023 FIFO cell proven. **Stateless-pool CORRECT** (exactly-once under concurrency, beats hand-rolled 1.5–2.8×). **Weighted-HRW WRONG** — caught a real defect: ADR-006's `weight·H` formula is non-proportional (fix: `w/(−ln H)`, proven proportional + bounded-churn) and the `CoV≤0.2` balance threshold is a uniform-only quantization floor → **025 held Draft** pending the formula/threshold repair (applied) + re-gate. |
| [ADR-012](decisions/ADR-012-weighted-hrw-distribution-regate.md) | Weighted-HRW re-gate (025) | **Verification record** — re-gate of the corrected log-WRH form. **INCONCLUSIVE**, and it's the honest verdict: the scheme is **demonstrably at the multinomial floor** (WRH within 1.9% of the ideal sampler at every N; churn exact — 0 bins between unchanged nodes) but the decision *band* was still ill-posed (compared p99 vs a mean-level closed-form floor, a band the ideal sampler itself busts). Refused to fake CORRECT (post-hoc widening) or WRONG (blaming the scheme for a threshold defect). Supersedes ADR-011 Gate B's WRONG for the corrected form. 025 stays Draft pending a like-for-like preregistered re-gate (running). |
| [ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md) | Weighted-HRW re-gate #2 (025) | **Verification record — CORRECT.** Re-gated the corrected proportional log-WRH `score = w_n/(−ln H)` under the like-for-like instrument ADR-012 prescribed: a **preregistered** p99-vs-p99 band derived from an independent `mt19937_64` multinomial's own bootstrap dispersion and printed **before** any WRH number was computed (no shared code/RNG, 256 paired seeds, no post-hoc widening). `R(N) ∈ [0.8765, 1.1051]` at every N (0.96/1.01/1.02/1.00); share-error ≤ MC p99·1.10 against an extreme-value-adjusted cap; churn between *unchanged* nodes = 0 exactly, non-vacuous. Both mandatory controls fired — modulo reshard moves ≥0.98, non-proportional `w·H` misses 4–26× (8–28σ). Anti-circularity <1% at every N (fixes ADR-012's 3.98% artifact); sanitizers clean with teeth, cross-compiler byte-identical (g++14.2 / clang20.1). With ADR-011 Gate C (Stateless) this satisfies the AND → **025 Accepted (x86-64)**. Residual doc-only: ADR-006 line 104 already carries the log-WRH correction. |
| [ADR-014](decisions/ADR-014-streaming-async-suspend-real-scheduler-gate.md) | Streaming async-suspend real-scheduler gate (024) | **Verification record — CORRECT.** 024's single named promotion gate: the async-suspend/resume seam wired to the **real 002 multi-threaded scheduler + 015 admission gate** (not the ADR-005 model resolver). At 10⁷ frames on g++ 14.2 + clang 20.1: `lost = dup = torn = fifo_violations = 0`, descriptor membership ≤1 (no double-enqueue/orphan), credit only for completed frames, **0** steady-drain heap allocs + **0** cross-core RMW/frame, TSan clean; the transfer path is genuinely taken (32282/32282 parked activations off all workers). All **three** mandatory controls fired non-vacuously — single-cursor (tears/loses + credits parked frames), re-enqueue (dup ≤602016, two executors, wedge), fence-removed (loses ~194K vs 0). → **024 Accepted (x86-64)**. Deferred: Hard absolute-latency (023 silicon) and the ARM64 weak-memory re-gate of the `seq_cst` Dekker close-out (TSO-proven only). |
| [ADR-015](decisions/ADR-015-actor-execution-vehicle-passive-stackless-vs-fibers.md) | Actor execution vehicle: passive+stackless vs fibers | **Design decision** — a 4-vehicle debate-prove (32 CORRECT / 1 DISPROVEN, real C++ + sanitizers). **Core = passive + stackless run-to-completion**, unchanged: 192 B/idle-actor (5.59 M/GB, identical at 10⁶↔10⁷), depth-bounded suspended frame beating a fiber at every D≤8, sync p99 96.4 ns / 41.8 M/s drain, 0 ctx-switch, 0 hot-path alloc — the only vehicle passing the safety gate *and* winning the idle-economy+throughput axis. Every fiber-per-actor vehicle is disqualified as core (Fibra breaches the 250 ns HARD ceiling on cold resume 6853 ns p999 and hits a VMA wall at ~32 K actors; BorrowedFiber pins ~16 KiB/suspended-actor ≈120×). **The "pool green threads across idle actors" hypothesis is disproven** — a fiber pins bytes+VMA the moment it runs. But a fiber facility **earned a scoped place**: an opt-in, off-hot-path, per-blocking-invocation (never per-actor) **`BlockingHandler`** — thread-backed `quark::blocking<>` (asm-free default) + gated stackful `quark::fiber<>` — for the one axis stackless physically cannot serve (suspending an un-colorable foreign-C chain in place; +C4 multiplexing). Folded into 001/002/015/024/023. |
| [ADR-016](decisions/ADR-016-serialization-wire-fast-path-encode-gate.md) | Serialization wire fast-path encode gate (016) | **Verification record — CORRECT.** 016's promotion gate: the negotiated **tagless packed** wire encode budget + negotiation/evolution correctness. In all 4 build cells ({g++ 14.2, clang 20.1} × {-O2, -O3}), tagless encode of a 24 B POD is **p99 25–28 ns** (~20× under the 500 ns Hard ceiling, inside the 200 ns goal), **near-memcpy** (0.85–1.06× of `memcpy`), **0 alloc** / 1.2×10⁶ encodes, **reflection-free** (`-fno-rtti`, 0 RTTI symbols in the codec TU); round-trip + additive evolution + v1→v2→v3 migration all pass under clean ASan/UBSan. All three mandatory controls fired — fingerprint-mismatch **corrupts** under forced-tagless (ASan heap-overflow + wrong value), endian/ABI-mismatch **corrupts** (byte-swapped id), tagged 1.67–1.83× slower than tagless. → **016 Accepted (x86-64)**. Deferred: the ≤200 ns *goal*-stamp to 023 reference silicon, and the ARM64/big-endian cross-peer re-gate (its fallback proven load-bearing). |
| [ADR-017](decisions/ADR-017-durable-reminder-mass-due-scale-gate.md) | Durable reminder mass-due scale gate (027) | **CORRECT.** A `design → debate → prove` loop ran 3 competing durable-reminder designs through red-team + executed C++23. Winner **SEGSTREAM** models the due-wave as a stream: a due-segment binds to ONE bounded 024 StreamChannel drained under a 022 fire-rate token bucket — spread is the *drain rate*, peak in-flight is the *credit window*, not N. The 10⁶-at-9 PM wave flattens to `peak == fire_rate` (re-measured from a clean build), per-tick scan O(due-now), zero committed-reminder loss across crash, owner-sharded no-duplicate-fire. → **027 Accepted (x86-64)**. |
| [ADR-018](decisions/ADR-018-outbound-streaming-replies.md) | Outbound streaming replies — an `ask` that returns a stream (006) | **Reply-Credit-Ring** (PUSH). `ask_stream<F>` returns a bounded, pre-allocated, credit-controlled SPSC ring = the **024 inbound ring with producer/consumer roles flipped** (callee = `head` producer, caller = `disp`/`tail` consumer); credit flows caller→callee for free through the same derived `capacity−(head−tail)` — **no shared counter**. Single-resolve `StreamReplyCell` + N-item ring + in-band EoS; caller drains one batch per activation turn via `StreamActivation<F>::drain` verbatim from ADR-014. Cancellation/deadline (018) teardown returns credit, leaks no ring, delivers nothing after teardown; exactly-once (017) per-item dedup watermark; reply-UAF gate extended to multi-resolve. **Accepted (design direction, x86-64)** for the mechanism; the 006 outbound-streaming *axis* stays Draft pending its named promotion gate. |
| [ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md) | Best-effort broadcast / publish primitive — `Topic<M>`, at-most-once fan-out (006) | **`Topic<M>`, D-A.** A subscriber-agnostic one-to-many primitive whose load-bearing semantic is **best-effort at-most-once**: the publisher **never blocks and never stalls** on any subscriber; a slow/full/dead subscriber is DROPPED (per-subscriber, counted) — the deliberate opposite of ADR-018's backpressure. Membership = `atomic<shared_ptr<const SubVec>>` copy-on-write snapshot + `active` flag + bounded-quiescence unsubscribe; ONE immutable refcounted `SharedPayload<M>` fanned as N thin descriptors onto each subscriber's **verbatim ADR-002 mailbox**; cross-node coalesced to one frame per node. 8/8 gates CORRECT for local fan-out (1 copy/pub amortized N×, publisher-never-stalls, exact at-most-once accounting, per-(pub,sub) FIFO, ASan/UBSan/TSan clean). D-B's hand-rolled refcount-through-mutable-pointer took a **heap-UAF on clean build** (GATE 6), proving the `atomic<shared_ptr>` SMR load-bearing. → **006 broadcast Accepted (x86-64) for LOCAL fan-out**; cross-node fan-out **Draft on GATE 7**; ARM/weak-memory deferred. |

</details>

<details>
<summary><strong>Glossary — the vocabulary this project uses</strong></summary>

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
| Persistence (012) | `InMemoryStore` (reference) + `FileStore` (append-only WAL + `fdatasync`, crash-durable) — see [PersistenceAdapters.md](PersistenceAdapters.md) | `SqliteStore` / `RocksStore` (opt-in: `QUARK_WITH_SQLITE`/`QUARK_WITH_ROCKSDB`); Postgres/object-store behind the same `Store` seam |
| Metrics/Trace (009) | snapshot API + Prometheus text | OpenTelemetry / OTLP |
| Config (013) | programmatic `EngineConfig` + env vars | TOML/JSON file loader |
| Governance (022) | per-node token-bucket rate limits + bounded queues + circuit breakers | distributed exact-limit coordinator (Redis/etcd) |
| Benchmark harness (023) | in-house timing loop over the PAL clock (dev tooling) | google-benchmark (dev-only, never linked) |
| Inbound streaming (024) | pre-allocated per-stream SPSC credit-ring + shard-`pmr` arena; copy into inline slots | transport-registered zero-copy RX buffers (io_uring/RDMA) via the PAL |
| Large-scale topology (026) | in-house VirtualBins + bounded partial-view (SWIM) + Kademlia relay, coordinator-free | external coordinator (etcd/Consul) behind the `Membership` seam |

Remaining cross-cutting design questions are tracked in [OpenQuestions.md](OpenQuestions.md).

## Contributing

Every change must follow the RFC specs (`001`–`027`) and the proven decisions
(`decisions/ADR-*`) — when code and a spec disagree, the spec wins; if the spec is genuinely
wrong, fix the spec first (RFC-style, backed by an ADR), then the code. Read
**[CONVENTIONS.md](CONVENTIONS.md)** before opening a PR — it covers target/scope, language and
dependency rules, and the hot-path rules that are tested, not trusted. CI (`.github/workflows/ci.yml`)
runs the full correctness matrix (gcc/clang Release, ASan+UBSan, TSan) on both x86-64 and arm64
and the 023 performance gate on every push and pull request.

## License

[MIT](LICENSE)
