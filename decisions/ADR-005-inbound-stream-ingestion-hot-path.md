# ADR-005 — Inbound stream-ingestion hot path (StreamChannel)

- **Status:** Accepted
- **Date:** 2026-07-14
- **Scope:** The **inbound stream-ingestion hot path** — how a high-rate / large /
  unbounded stream of frames reaches an actor *without* fanning one discrete
  mailbox descriptor (`Descriptor` + `MessageHandle`) per frame. This is the gap
  006 records as "streaming replies" and leaves unspecced for **inbound** streams.
  It **composes beside** the Accepted intrusive Vyukov mailbox (ADR-002/003/004),
  which is unchanged; it does not replace or re-open it.
- **Does NOT touch:** the mailbox MPSC core. The mailbox is settled. This ADR adds
  one descriptor *variant* and one drain routine next to it.
- **Related specs:** `001-Actor-Execution-Model.md`, `002-Scheduler.md`,
  `003-Memory.md`, `004-Resources.md`, `006-Messaging-and-Addressing.md`,
  `015-Reentrancy-and-Quiescence.md`, `022-Resource-Governance-and-Overload-Control.md`,
  `023-Performance-Targets-and-Budgets.md`. Introduces a new **024-Streaming** spec.

## Question

How does a high-rate inbound stream reach an actor without a per-frame descriptor
fan-out, while upholding every core invariant and the 023 streaming budgets?
A winner must, on x86-TSO (GCC 14.2 + Clang 20.1, `-O3`+LTO, pinned core):

- **(fast)** sustain ≥ 10 M small-frames/s/core (goal) / ≥ 4 M (hard), amortize the
  per-frame handoff far below a discrete `tell`, and meet the local-tell latency
  reference (p50 ≤ 100 ns goal / ≤ 250 ns hard; p999 ≤ 5 µs / ≤ 50 µs);
- **(safe)** 0 steady-state hot-path heap allocs (measured); 0 cross-core atomic
  RMW on the sequential consumer drain path (measured); race-free credit accounting
  and no lost wakeup under TSan;
- **(correct)** GATE-1 single-executor (a Suspended async handler must not advance
  the drain); GATE-2 FIFO end-to-end within a stream; GATE-3 flow-control-not-
  shedding (bounded memory, producer stalls, no mid-stream drop); GATE-4 no lost
  wakeup (reuse 002/ADR-004 seq_cst Dekker close-out); GATE-5 credit race-free —
  **without bending a core invariant** (at-most-one-executor, stable placement,
  workers-are-lanes-not-owners, std-only C++23, no heap on the steady hot path).

## Candidate designs (one-line summaries)

- **A — StreamChannel: single-descriptor credit-ring** *[winner].* One pre-allocated
  per-stream SPSC ring; two single-writer 64-bit monotone cursors (`head`/`tail`)
  make credit a **derived** quantity `window-(head-tail)` (no shared credit counter);
  ONE re-usable `StreamActivationDescriptor` posted on the same intrusive mailbox,
  only on the ring's empty→nonempty edge; consumer drain is pure plain load + copy +
  plain store (0 cross-core RMW/frame); x86 `armed.exchange` doubles as the Dekker
  StoreLoad arm; inline ≤ 56 B slots (zero-copy) or by-reference for large frames.
- **B — CREDIT-RING: ring as an Activation Resource (004).** Same SPSC-ring shape,
  modelled as a `Cached<>` activation resource; byte/slot credit window; **revised**
  under cross-examination to move all arm-RMW to the producer + add hysteresis arming
  + a producer-un-stall rendezvous + enforced single-writer token.
- **C — SPSC-CHANNEL: separate byte-credit return path.** SPSC frame ring plus a
  **third** byte-denominated `credit_returned_` counter separate from the slot ring,
  so a consumer retaining a zero-copy span across a `co_await`/forward does not let
  the producer overwrite live arena bytes; **revised** to a strict in-order-prefix
  reclaimer, a credit-return doorbell, a two-tail close-out, static (CRTP) dispatch,
  and a pmr coroutine-frame allocator.

## Evidence table

