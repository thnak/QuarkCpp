# Quark — Performance (measured)

This is the **code-and-result** companion to [023 — Performance Targets & Budgets](023-Performance-Targets-and-Budgets.md).
Every number below was produced by a benchmark in [`bench/`](bench/), built **release + native** and run
**pinned to a single core** on the machine described next. Each result carries a verdict against the 023 budget
it is measured against: `[goal]` (meets the aspiration), `[hard]`/`[floor]` (inside the veto ceiling but over goal),
`[MISS]` (over the ceiling). The harness ([`bench/bench_harness.hpp`](bench/bench_harness.hpp)) is the single place
the 023 budget table lives, so a benchmark reports a *verdict*, never a re-hardcoded number.

> **These are regression tripwires, not the canonical stamp.** 023's reference machine is one modern x86-64 *server*
> core (Zen 4 / Sapphire Rapids class, ~3–4 GHz). This box is a **slower, virtualized** Cascade Lake Xeon at 2.1 GHz
> (details below). The engine still clears almost every 023 budget here; where it lands in `[hard]` rather than `[goal]`
> (e.g. the scheduler-path local-tell p50), that is the ~2× clock gap, not a design regression — the canonical
> stamped numbers live in the ADRs (ADR-005/006/007/009/010/013/016), measured on the reference methodology.

## The machine & build

| | |
|---|---|
| **CPU** | Intel Xeon Silver 4208 @ 2.10 GHz, 2 sockets × 16 cores (Cascade Lake, AVX-512), **virtualized** (`hypervisor` flag) |
| **Caches** | L1d 32 KiB/core, L3 16 MiB/socket |
| **NUMA** | node0 = cores 0–15, node1 = cores 16–31 (benches pinned to node0) |
| **Compiler** | gcc 14.2.0 |
| **Build** | `Release` + `-O3 -march=native` (resolves to `-march=cascadelake`) |
| **Clock** | `pal::clock` = `CLOCK_BOOTTIME` via the 019 PAL (VDSO-backed, suspend-counting) |
| **Pinning** | `taskset -c 0` (single-thread latency/throughput); `-c 0-1` for the two cross-thread sections |
| **Frequency** | governor not exposed under virtualization; treat as fixed ~2.1 GHz nominal, turbo state unknown |

Because the host is a **VM**, the tails are fatter than a quiesced bare-metal reference box would show (visible as
`mean ≫ p50` and high CoV on some lines) — hypervisor scheduling jitter, not engine behavior. Percentiles (p50/p99/p999)
are reported precisely for this reason: means lie, the tail tells the truth (023 §"means lie").

### Reproduce

```bash
cmake -B build-bench-native -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-march=native" \
      -DQUARK_BUILD_TESTS=OFF -DQUARK_BUILD_SAMPLES=OFF -DQUARK_BUILD_BENCH=ON
cmake --build build-bench-native -j4                 # -j4, never -j$(nproc) — machine-safety rule

taskset -c 0   build-bench-native/bench/mailbox_bench
taskset -c 0   build-bench-native/bench/activation_bench
taskset -c 0   build-bench-native/bench/stream_bench
taskset -c 0   build-bench-native/bench/serialize_bench
taskset -c 0   build-bench-native/bench/placement_bench
taskset -c 0   build-bench-native/bench/supervision_bench           # + _noguard control
taskset -c 0-1 build-bench-native/bench/sched_bench                 # wakeup section needs a worker lane
taskset -c 0-1 build-bench-native/bench/ask_bench                   # realistic ask needs a worker lane
```

> ⚠️ **Never** run a bench without `taskset`, never past `-c 0-3`, and always build `-j4`. Saturating all cores can
> hang or power off this box (see the repo machine-safety rule).

---

## Results at a glance

