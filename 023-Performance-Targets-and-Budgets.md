# 023 — Performance Targets & Budgets

Every spec in this RFC leans on words — *zero-cost*, *contention-free*, *O(1)*,
*no hot-path allocation* — and not one states a **number**. That makes the
headline claims un-falsifiable: "zero-cost" is not a verdict until there is a
budget it either meets or misses. This spec pins the numbers, states the reference
machine they are measured on, and defines the **benchmark harness** that enforces
them so a claim like "the mailbox is contention-free" becomes a test that passes
or fails, not an assertion of taste.

> A performance target is a **contract against a design**, not a promise to an end
> user. It exists so a design choice can be *rejected* for missing it — the
> empirical counterpart to 014's correctness testing. 014 proves the engine is
> *right* (deterministic simulation, virtual clock); 023 proves it is *fast enough*
> (real hardware, wall clock). Same engine, opposite clock.

## The reference machine — numbers are meaningless without one

All per-core budgets below are measured on a fixed baseline:

- **Primary baseline: one modern x86-64 server core** — Zen 4 / Sapphire Rapids
  class, ~3–4 GHz, warm L1/L2, **release build (`-O2`/`-O3`, LTO)**, single core
  pinned (002), turbo behavior noted per run.
- **ARM64 (Graviton3/4, Ampere, Apple) is expressed as an allowed ratio**, not a
  separate table: **≤ 1.5× latency and ≥ 0.66× throughput** versus the x86-64
  budget (provisional). ARM64's weaker memory model is a *correctness* matter
  already handled by `std::atomic` (019), not a budget relaxation — the ratio
  covers microarchitectural difference, not sloppier ordering.
- Numbers are **ranges and ceilings, not points** — measurement noise is real, so a
  budget is "goal / hard ceiling," and the harness reports distributions, never a
  single hero number.

## The priority rule — balanced, throughput breaks ties

Latency, throughput, and footprint cannot all be maximized at once (batching buys
throughput at the cost of tail latency; tiny descriptors buy footprint at the cost
of indirection). The locked decision:

> **Budget all three; when a design choice genuinely trades them off, favor
> throughput — but never past a hard latency or footprint ceiling.**

This is what makes throughput the *tiebreaker*, not the *dictator*: a design may
not buy throughput with unbounded batching, because the tail-latency ceiling and
022's **shed-don't-buffer** rule (bounded queues) forbid trading latency away
without limit. Throughput wins the coin-flip; the ceilings are the coin's edge.

## The budgets

Each budget is **Hard** (a design that misses it is rejected) or **Goal**
(aspirational; missing it is a tracked regression, not a veto). All local figures
are the x86-64 baseline above.

### Hot-path latency

| Path | Goal | Hard ceiling |
|---|---|---|
| Local `tell` enqueue→dequeue (sequential, same shard) | ≤ **100 ns** | ≤ 250 ns |
| Local `ask` round-trip (same node) | ≤ **1 µs** p50 / ≤ 5 µs p99 | p99 ≤ 20 µs |
| Cross-node engine overhead (**excl.** network RTT) | ≤ **5 µs** serialize + dispatch | ≤ 15 µs |
| Wire fast-path encode of a small POD message (016) | ≤ **200 ns** (near-memcpy) | ≤ 500 ns — **proven** p99 **25–28 ns** (24 B POD, 4 build cells), 0.85–1.06× `memcpy`, 0 alloc ([ADR-016](decisions/ADR-016-serialization-wire-fast-path-encode-gate.md)); ≤200 ns goal-stamp on reference silicon deferred |
| Cold activation (first message, lazy; excl. user ctor & 012 recovery) | ≤ **10 µs** | ≤ 50 µs |
| Work-steal acquire (002) | ≤ **1 µs** | ≤ 5 µs |

### Throughput

| Metric | Goal | Hard floor |
|---|---|---|
| Local `tell`, sequential actor, sustained | ≥ **10 M msg/s/core** | ≥ 4 M/s/core |
| Local `tell`, tight enqueue/dequeue loop (peak) | ≥ **50 M msg/s/core** | ≥ 20 M/s/core |
| Aggregate scaling across N cores | ≥ **0.8·N** linear (near-linear) | ≥ 0.6·N |