Claim kinds **F**/**S**/**C** = fast/safe/correct. *Survived* = survived red-team
cross-examination without being conceded/withdrawn (revised-and-surviving counts as
survived). *Proven* = executed-C++ verdict. Host: x86-64 Linux 32-core, producer /
consumer pinned to separate cores, warm, `-O3 -march=native -flto`, GCC 14.2 /
Clang 20.1, TSan+ASan+UBSan. TSC ~2.1 GHz base, **turbo off** — **not** the 023
Zen4/SPR 3–4 GHz reference core, so absolute ns are **conservative**; ratios,
0-RMW and 0-alloc gates are host-independent. All ARM/weak-memory sub-claims are
**INCONCLUSIVE** (no hardware) and carry no decision weight.

### Design A — StreamChannel (winner)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 ≥10M/s (goal), ≥3× below a tell | yes (+layout fix) | **CORRECT** | STREAM 30.25 M/s g++ / 57.12 M/s clang vs TELL 2.40 / 2.89 M/s → amortized **12.6× / 19.7×** (≫3×); both ≫10M goal. Layout fix load-bearing: 30.25 > 27.94 (BAD colocated `credit_limit`) |
| F2 drain-step + signal→handler latency | yes (mixed-load ceiling withdrawn) | **CORRECT** | drain-step p50 **18.1 / 15.3 ns**, p99 397 / 128, p999 4301 / 2224 (goal p50≤100, p999≤5µs met, incl ~10ns rdtsc); signal→handler isolated p999 **959 / 813 ns** (hard ≤50µs met) |
| S1 0 lock-RMW + 0 fence per frame | yes | **CORRECT** | objdump both compilers: per-frame loop = plain `mov` only, 0 lock/mfence/xchg/cmpxchg; runtime lock-ops = **O(empty-transitions), not O(frames)** (457k g++ / 1.55M clang over 10M coupled; 0 in controlled steady-nonempty) |
| S2 0 hot-path heap alloc / 50M frames | yes | **CORRECT** | global-`new` trap armed after warmup, 50M window: **0 allocs**, both compilers. Idle-density graded (not gated): 60K streams/GB@cap256 — below 023's 1M/GB target (documented footprint tax) |
| S3 race-free + Dekker load-bearing | yes | **CORRECT** | 5M isolating Dekker trials: WITH fence **lost=0**; WITHOUT lost=705 (g++) / 226 (clang) → fence load-bearing. TSan **0 races / 10M**, delivered==pushed. Credit derived from two single-writer cursors → GATE-5 impossible-by-construction |
| C1 FIFO end-to-end (GATE-2) | yes | **CORRECT** | 10M monotone-seq frames: order_violations=0, next_dispatch==10,000,000==N; holds across batch-flush + park boundaries, K=1000 and K=97, TSan clean |
| C2 GATE-3 flow-control (ours), operational vs baseline | yes (overclaim withdrawn) | **CORRECT** | ring: delivered=20M, **dropped=0**, max occupancy **256==capacity** (bounded), 171–176M producer stalls (lossless). Baseline `DropOldest` sheds **3.9–4.8M** mid-stream. Baseline-"violates-GATE3" reframed as operational, not a gate verdict |
| C3 GATE-1 + suspend-parks + no lost wakeup (GATE-4) | yes (split-cursor fix) | **CORRECT** | split `disp`/`tail` cursors: concur_violations=0, completed==N, exactly-once, **no watchdog HANG** (fill-to-stall then complete → full window drains). Negative control (single-tail) **WEDGES** (timeout 124) → fix necessary, detector non-vacuous |

**Design A: 8/8 survived and proven CORRECT; 0 disproven, 0 inconclusive.** Four
cross-examination holes (single-tail resume double-dispatch, `credit_limit`/`armed`
false sharing, C2 correctness-overclaim, F2 mixed-load ceiling) were conceded and
repaired; every repair carries a fired positive control. No safe/correct claim WRONG.