| Feature (spec) | Metric | Measured | 023 budget | Verdict |
|---|---|---|---|---|
| **tell** — mailbox (003) | enqueue→dequeue p50 | **59 ns** | ≤ 100 / 250 ns | `[goal]` |
| **tell** — mailbox (003) | peak throughput | **38.6 M/s** | ≥ 50 / 20 M/s | `[floor]`¹ |
| **tell** — scheduler (002) | full-lifecycle throughput | **11.0 M/s** | ≥ 10 / 4 M/s | `[goal]` |
| **tell** — scheduler (002) | local-tell p50 (thru scheduler) | **116 ns** | ≤ 100 / 250 ns | `[hard]`¹ |
| priority zero-cost (002) | `UniformFIFO` vs raw MPSC | **+0.45 ns** | within noise | `[within noise]` |
| **ask** (006) | engine-overhead round-trip p50 | **147 ns** | ≤ 1 µs / p99 ≤ 5 µs | `[goal]` |
| **ask** (006) | realistic `block_on` p50 | **2.0 µs** | (futex-bound²) | — |
| **streaming** (024) | sustained ingest | **140.8 M/s** | ≥ 10 / 4 M/s | `[goal]` |
| **streaming** (024) | per-frame drain cost | **7.1 ns** | ≤ 100 / 250 ns | `[goal]` |
| **streaming** (024) | ingest vs discrete tell | **5.0× cheaper** | ≥ 3× | `[goal]` |
| **activate/deactivate** (001) | cold activation p50 | **111 ns** | ≤ 10 / 50 µs | `[goal]` |
| **activate/deactivate** (001) | cycle throughput | **14.8 M/s** | ≥ 10 / 4 M/s | `[goal]` |
| idle density (003) | activations / GB | **1.95 M/GB** | ≥ 1 M / 0.5 M | `[goal]` |
| **serialize** (016) | tagless wire encode p99 | **50 ns** | ≤ 200 / 500 ns | `[goal]` |
| **placement** (010/026) | VirtualBins lookup, N-indep. | **12.5 ns** | ≤ 20 / 50 ns | `[goal]` |
| supervision guard (007) | guarded vs no-guard success path | **~1.0×** | ≤ noise | `[free]` |

¹ The two `[floor]`/`[hard]` lines are the **peak** paths most sensitive to raw clock; at ~2.1 GHz vs the ~3.5–4 GHz
reference they land inside the hard ceiling but over goal — a clock gap, not a design miss (ADR-007 proved p50 46.8 ns
tell→dispatch, ADR-002 the ≥50 M/s peak, on the reference core).
² The realistic-ask number **adds the OS futex park/wake**, a scheduler/deployment cost the engine does not own —
excluded from the engine budget exactly as 023 excludes network RTT from the cross-node budget (see the ask section).

---

## tell — the send hot path (003 mailbox, 002 scheduler)

`tell` is fire-and-forget delivery. Two layers are measured: the bare **003 mailbox** (enqueue→dequeue), and the
full **002 scheduler lifecycle** the message actually rides (post → run-queue → select → acquire → drain → close-out).

**Code** — [`bench/mailbox_bench.cpp`](bench/mailbox_bench.cpp), [`bench/sched_bench.cpp`](bench/sched_bench.cpp):

```cpp
// mailbox: time each enqueue→dequeue round trip (occupancy-1 sequential path), 0 alloc
d->message_id = MessageId{i};
mb.enqueue(d);
DrainResult r = mb.try_dequeue();          // p50/p99/p999 over 2M steady-state samples
```
```cpp
// scheduler: the honest per-core lifecycle, one descriptor + one activation reused (0 alloc)
if (act.post(&d)) rq.enqueue(&s);
const RunResult r = rq.select();
(void)act.try_acquire();  act.drain_step(64);  (void)act.close_out();
```

**Result** (`taskset -c 0`, and `-c 0-1` for the wakeup probe):