### Memory & density

| Metric | Goal | Hard ceiling |
|---|---|---|
| Message handle + descriptor (003) | — | **≤ 64 B** (one cache line); payload stored separately |
| Idle-activation engine overhead (excl. user state) | ≤ **512 B – 1 KB** | ≤ 2 KB |
| Idle-actor density (engine overhead) | ≥ **1 M activations / GB** | ≥ 500 K / GB |
| Idle footprint — **proven** (ADR-015) | — | **192 B/idle-actor** (identical at 10⁶ and 10⁷), **5.59 M/GB** (~28× under the 2 KB ceiling), 1 non-scaling stack VMA — the passive+stackless core owns no per-actor stack |
| Suspended footprint, depth-parameterized — **proven** (ADR-015) | — | stackless frame **656 B (D1) … 2.9 KB (D8, < page) … 20.8 KB (D64)**; beats a fiber (≥6088 B + guard VMA even at D1) at every depth **D ≤ 8**; deep async nesting (D ≳ 16–32) can reach a fiber page |
| Per-message hot-path heap allocations | — | **0** (measured, not asserted) |
| Cross-core atomic RMWs on the sequential drain path | — | **0** (per-shard single-writer, 009) |

The last two are the sharpest: they promote two *stated invariants* ("no hot-path
allocation," "contention-free counters") into **measured gates** — a hooked
allocator counts allocations per message, and TSan + `perf` counters confirm no
cross-core RMW on the drain loop. A design that quietly allocates or shares a
counter now *fails a test* instead of eroding a claim.

### Tail

| Metric | Goal | Hard ceiling |
|---|---|---|
| p99/p50 local latency under sustained load | ≤ **10×** | ≤ 25× |
| p999 local `tell` latency | ≤ **5 µs** | ≤ 50 µs |

Tail is first-class because **means lie**: a design can post a great average and a
catastrophic p999 (a hidden lock, an allocation spike, a rehash). Fairness (002)
and shed-don't-buffer (022) exist precisely to protect the tail, so the tail is
budgeted, not just the median.

### Streaming ingest (024)

Inbound stream frames ride a different hot path than discrete `tell` (a batch drain
off a per-stream ring, not one descriptor per frame), so they get their own block.
Proven by [ADR-005](decisions/ADR-005-inbound-stream-ingestion-hot-path.md) on a
conservative rig — absolute ns re-measure on the reference core before the Hard
latency budget is stamped.

| Metric | Goal | Hard | Proven (ADR-005) |
|---|---|---|---|
| Sustained ingest, small frames | ≥ **10 M/s/core** | ≥ 4 M/s | 30.25–57.12 M/s |
| Per-frame amortized cost vs. a discrete `tell` | ≥ **3×** cheaper | — | 12.6–19.7× |
| Per-frame drain-step latency | p50 ≤ **100 ns** | ≤ 250 ns | p50 15–18 ns |
| signal→handler latency | p999 ≤ **5 µs** | ≤ 50 µs | p999 813–959 ns |
| Steady hot-path heap allocations | **0** (measured) | 0 | 0 / 50M frames |
| Cross-core atomic RMW on the drain path | **0** (measured) | 0 | 0/frame, both compilers — O(empty-transitions), not O(frames) |

### Blocking / fiber adapter (ADR-015) — explicitly *off* the sync budget

The opt-in `BlockingHandler`/`FiberHandler` adapter (001) is a **µs–ms offload path, not
the ns-scale sync path** — it is **excluded from the 100/250 ns sync ceilings** by rule.
Its purpose is to free the mailbox lane, not to be fast; budgets here are for capacity
planning, not hot-path gating.

| Metric | Budget | Proven (ADR-015) |
|---|---|---|
| Thread-backed `quark::blocking<>` suspend→resume | **loaded distribution** — plan against p50, not the floor | p50 **62 µs** / p99 143 µs (min 2.08 µs) on a non-quiesced rig — **do not stamp the optimistic ~3 µs figure** |
| Stackful `quark::fiber<>` register-only switch | ≤ **30 ns** hot | p50 16–20 ns hot; **cold-switch p99 ~524 ns/pair** (why it stays off the hot path) |
| Suspended-fiber footprint | bounded by pool `P` + 022 shed | **16–20 KiB per in-flight call** (stack + guard VMA); VMA ceiling ~500 K this rig / **~32 K on a default kernel** — a hard operational limit, surface in config/docs |
| Lane freed during a blocking call | — | co-resident actor ran **9.31 M msg/s** during a 50 ms blocking call |

This path's stackful asm + `__sanitizer_*_switch_fiber`/`__tsan_*_fiber` shims are the
**only** non-std-core surface, confined to the PAL (019); every fiber correctness proof is
inadmissible until its annotation positive-control fires (a mandatory gate-0).

### Distribution / placement (010, 026)

| Metric | Goal | Hard | Proven (ADR-006) |
|---|---|---|---|
| Placement lookup, **N-independent** | p99 ≤ **20 ns** | ≤ 50 ns | 5–6 ns mean, ≤38 ns p99 (VirtualBins) |
| Membership repair, single join/leave | ≤ **2 ms** | — | requires **batched admission** for bulk join (serial per-event blows it) |
| Bin/bucket balance (**uniform placement**) | CoV ≤ **0.2**, max/mean ≤ 1.5 at N ≤ max_nodes | — | passes at `B ≥ 16·max_nodes`. *Uniform-only* — ADR-011 proved this is a proportional quantization floor that **weighted** placement cannot meet at 16 bins/node; the weighted target is load-per-weight (ρ = realized/weight), 025/ADR-011 |
| Bin/bucket balance (**weighted placement**) | load-per-weight ρ at the multinomial floor: `R(N) = CoV_p99(ρ_WRH)/CoV_p99(ρ_MC) ∈ [0.8765, 1.1051]` | — | **proven** — proportional log-WRH `w/(−ln H)` re-gated CORRECT under a like-for-like preregistered p99-vs-p99 band vs an independent multinomial; `R(N)` in band at every N, churn exact (0 bins between unchanged nodes), both controls fired ([ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md)) |
| Cross-node **FIFO under relay** | correct | **correct (Hard gate)** | **correct** — 0 inversions / 100 trials × 10⁶ arrivals per cell, both g++14.2 & clang20.1 ([ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md)); control fired 88–96% (137/168 inversions), null baselines 0, real hop-vector change 2–6→1 in 100/100, TSan/ASan/UBSan clean |

The 0-cross-core-RMW drain-path gate binds the 026 placement read path too (proven 0
RMW). Absolute ns re-measure on the reference core before stamping.

### Handler dispatch & `ask` (005 / 006 / 008)

Proven by [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)
(dense per-actor jump-table, x86-64, GCC 14.2 + Clang 20.1). Recorded as permanent
regression gates:

| Metric | Goal | Hard | Proven (ADR-007) |
|---|---|---|---|
| Local sync `tell` enqueue→dequeue→dispatch | p99 ≤ **100 ns** | ≤ 250 ns | p50 46.8 / p99 62.1 / p999 119.3 ns |
| Dispatch step alone (indexed indirect call) | — | — | p50 ≈ 20 / p99 ≈ 25 ns |
| Local `ask` round-trip (pooled `ReplyCell`) | p50 ≤ **1 µs** / p99 ≤ 5 µs | p99 ≤ 20 µs | p50 83.1 / p99 129.8 / p999 256.8 ns |
| Hot-path heap alloc — sync tell / async handler / ask | **0** (measured) | 0 | 0 (positive control: per-message `std::stop_source` = 1 alloc/msg) |
| Cross-core atomic RMW on the drain | **0** (measured) | 0 | 0 (TSan clean; relaxed-publish control races `msg_slot_`) |
| Dispatch code size | — | — | **≈ 260 B/actor, linear, independent of engine-wide msg count** |

Permanent CI gates from this proof: the **jump-table-vs-hand-switch parity check**
(uniform *and* 95%-skew mixes must stay within noise), the **relaxed-publish TSan
positive control** (relaxing the `msg_slot_` publish must race), and the **reply-lifetime
ASan+TSan gate** (concurrent cancel+complete, ABA reuse, sync-complete-before-suspend).
The dense slot is process-local and must never be serialized (008).

### Configuration control plane (013 / 008 / 011)

Proven by [ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md)
(Frozen-Core + Hot-Leaf). Config is **not** a hot path in itself, but the **operational
read** (drain budget, mailbox bound, overflow, idle timeout) rides the drain/admission hot
path and is bound by the existing gates; the **publish** is explicitly cold.

| Metric | Goal | Hard | Proven (ADR-008) |
|---|---|---|---|
| Operational config read (packed word) | — | bound by the **0-RMW drain gate** + ≤100 ns local-tell | single `mov + mask`, **0 RMW**, no decode branch (objdump both compilers, -O2 & -O3+LTO); ~1.9–2.3 ns amortized incl. call, inlined = 1 instr |
| No torn config read under concurrent publish | **0** (measured) | 0 | 0 torn over 1.35B–4.6B reads (x86-64; ARM64 arm unverified) |
| Reconfig storm co-resident with drain | — | drain drop within noise | <5% with `alignas(64)` cell; packed-8B control = 74% drop (padding load-bearing) |
| Live-reconfig **publish→visible** | *(cold path — no Hard budget)* | — | 67–73 ns/op publish (0 alloc); visible p50 ≈ 140 ns, p99 206–273 ns on sub-reference silicon |

The publish→visible number is a **cold-path** metric with **no Hard ceiling** — config is
operational, not per-message. It re-measures on the Zen4/SPR reference core before any
tightened target is stamped. The **0-RMW operational-read** gate and the **no-torn**
control are permanent CI gates.

### Failure / supervision (007 / 004 / 015 / 012)

Proven by [ADR-009](decisions/ADR-009-failure-supervision-and-recovery-policy-model.md)
(Minimal / Assert-Intact, zero-cost guard). The rule: the failure machinery must be
**invisible on the success path** and **cold** on the failure path.

| Metric | Goal | Hard | Proven (ADR-009) |
|---|---|---|---|
| Local sync tell **with** handler-boundary guard | p99 ≤ **100 ns** | ≤ 250 ns | p99 54.4 ns guarded vs 53.5 ns no-guard control, **ratio ≤ 1.019** across GCC/Clang × -O2/-O3+LTO |
| Hot-path heap alloc on the no-throw path | **0** (measured) | 0 | 0 allocs/msg over 1e7; objdump: no added call/alloc/state-branch on success |
| `Transactional<>` opt-in rollback cost | *(opt-in, off the default path)* | — | ∝ `sizeof(State)` (4×`movdqu` 64 B copy control); **nothing** unless selected |
| Failure path (throw → unwind) | *(cold / off-budget)* | — | µs-scale Itanium `.gcc_except_table` unwind, bounded by `MaxRestarts`/`Within` + 022 rate limiter |
| Poison-message reclamation | exactly once | **exactly once** | one `Running→Completed` gen-bump + pool-return join point, 4×2M descriptors, TSan+ASan clean |

The escalation-storm guard binds to 022's **per-shard-local** token bucket (no global atomic,
consistent with the 0-cross-core-RMW drain gate). Permanent CI gates: the guarded-vs-no-guard
**success-path ratio** (must stay ≤ noise), the **poison-exactly-once** ASan control
(relaxed-store → double-free must fire), and the **reply-cell UAF/ABA** gate for
ask-triggers-restart (ADR-007/009).