### Design B — CREDIT-RING (revised)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 0 consumer-RMW + ≥4M floor | yes (fatal fix: producer-only arm-RMW) | **CORRECT** | consumer-path RMW on `stream_state_` = **0** all operating points (negative control submitted-protocol = 0.30/frame); throughput **4.05–6.95 M/s** → clears 4M **hard floor, MISSES 10M goal**. CAVEAT (reported): signal→handler **p50 297–421 ns — breaches the 023 250ns hard ceiling** for tiny frames |
| F2 zero-copy ≥2.5× at F≥512B | yes | **WRONG** | g++ 512B ratio **1.80–1.92× (<2.5×)** — falsified on its own criterion (reproduced twice). Qualitative monotonic-in-F survives (2KB 3.59×, 16KB 26.9×); the 512B crossover is compiler-dependent (clang passes 3.10×) |
| C1 GATE-3 bounded + liveness | yes (+un-stall rendezvous) | **CORRECT** | max_inflight==window==1024, dropped=0, produced==delivered=5M, producers parked+woken; baseline sheds 26–44%; wedge control (no notify) stalls at 1024/5M → reverse edge load-bearing |
| C2 0 alloc + max-frame bound | yes | **CORRECT** | 0 allocs/frame over 1M × {inline, REF, copy-fallback, 4KB}; over-max → typed `std::unexpected` at admission (not alloc, not drop) |
| S1 GATE-4 + single-membership | yes | **CORRECT** | 5M isolating Dekker: fence lost=0, no-fence 0.21% (g++)/0.38% (clang); 400k real-mailbox disarm-window: 0 lost, single-membership held |
| S2 GATE-5 + enforced SPSC | yes | **CORRECT** | TSan clean 3M, granted==returned, monotonic; 2nd bind → typed error; 2-writer control → TSan race + 30282 lost updates (precondition load-bearing) |
| S3 GATE-1 + suspend cursor + FIFO | yes (submitted bug conceded+fixed) | **CORRECT** | 4-lane contention max_concurrency==1, exactly-once, 0 inversions; corrected dispatch-cursor: double_dispatch=0, cursor_regress=0. (Full 015 re-entry modelled in-thread, not wired to real scheduler this round) |

**Design B: 6 survived + proven CORRECT, 1 disproven (F2), 0 inconclusive.** Its
0-RMW gate holds only after a fatal-severity revision (producer-only arm-RMW +
required hysteresis); it **misses the 10M throughput goal** (4M floor only) and its
**tiny-frame signal→handler latency breaches the 023 250ns hard ceiling**.

### Design C — SPSC-CHANNEL (revised)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| C4 separate byte-credit REQUIRED for zero-copy | yes (prefix-clamp fix) | **CORRECT** | ASan: head-as-credit variant **use-after-poison WRITE size 256** (both compilers); prefix-clamped byte-credit stalls (frame4 admitted=0), no overwrite; copy-mode collapses the two counters |
| C1 GATE-3 flow-control + live | yes (+doorbell) | **CORRECT** | 3M offered==3M delivered, drops=0, in-flight pinned 65536B==byte_budget; no-doorbell EPOLLET control strands at 692k/482k; per-tell baseline sheds 2.99M/3M |
| C2 GATE-4 two-source no lost wakeup | yes (two-tail close-out) | **CORRECT** | 300k StoreLoad-isolation trials: strandings=0 with fence+both-tail probes; drop-stream-probe 107/49, drop-mb-probe 63/48, remove-fence 111/343 (controls fire) |
| C3 GATE-1 single-executor + suspend | yes | **CORRECT** | head_/credit_returned_ byte-identical across suspension, 2nd worker CAS loses, inflight never >1; 015 re-admit drains remainder |
| S3 GATE-5 3-thread multi-writer | yes (blanket claim narrowed) | **CORRECT** | 3-thread TSan (stream+mailbox+consumer): 0 races on all 4 atomics, conservation_breaches=0, final_inflight=0 |
| S1 0 global alloc sync+async | yes (pmr coro alloc added) | **CORRECT** | sync 0 allocs/10M; async co_await (HALO-defeated) with pmr shard promise alloc **0 global allocs/10M**; control without pmr = 10M (requirement real) |
| S2 0 cross-core RMW on drain | yes (+layout fix) | **CORRECT** | objdump: `do_drain` 0 lock-ops, 0 indirect calls (CRTP inlined); only locked RMW is the per-batch producer exec_state CAS, never in the drain loop |
| F1 ≥10M/s + ≤5ns/frame | yes (CRTP fix) | **CORRECT** | throughput {6.8–9.9, 25–31, 58–86, 102–121} M/s @batch{1,8,32,128} → goal met at batch≥8, floor everywhere. CAVEAT: ≤5ns holds only copy-mode; **zero-copy drain with the required prefix-clamp reclaimer is 21–22 ns/frame** |
| F2 cross-core p50≤100ns | yes | **INCONCLUSIVE** | same-shard sequential p50=43.9 / p99=110.7 ns (meets 023); cross-core hardware-floored at min=114 / p50~340 ns on the 2.1GHz rig — reference silicon unavailable → unconfirmed |
| F3 ≥10× cheaper than a tell @batch≥32 | yes | **WRONG** | zero-copy full drain 22.8 ns@b32 vs tell 45.7 ns → **~2.0×, not ≥10×**. The prefix-clamp reclaimer that C4 proves REQUIRED for zero-copy is the tax that defeats the 10× thesis in the very regime the design exists to serve |