```
mailbox enqueue→dequeue :  p50 59.0   p99 74.0    p999 177.0 ns          [goal]
mailbox peak throughput :  38.56 M msg/s/core                            [floor]  (goal ≥ 50 / floor ≥ 20)
scheduler full-lifecycle:  11.00 M msg/s/core                            [goal]   (goal ≥ 10 / floor ≥ 4)
scheduler local-tell    :  p50 116.0  p99 169.0   p999 304.0 ns          [hard]   (goal ≤ 100 / hard ≤ 250)
UniformFIFO zero-cost   :  raw 24.541 ns  vs  RunQueue<UniformFIFO> 24.991 ns   Δ +0.450 ns   [within noise]
cross-thread wakeup tell:  p50 1202  p99 1906  p999 22487 ns             (futex-wake-bound; informational)
```

**Takeaway.** The bare mailbox round trip clears the tell-latency goal (59 ns p50). Through the full scheduler
lifecycle the p50 (116 ns) is inside the hard ceiling — the ~2× clock gap to the reference core. Sustained
lifecycle throughput clears the 10 M/s goal, and the **priority run-queue is genuinely zero-cost when uniform**
(`UniformFIFO` within 0.45 ns of a raw MPSC — the 002/ADR-010 "priority is free until you use it" contract).

---

## ask — request/reply (006, ADR-007)

An `ask` is a full round trip: the caller posts an `Ask<Q,R>` envelope carrying a pooled `Responder`, a lane drains
and dispatches it, the handler `respond()`s through the shard-pooled `ReplyCell`, and the caller reads the value.
The benchmark reports **two** numbers that measure different things:

- **A) engine overhead** — the ADR-007 path the 1 µs budget actually governs: build envelope + dispatch + pooled
  `ReplyCell` resolve, drained inline on the **same thread** (no cross-thread park). This is the engine's own work.
- **B) realistic `block_on(ask)`** — the developer-facing round trip over the running engine, which **adds the OS
  futex park/wake**. That is µs-class and a property of the *scheduler/deployment*, not the engine — reported for
  honesty, and excluded from the engine budget exactly as 023 excludes network RTT.

**Code** — [`bench/ask_bench.cpp`](bench/ask_bench.cpp):

```cpp
struct Squarer : Actor<Squarer, Sequential> {
    using protocol = Protocol<Ask<GetSquare, std::uint64_t>>;
    void handle(const Ask<GetSquare, std::uint64_t>& m) noexcept { m.respond(m.query.x * m.query.x); }
};
// A) same-thread: post the ask envelope to the lane, drain it inline, read the resolved cell.
// B) realistic:   result<uint64_t> r = block_on(ref.ask<uint64_t>(GetSquare{i}));
```

**Result** (`taskset -c 0-1`):

```
A) engine-overhead ask :  p50 147.0  p99 226.0  p999 381.0 ns            [goal]   (023 p50 ≤ 1000 / p99 ≤ 5000)
B) realistic block_on  :  p50 2009   p99 45110  p999 110912 ns  (mean 11067)      futex-wake-bound
```

**Takeaway.** The engine's own ask overhead is **147 ns p50 / 226 ns p99** — comfortably inside the 1 µs / 5 µs
goal, and 0-allocation after warmup (the pooled `ReplyCell`). The realistic `block_on` figure is dominated by the
kernel futex round trip on this VM; in production an on-lane `co_await ask` (no thread park) pays only the
engine-overhead cost, and `block_on` is an off-lane *bootstrap* verb, not the steady request path.

---

## streaming — inbound ingest (024, ADR-005)

Stream frames ride a **different hot path** than discrete `tell`: a budgeted batch drain off a per-stream **credit
ring**, not one descriptor per frame. Backpressure is structural — the producer can only push while it holds credit,
so a fast producer throttles losslessly instead of allocating unboundedly.

**Code** — [`bench/stream_bench.cpp`](bench/stream_bench.cpp):