### Priority & fairness scheduling (002 / 005 / 011)

Proven by [ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md)
(K-band per-shard run-queue). The contract: **priority is free until you use it**, orders
activations not messages, and its anti-starvation bound is enforced, not hoped-for.

| Metric | Goal | Hard | Proven (ADR-010) |
|---|---|---|---|
| `UniformFIFO` enqueue+select vs no-priority control | — | **objdump byte-identical** (hard gate) | byte-identical both compilers, -O2 & -O3; tell→dispatch p50 89.7 vs 85.0 ns; ≥ 10.8 M msg/s/core (above the 10 M floor) |
| Cross-core RMW added by banding (enqueue + per-select) | **0** (measured) | 0 | 0 lock-prefixed ops in the K-way probe; enqueue = same single `tail_.exchange` |
| High-band dispatch p99 under mixed saturating load | beats `UniformFIFO` | — | 9.3 µs vs 2.94 ms (~316×), p999 27 µs (`PriorityBands<2,RotatingReserve<8>>`) |
| Anti-starvation bound | reported + enforced | — | `(d+1)·K·M` select turns, measured tight for every band (K=4,M=8: d=0→32, d=4→160) |
| Band count | — | **K ≤ 8** | K-way probe select p99 inflects past K≈8 under adversarial producer pressure (645 ns at K=8) |