**Design C: 8 survived + proven CORRECT, 1 disproven (F3), 1 inconclusive (F2).**
Its byte-credit-separation *insight* (C4) is real and proven; but its headline
amortization thesis (F3 ≥10×) is **disproven in the zero-copy regime that alone
justifies its extra counter**, and its cross-core latency is INCONCLUSIVE.

## Decision

**Winner: Design A — StreamChannel (single-descriptor credit-ring). Promote to a new
024-Streaming spec (Draft → Accepted on x86-TSO).** Ranking of survivors:
**A > SPSC-CHANNEL (C) > CREDIT-RING (B).**

Rationale, in the required ranking order:

1. **Safety gate.** No design has a *safe/correct* claim marked WRONG, so none is
   disqualified by the strict gate. But A is the only one that clears it with **zero
   revisions to a safety claim's substance and zero fired-fatal repairs on the
   winning path** — S1/S2/S3/C1/C2/C3 all CORRECT first pass (C3's split-cursor and
   the false-sharing layout fix were the two repairs, each control-backed). B needed
   a **fatal-severity** rework (the submitted consumer `exchange` was a real
   0-RMW-gate failure at the natural operating point) to pass; C shipped a **fatal**
   credit-arithmetic use-after-overwrite (C4) that ASan tripped and only a
   prefix-clamp repaired.

2. **Proven beats claimed.** A = **8 survived + proven CORRECT, 0 disproven, 0
   inconclusive** — a clean sheet. C = 8 CORRECT but **1 disproven (F3)** and **1
   inconclusive (F2)**; both count against / carry no weight. B = 6 CORRECT and **1
   disproven (F2)**. On proven-claim count and disproven-count, A strictly dominates.

3. **Best measured hot-path numbers.** A wins the tiny-frame streaming path
   outright and hits **every** 023 gate with measured evidence: **30.25 / 57.12
   M/s** (≫10M goal), amortization **12.6× / 19.7×** below a tell (proven, vs C's
   F3 ~2.0× disproven), drain-step **p50 15–18 ns** (≤100 ns goal), signal→handler
   p999 **813–959 ns** (≤50 µs hard), **0 allocs / 50M**, **0 cross-core RMW/frame**
   (objdump both compilers). B **misses the 10M goal** (4M floor only) and its
   tiny-frame signal→handler **p50 297–421 ns breaches the 023 250 ns hard latency
   ceiling** — under 023 a Hard-ceiling miss is a rejection for that path. C posts
   higher *raw* throughput (102–121 M/s @batch128) but its per-frame *cost advantage*
   collapses to ~2× in the zero-copy regime and its cross-core latency is
   unconfirmed on the reference silicon.

4. **No core invariant bent.** A upholds at-most-one-executor (drain under the
   exec-state CAS; a Suspended handler advances neither cursor — C3 proven), FIFO-by-
   default (single-writer monotone cursors — C1 proven), stable placement and
   workers-as-lanes (the activation rides the exec-state, not the ring), std-only
   C++23, and 0 heap on the steady hot path (S2 proven). B and C each pass invariants
   too, but only after conceding a fatal defect on the path that would have bent one
   (B's per-frame arm-RMW = the fan-out it exists to kill; C's overwrite of a live
   retained span = an unbounded-memory / GATE-3 correctness break).