```cpp
while (next_push < kFrames) {                       // producer: push until credit depletes
    if (!ch.try_push(Frame{next_push, ...})) break; // no credit → drain to return credit
    ++next_push;
}
StreamBatch<Frame> batch(ch, kDrainBudget);         // consumer: budgeted batch drain
while (const Frame* f = batch.next()) { sink += ...; ++processed; batch.retire(); }  // retire() returns credit
```

**Result** (`taskset -c 0`, 50 M frames through a 1024-slot ring):

```
1) sustained ingest        :  140.81 M frames/s/core                     [goal]  (goal ≥ 10 / hard ≥ 4)
2) per-frame drain cost    :    7.10 ns/frame                            [goal]  (goal ≤ 100 / hard ≤ 250)
3) ingest vs discrete tell :  discrete tell 28.72 ns  vs  ingest 5.72 ns  →  5.0× cheaper   [goal]  (≥ 3×)
```

**Takeaway.** Batched ring ingest sustains **140 M frames/s/core at 7 ns/frame** — an order of magnitude over the
10 M/s goal, and **5× cheaper per item than a discrete mailbox tell**, which is the entire reason the stream path
exists (023 §Streaming ingest / ADR-005). The steady loop allocates nothing (the ring is allocated once, cold).

---

## activate / deactivate — the activation lifecycle (001, 002; ADR-015)

An actor is **activated lazily** on its first message and **deactivated** (returned to `Idle`) when its mailbox
drains. In a large, sparse population this activate→process→deactivate cycle runs constantly, so both its per-cycle
cost and the per-idle-actor footprint matter.

**Code** — [`bench/activation_bench.cpp`](bench/activation_bench.cpp):

```cpp
a->post(&d);                 // activate edge:  Idle → Scheduled
(void)a->try_acquire();      //                 Scheduled → Running
a->drain_step(64);           // dispatch the message
(void)a->close_out();        // deactivate:      Running → Idle    (state()==Idle asserted)
```

**Result** (`taskset -c 0`):

```
1) idle footprint     :  sizeof(Activation) = 512 B      density = 1.95 M activations/GB     [goal]
                         (ADR-015 marginal cost = 192 B/idle — the passive, stackless core owns no per-actor stack)
2) cold activation    :  p50 111.0  p99 492.0  p999 730.0 ns   (200k fresh activations)      [goal]  (goal ≤ 10 µs)
3) activate/deactivate:  14.75 M cycles/s/core                                               [goal]  (goal ≥ 10 / floor ≥ 4)
```

**Takeaway.** A **cold** activation — a never-run actor's first message, caches cold — completes in **111 ns p50**,
~90× under the 10 µs goal, and the steady activate/deactivate cycle sustains **14.8 M/s**. Each idle activation costs
512 B whole-object here (well under the 2 KB ceiling → 1.95 M/GB); the ADR-015 *marginal* per-idle cost is 192 B
(≈ 5.6 M/GB) because the passive, stackless execution core owns no per-actor stack — this is what makes million-actor
populations affordable.

---

## serialize — the wire fast path (016, ADR-016)

The 016 fast path encodes a described POD **tagless** (values in tag order, packed) at near-`memcpy` speed; the
canonical tagged TLV path must be measurably slower, proving the fast path is not vacuous.

**Code** — [`bench/serialize_bench.cpp`](bench/serialize_bench.cpp) (24 B POD into a cold 64 B line strided over 64 MB > L3):

```cpp
QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))
std::memcpy(dst, &p, sizeof(Pod));          // A) baseline
encode_tagless(p, dst);                      // B) the fast path
encode_tagged(p, dst, kStride);              // C) canonical TLV (must be slower)
```

**Result** (`taskset -c 0`):

```
A memcpy   :  p50 41.0  p99 82.0   p999 310.0 ns
B tagless  :  p50 35.0  p99 50.0   p999 102.0 ns        tagless p99 = 50 ns   [≤200 goal OK]
C tagged   :  p50 41.0  p99 200.0  p999 423.0 ns
near-memcpy factor (tagless p99 / memcpy p99) = 0.61×   ·   fast-path-not-vacuous (tagged/tagless p50) = 1.17×
```