Permanent CI gates: the **`UniformFIFO`-byte-identical** check (the zero-cost-when-uniform
contract), the **0-RMW** banding check, the **high-band-beats-FIFO p99** check, and the
enforced **`(d+1)·K·M`** starvation bound. *Caveat:* RMW counts came from objdump lock/xchg
counting + TSan (`perf_event_paranoid=4` blocked HW counters) — re-confirm with `perf c2c`
on a `CAP_PERFMON` CI box before stamping the HW-counter form of the gate.

## How a budget binds a design

A budget is only real if something enforces it. Each is wired to the existing
proof machinery:

- Every falsifiable performance claim in a spec (e.g. "mailbox enqueue is O(1) and
  lock-free") is paired with a **budget** and an **experiment** that measures it —
  the `quark-prover` / `design-debate-prove` loop. A claim is **PROVEN** only when
  measured C++ meets its budget on the reference machine, not when it argues well.
- A spec cannot promote from **Draft → Accepted** with an unproven Hard budget on
  its hot path.

## The benchmark harness

Targets need a machine that measures them repeatably. The harness has two tiers:

### Microbenchmarks — catch regressions

Per-operation loops (enqueue/dequeue, dispatch, serialize, steal) measured in
ns/op and ops/s. Requirements that separate a real benchmark from a misleading one:

- **Percentiles, never just means** (p50/p99/p999) — the tail is where the bugs hide.
- **Warmup + steady-state**, discard cold iterations; **pinned core** (002);
  frequency-scaling/turbo state recorded per run so numbers are comparable.
- **Timing via the PAL clock** (019 `mono_now`, or `rdtsc`/`cntvct` where the
  backend exposes it) — high-resolution, low-overhead, cross-platform.
- Statistical honesty: report variance; a regression must exceed the noise band to
  count (below).

### Macrobenchmarks — validate realism

Whole-engine workloads that exercise the interactions a microbenchmark misses:
fan-in/fan-out, hot-key contention, request/reply chains, a mixed persistent +
in-memory population, and a churny cluster (010/021). These consume **009's metrics
snapshot** (per-shard counters, latency histograms) as their measurement surface —
the observability spec *is* the macro instrument, no separate plumbing.

### Regression gating

- The harness runs in CI on a **pinned, quiesced machine** (a shared runner's noise
  makes ns-scale numbers meaningless).
- A change fails the gate when a **Hard** budget is violated, or a **Goal** metric
  regresses **beyond a noise band** (a per-metric threshold, e.g. > 5% sustained
  over the rolling baseline) — not on a single noisy sample.
- Results are tracked over time so slow drift is visible, not just single-PR cliffs.

## Why not the deterministic simulator (014)?

014's simulation backend runs on a **virtual clock** — perfect for reproducing
*logic* (message orderings, fault interleavings), useless for measuring *cycles*
(it doesn't model cache misses, branch prediction, or real contention). Perf must
therefore run on the **native PAL backend on real hardware** (019). This is the
same compile-time backend-selection seam serving its second master: 014 picks the
sim backend for deterministic correctness, 023 picks the native backend for honest
timing. One engine, two floors.

## Self-debate

- **Mean or percentile?** Percentile, always, plus the tail (p99/p999). A mean
  hides the exact pathologies — a stray lock, an allocation spike — that this whole
  RFC's hot-path discipline exists to prevent. Budgeting the mean would let a design
  pass while its tail is on fire.
- **Micro or macro benchmarks?** Both, different jobs. Micro is the *regression
  tripwire* (sensitive, isolates one op, runs fast in CI); macro is the *reality
  check* (catches interaction effects a micro loop can't see, e.g. false sharing
  that only appears under a real workload mix). Ship both; trust neither alone.
- **Single number vs. ceiling+goal?** Ceiling+goal. A single target is either so
  loose it's meaningless or so tight that measurement noise flaps the gate. The Hard
  ceiling is the veto line; the Goal is the aspiration the trend chart tracks.
- **Throughput as tiebreak — can't a design just batch to win?** No — and this is
  why the ceilings exist. Batching that inflates the p99 tail past its ceiling, or
  that grows an unbounded queue (forbidden by 006 bounds and 022 shed-don't-buffer),
  is rejected regardless of the throughput it posts. Throughput breaks *ties between
  otherwise-compliant designs*, it does not license buying throughput with latency.
- **Pin the numbers now, before implementation exists?** Yes, deliberately. Targets
  set *before* the code bias the design toward meeting them; targets rationalized
  *after* measuring whatever the first implementation happened to do are not targets,
  they are excuses. These are provisional and will be re-baselined once real
  hardware numbers land — but they exist first, on purpose.

## Non-goals

- **Not an end-user SLA.** These bind the *engine's* internal design; a deployment's
  user-facing latency/throughput depends on its hardware, network, workload, and
  handler code — none of which this spec controls.
- **Not benchmarketing.** No cherry-picked configuration, no "up to X" peak quoted
  as typical. The reference machine and methodology are fixed and disclosed; the
  tail is always reported.
- **Not micro-optimizing every path.** Budgets target the **hot paths that
  dominate** (send, dispatch, schedule, serialize) — cold paths (startup, config,
  admin) are correctness-only.
- **Not cluster-scale SLOs.** Cross-node *throughput* budgets exclude network RTT
  precisely because RTT is the deployment's property, not the engine's; the engine
  budgets only its own overhead on top of the wire.

## Interaction

- **001 / 002 / 003** — the hot paths these budgets govern (dispatch, scheduling,
  descriptor/payload layout); the ≤ 64 B descriptor and 0-allocation gates enforce
  003's layout claims.
- **006** — local `tell`/`ask` latency and throughput budgets measure the send verbs
  directly.
- **009** — the metrics snapshot + latency histograms are the macrobenchmark's
  measurement surface; per-shard counters make the 0-cross-core-RMW gate checkable.
- **014** — the correctness counterpart; deliberately the *other* PAL backend
  (virtual vs. real clock). A design must pass both: right under sim, fast under
  native.
- **016** — the wire fast-path encode budget measures the tagless near-memcpy claim. **Proven** (ADR-016): tagless encode p99 25–28 ns, near-memcpy, 0 alloc, negotiation fallback + evolution correct; the 500 ns Hard ceiling is stamped, the 200 ns goal defers to reference silicon.
- **019** — defines the native-vs-sim backend seam and hosts the high-resolution
  timing primitive; the reference machine is an x86-64/ARM64 PAL target.
- **022** — the shed-don't-buffer / bounded-queue rules are what keep the
  throughput tiebreak from trading the tail away; governance and perf budgets are
  mutually reinforcing.
- **The `quark-prover` / `design-debate-prove` loop** — budgets are the pass/fail
  thresholds those agents grade against; without them the prover can only report.

## Dependencies

The engine gains **nothing** — the harness is dev-tooling, not runtime code.
Timing uses the PAL clock (019, no new dependency). A microbenchmark driver may
optionally use google-benchmark, but only as a **dev-only** tool never linked into
the engine; the default driver is an in-house timing loop over the PAL clock to
keep even the tooling low-dependency, consistent with the whole-RFC posture.

## Open questions

- **The ARM64 ratio, concretely** — is ≤ 1.5× latency / ≥ 0.66× throughput right
  across Graviton/Ampere/Apple, or does each need its own ratio? Requires real
  silicon to fix.
- **Frequency-scaling policy for repeatability** — turbo disabled (stable but
  unrepresentative) vs. turbo enabled (representative but noisy); or report both.
- **Canonical macro workloads** — which handful of workloads are the *official*
  regression set, and how they're kept representative as the engine evolves.
- **CI noise band** — the exact per-metric regression thresholds and machine-quiescence
  requirements that make ns-scale gating reliable rather than flaky.
- **Re-baselining cadence** — when provisional targets are replaced by
  hardware-measured ones, and the policy for tightening a Goal into a Hard ceiling
  once a design reliably beats it.