### What the two losers contribute to the spec anyway

- **From C (SPSC-CHANNEL): the byte-credit insight is adopted as an opt-in mode.**
  C4 is *proven*: when a zero-copy transport lets a handler retain a span across a
  `co_await` or a downstream forward, slot-count credit (A's implicit
  `window-(head-tail)`) is **unsafe** — the producer can overwrite live arena bytes.
  A's inline-slot default is immune (it copies ≤ 56 B into the ring, so no live
  external span exists), but A's **large/by-reference** slot variant inherits exactly
  C's hazard. 024 therefore adopts C's **strict in-order-prefix byte-credit counter
  as a `StreamMode::ZeroCopyRetained` policy** layered on A's ring, compiled away in
  the inline/copy default. This is a proven, load-bearing addition, not a hedge.
- **From B (CREDIT-RING): producer-only arm-RMW + required hysteresis + a producer
  un-stall rendezvous.** B's fatal-then-fixed F1 is the sharpest lesson: under GATE-3
  flow control the ring naturally sits near-empty, so any *consumer-side* arm RMW
  fires ~per-frame and breaks the 0-RMW gate. A already keeps the consumer re-arm as
  a plain store + the exchange on the producer, but 024 must **normatively forbid a
  consumer-side RMW on the drain path** and require **hysteresis (low-watermark)
  arming** so the emergent batch is large enough to chase the 10M goal. B's
  producer-un-stall Dekker (waking a credit-starved producer) is also adopted:
  A's `try_push` returns `false` on stall; the spec must define the reverse wakeup
  edge so a stalled producer is not left wedged under edge-triggered readiness.

## Residual risks (carried into 024)

1. **Async-suspend resume seam (GATE-1/GATE-4).** A's winning C3 proves the
   split-cursor (`disp` / `tail`) resume and the no-wedge property with an in-thread
   resolver and a fired wedge control — but the **015 admission-gate integration**
   (the stream-descriptor-aware continuation that re-enters `StreamChannel::drain`
   on the actor's lane without re-enqueuing `desc`) was modelled, not wired to the
   real scheduler. Promotion of the *suspend path specifically* past Draft needs an
   ADR-004-grade run against the real 002 scheduler + 015 gate.
2. **Idle-density footprint tax.** 64 B cache-line slots × cap-256 = 16 KB/stream →
   ~60 K streams/GB, ~17× under 023's 1 M-activations/GB idle target. Mitigations
   (small default cap governed by `credit_limit`, shared slot slab, packed slots)
   each trade against near-empty false sharing and must be `perf c2c`-measured before
   adoption. Graded, not gated — but real for many-small-streams workloads.
3. **SPSC precondition is load-bearing and needs runtime enforcement.** GATE-5 holds
   only for one producer per stream; a stream-open single-writer token (typed 007
   error on double-bind + debug assert), proven load-bearing by B's 2-writer control,
   must be specified. Multi-source fan-in stays on the mailbox.
4. **Reference-silicon re-measurement.** All absolute ns are on a conservative
   ~2.1 GHz turbo-off rig, not the 023 Zen4/SPR core. Ratios, 0-RMW and 0-alloc gates
   are host-independent and PASS; absolute latency percentiles must be re-run on the
   reference machine before the 024 Hard-latency budget is finally stamped.
5. **Weak-memory (AArch64) INCONCLUSIVE.** The `armed.exchange`-as-Dekker arm relies
   on x86 `lock xchg` being a full StoreLoad barrier; on ARM it needs an explicit
   seq_cst fence / seq_cst exchange. Per ADR-004 posture this is deferred (no
   hardware) and must be herd7/GenMC litmus-checked before any ARM claim; it does not
   decide this ADR.
6. **Zero-copy suspend HOL (only if `ZeroCopyRetained` is used).** A suspended handler
   pins its referenced RX buffer; on a shared provided-buffer pool one slow consumer
   can starve unrelated streams. 024 must cap per-stream pinned buffers or force a
   copy-out on suspend (adopted from C's F2 residual).

## Spec recommendations

See the "Spec recommendations" section of the closing summary — 024-Streaming (new)
plus targeted edits to 006, 004, 003, 002, 001, 022, 023.