**Takeaway.** Tagless encode is **50 ns p99** — well under the 200 ns goal, and actually *at or below* `memcpy` here
(0.61×; the tagless writer streams tighter than the strided `memcpy` baseline on cold lines). The tagged path is
measurably slower (1.17× p50, 4× p99), confirming the fast path is real (023 line 60 / ADR-016).

---

## placement — distribution lookup (010, 026; ADR-006)

Placement answers "where does this actor live?" as a pure function of identity. Raw rendezvous (HRW) is O(N) over the
roster; the 026 `VirtualBins` cache turns it into an **O(1), N-independent** bin lookup — the property the budget names.

**Code** — [`bench/placement_bench.cpp`](bench/placement_bench.cpp):

```cpp
VirtualBins vb(std::span<const NodeId>(nodes), 16 * n_nodes);   // B ≥ 16·N for balance
sink += vb.owner_of(ids[i & 4095]).value_or(NodeId{0}).value;   // amortized ns/op (sub-clock op)
```

**Result** (`taskset -c 0`):

```
VirtualBins owner_of  N=  16 :  12.64 ns/op     [goal]  (goal ≤ 20 / hard ≤ 50)
VirtualBins owner_of  N= 256 :  12.52 ns/op     [goal]        N-independence: 256/16 = 0.99×  [flat]
raw HRW place()       N=  16 : 141.86 ns/op     (O(N) — informational, why the cache exists)
raw HRW place()       N=  64 : 433.92 ns/op     (O(N) — grows with N)
```

**Takeaway.** `VirtualBins` lookup is **12.5 ns and flat** from N=16 to N=256 (0.99× — genuinely N-independent),
inside the 20 ns goal. Raw HRW grows 3× from N=16 to N=64, which is exactly why the O(1) cache exists — at a million
nodes the raw scan is unusable, the bin lookup is unchanged.

---

## supervision — the zero-cost failure guard (007, ADR-009)

The handler-boundary guard (the one `try/catch` that contains a throwing handler) must be **invisible on the success
path**. The benchmark compiles the same actor twice — with the guard and with it compiled out
(`-DQUARK_SUPERVISION_NO_GUARD`) — and compares the no-throw success path.

**Code** — [`bench/supervision_bench.cpp`](bench/supervision_bench.cpp) + the `supervision_bench_noguard` control twin.

**Result** (`taskset -c 0`):

```
guarded          :  p50 64.0  p99 88.0  p999 189.0 ns
control (noguard):  p50 64.0  p99 90.0  p999 200.0 ns
→ ratio ≈ 1.0 : the handler-boundary guard adds nothing on the success path
```

**Takeaway.** Guarded and unguarded success paths are **identical within noise** (p50 64 = 64 ns) — the Itanium ABI
puts the landing pad in a cold section, so the try/catch costs nothing until something throws (023 §Failure /
ADR-009 F1). Failure containment is genuinely free on the path that never fails.

---

## What this document is *not*

- **Not the canonical stamp.** These are this-machine tripwires; the reference numbers are in the ADRs, measured on
  the 023 reference methodology. Where a line reads `[hard]`/`[floor]` rather than `[goal]`, that is the ~2× clock gap
  and virtualization jitter, not a design regression.
- **Not the deterministic gates.** The *invariant* budgets — descriptor ≤ 64 B (`static_assert`), **0 hot-path heap
  allocations**, **0 cross-core RMW on the drain path**, and the objdump zero-cost parity checks — are machine-independent
  and live in [`tests/`](tests/) as pass/fail CTest gates, not here (a shared/noisy runner would flap a ns gate). This
  document is the *noise-sensitive numeric* tier: reported with verdicts for eyeballing and trend-tracking.
- **Not an end-user SLA.** These bind the engine's internal design (023 §Non-goals); real deployment latency depends
  on hardware, network, workload, and handler code.
