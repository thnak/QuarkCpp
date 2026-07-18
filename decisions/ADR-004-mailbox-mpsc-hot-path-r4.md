# ADR-004 — Mailbox MPSC hot path (evidence round 4: SEG-FAA + REX-CAS challenge)

- **Status:** Accepted
- **Date:** 2026-07-14
- **Confirms:** [ADR-003](ADR-003-mailbox-mpsc-hot-path-r3.md),
  [ADR-002](ADR-002-mailbox-mpsc-hot-path-r2.md), ADR-001. The winner is
  **unchanged for the fourth independent evidence round**: the incumbent
  **Intrusive Vyukov MPSC** mailbox. What is new this round: two fresh
  challengers were built, red-teamed, and proven against the incumbent —
  **SEG-FAA** (segmented fetch-add ring, the revived Design-B lineage) and
  **REX-CAS** (a Treiber-CAS reincarnation of the rejected REX). Neither clears
  the bar to reopen ADR-002/003. Four incumbent hardening changes surfaced by
  executed C++ this round are promoted to normative spec changes, and one
  liveness overclaim in 015 is corrected.
- **Scope:** The per-actor Mailbox — the MPSC queue that owns FIFO ordering,
  stores fixed-size `MessageHandle`s (never payloads), enqueued by many producer
  threads, drained by exactly one worker (single-executor invariant).
- **Related specs:** `001-Actor-Execution-Model.md`, `002-Scheduler.md`,
  `003-Memory.md`, `015-Reentrancy-and-Quiescence.md`, `022`, `023`.

## Question

Does any new mailbox design clear the bar to reopen the Accepted ADR-002/003
incumbent? A winner must be **(fast)** allocation-free and non-collapsing under
producer contention, **(safe)** data-race / UB / ABA-free under TSan+ASan+UBSan,
and **(correct)** strict FIFO with no lost/duplicated handles and tombstones
skipped exactly once — without bending a core invariant (at-most-one-executor,
FIFO-by-default, no heap on the steady hot path, no data race under sanitizers,
and 015's rule that *a stall must never extend past the one message being
published*).

## Candidate designs (one-line summaries)

- **A — Intrusive Vyukov MPSC (exchange-published, stub-anchored, gen-gated)**
  *[incumbent].* Queue node *is* the pooled `Descriptor` (8-byte pointer-inter-
  convertible `MailNode next` first member). Producers publish wait-free with one
  `tail_.exchange(acq_rel)` + one release link store — ABA-free by construction
  (never compares an address). Single consumer walks a plain private `head_` with
  zero cross-core RMW; handoff rides the exec-state CAS. Transient publish window
  surfaced as `Busy`. Generation-gated packed-CAS tombstone cancel.
- **B — SEG-FAA (segmented fetch-add ring)** *[challenger].* Producers claim a
  slot with one `fetch_add` on a segment producer counter; cacheline-isolated
  slots store the handle **by value** (abandons 003's intrusive layout). Single
  consumer batch-sweeps with acquire loads (zero drain RMW). Revised under
  cross-examination to add **EBR-lite** segment reclamation + **reset-under-grace**
  after two conceded fatals (recyclable-pointer hazard; missing segment reset).
- **C — REX-CAS (Treiber-push + in-place reversal)** *[challenger].* Producers
  push onto a LIFO Treiber stack with a **classic link-then-publish CAS** (fixing
  the exchange-REX publish gap); single consumer detaches the whole batch with one
  `exchange(nullptr)` and **reverses it in place** into a private FIFO cursor.

## Evidence table

Claim kinds **F**/**S**/**C** = fast/safe/correct. *Survived* = survived red-team
cross-examination without being conceded/withdrawn. *Proven* = executed-C++ verdict.
Host: dual-socket **Intel Xeon Silver 4208** (2×8c, ~2.095 GHz, throttled — **not**
the 023 Zen4/SPR reference core, so absolute ns are conservative; ratios and
structural properties are host-independent). g++ 14.2 / clang 20.1, `-std=c++23`,
TSan+ASan+UBSan, `-O3` (sanitizer runs `-O1`).

### Design A — Intrusive Vyukov (incumbent, re-proven with round-4 hardening)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free wait-free enqueue | yes | **CORRECT** | asm = `1 xchg + 2 stores + ret`, no loop, no cmpxchg (both compilers); 0 new / 0 pmr per msg, P=1..128, 8M each |
| F2 saturation-without-collapse | yes (scoped) | **CORRECT** | ≥20 Mops for P≥4 (22.4→33.1); non-collapse P128/P16 = 1.04; honest: CAS-loop wins P=1 |
| F3 zero-RMW drain + local-tell latency | yes (+FS fix) | **CORRECT** | `try_dequeue` pop loop: 0 lock/xchg/cmpxchg (1 xchg only in empty-boundary stub re-arm); **p50=59 / p99=75 / p999=182 ns** (budget 100/250/50000); cross-core occ-1 p50=364 / p999=2058 ns |
| S1 no race/UB on `head_` handoff | yes | **CORRECT** | rel/acq exec-CAS: TSan+ASan+UBSan clean 10M rotating consumer, both compilers; **control**: relaxed exec-CAS → `head_` race in `try_dequeue` every run |
| S2 ABA-free by construction (hot path) | yes (x86-TSO) | **CORRECT** | enqueue cmpxchg count = 0; 16M same-addr reuse at P=32 → dup/miss/stale = 0. AArch64 litmus NOT run (tooling absent) |
| S3 gen-gated cancel = defined no-op (**48-bit gen**) | yes (width fix) | **CORRECT** | 48-bit packed CAS: 8M cancel-vs-reclaim ASan+TSan clean, wrongly_cancelled=0; **control A** bare ptr → heap-UAF; **control B** two-atomic → 17,497 TOCTOU; **width control** u8 → wrong-cancel fires |
| C1 strict FIFO, no loss/dup | yes | **CORRECT** | K=8/16, 16M: inversions=0, dup=0, missing=0, produced==dispatched. AArch64 cross-check NOT run |
| C2 tombstone exactly once (+**budget-counted**) | yes | **CORRECT** | 2M 50%-cancel: free_count==1, handler_ran=0; race-injected claim-CAS-fail → 0 run-cancelled, 0 double-reclaim; 10M all-cancel drain → BudgetExhausted after 1024 skips in 21 µs (not O(N)) |
| C3 no lost wakeup / Busy≠Empty (**symmetric fence**) | yes (fence fix) | **CORRECT** | symmetric store+fence+load Dekker: 0/300k lost, both compilers; 800k Busy probes, empty_misread=0; **control** no-fence → nonzero lost. **Honest**: x86 magnitude ~0.05–0.09%, NOT the ~48% (that needs both sides downgraded); GenMC AArch64 litmus NOT run |

**Design A: 9/9 survived and proven CORRECT; 0 disproven.** Four round-4 hardenings
(48-bit generation, own-cache-line stub, symmetric store+fence+load Dekker,
budget-counted tombstone skips + claim-CAS-fail routing) each carry a fired positive
control. No safety/correctness claim WRONG.

### Design B — SEG-FAA (challenger, revised)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free steady state | yes | **CORRECT** | 270 heap allocs / 100M msgs; post-warmup 1.0e-6 allocs/msg; 99.86% of segment transitions are EBR recycles |
| F2 scales/flatter p99 under contention | **NO — CONCEDED** | *(no weight)* | conceded: producer claim is one `lock xaddl` on one line (≈ Vyukov `xchg`) + extra `tail_seg_` load; aggregate plateaus ~4–6 M/s, **does not scale with P** |
| F3 zero-RMW drain | yes | **CORRECT** | drain body loads only, 0 lock-prefixed insns (EBR RMW only on the excluded boundary path) |
| S1 no race/UB (revised) | yes | **CORRECT** | 64×1M ASan+UBSan clean; TSan clean 3.2M; both compilers |
| S2 seq_cst Dekker loses 0 wakeups | yes | **CORRECT** | seq_cst 0/300k; rel/acq 144054/300k (48.0%) — matches ADR-003 |
| S3 no-hazard-pointer reclaim | **NO — CONCEDED**, revised → **S3R** | **CORRECT (revised)** | original boast withdrawn (recyclable-pointer hazard real); **EBR-lite** revision: poison_hits=0/3.4M, EBR-OFF contrast fires (5 hits). **Caveat**: proven with *symmetric seq_cst* EBR model, **not** the production asymmetric membarrier (deferred) |
| C1 exactly-once FIFO across recycles | yes (reset fix) | **CORRECT** | 64M through ≥3.7M recycles: dups=0, order_violations=0, missing=0 |
| C2 tombstone exactly once | yes | **CORRECT** | 8M cancel-race: packed CAS stale_writes=0; nonpacked contrast → 126,777 |
| C3 Busy≠Empty, single-publish stall | yes | **CORRECT** | 20k gap trials: Empty-in-Busy=0, order violations=0, max blocked-on = 1 publish |

**Design B: 8 survived + proven CORRECT, 0 disproven; F2 CONCEDED (0 weight).** But
its selling point (contention scaling) is the conceded claim; the **proven-safe build
is not the production hot-path build** (symmetric seq_cst EBR vs deferred asymmetric
membarrier); and its **measured p999 breaches the 023 Hard tail ceiling** (below).

### Design C — REX-CAS (challenger)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1p single-word CAS loop, no DWCAS | yes (re-scoped) | **CORRECT** | 1 relaxed load + 1 store + 1 8-byte `lock cmpxchg`; 0 cmpxchg16b/casp. (within-10% quant. withdrawn) |
| F2 honest loss under contention | yes | **CORRECT** | producer p99 worse for N≥8 (2476 vs 1105 ns @N8; 3.51× @N16); retry-ratio→2.68; livelock-free not wait-free |
| **F3 within 5% drain throughput at B≥64** | yes | **WRONG** | REX/Vyukov drain ratio B64=0.846, B1024=0.776, B16384=0.708 — 15–29% **below**, widening with B. Falsified on its own stated criterion |
| S1 no race/UB | yes | **CORRECT** | TSan+ASan+UBSan clean; teeth control (relaxed) → 3 races every run |
| S2 ABA benign (unconditional detach) | yes | **CORRECT** | 10M reincarnated-address stress: lost=0, dup=0, cycles=0; double-membership control detects corruption |
| **C1 no head-of-line freeze** | **NO — CONCEDED (fatal)** | *(withdrawn)* | O(batch) in-place reversal: detach→first-dispatch 4.4 µs @B1024, 254 µs @B16384, 39 ms @B262144 vs Vyukov flat ~40 ns. **Violates 015** |
| C1p preemption-independence (narrowed) | yes | **CORRECT** | at fixed B, dispatch flat in T (32 µs for T=1µs..10ms); exchange-REX scales with T (10.1 ms @T=10ms) |
| C2 strict FIFO | yes | **CORRECT** | 100M: fifo_violations=0, lost=0, dup=0 |
| C3 tombstone exactly once | yes | **CORRECT** | 100M cancel-race: disjoint, lost=0, dup=0, double_free=0 |

**Design C: 7 survived + proven CORRECT, 1 disproven (F3), 1 conceded fatal (C1).**
F3 WRONG and C1's O(batch) reversal reintroduces the exact head-of-line-latency class
015 forbids — REX-CAS **withdrew itself as default**, retaining only an optional
deep-backlog draining mode.

## Decision

**Winner: Design A — Intrusive Vyukov MPSC. ADR-002/003 confirmed for a fourth
round. Neither SEG-FAA nor REX-CAS clears the bar to reopen it.**
Ranking of survivors: **A > SEG-FAA > REX-CAS.**

Rationale, in the required ranking order:

1. **Safety gate.** A: no safe/correct claim WRONG; all of S1/S2/S3/C1/C2/C3 proven
   CORRECT, each round-4 fix backed by a fired control. SEG-FAA also passes the gate
   (S1/S2/S3R/C1/C2/C3 CORRECT) but only after **two conceded fatals** were repaired
   by structural revision, and S3R is proven with a *conservative model* (symmetric
   seq_cst EBR), not its production asymmetric-membarrier hot path — the deferred half
   is exactly the weak-memory reclamation hazard. **REX-CAS is out on Rule 4** (below):
   C1 was conceded fatal, reintroducing the 015-forbidden head-of-line stall, and its
   F3 is disproven.

2. **Proven beats claimed.** A = **9 survived + proven CORRECT, 0 disproven**.
   SEG-FAA = 8 proven CORRECT but its headline reason to exist — **F2, contention
   scaling — was CONCEDED (0 weight)**; measured aggregate throughput plateaus ~4–6
   M/s and does *not* scale with producers, i.e. it buys nothing over the incumbent on
   the axis it was designed to win. REX-CAS = 7 proven CORRECT, **1 disproven (F3)**,
   1 conceded fatal (C1).

3. **Best measured hot-path numbers on the budget-defining path.** The 023 local-tell
   budget is the **sequential same-shard** path. A wins it outright:
   **p50=59 / p99=75 / p999=182 ns**, drain **0 cross-core RMW**, cross-core occ-1
   p999=2058 ns (all within budget). SEG-FAA's proven-safe build posts **100–107 ns**
   local tell (production relaxed-EBR 74 ns) — slower than A — and, decisively, its
   **p999 blows the 023 Hard tail ceiling**: p999 rises to **2.25 ms** at 64:1
   producer:consumer oversubscription (ceiling is **≤ 50 µs**), a 45×-over-ceiling
   miss driven by a single-consumer plateau + pool-mutex convoy at segment boundaries.
   Under 023 a Hard-ceiling miss is a rejection. REX-CAS's O(batch) reversal produces
   32 ms detach→first-dispatch at deep backlog — the same class of miss.

4. **No core invariant bent — and both challengers carry a disqualifying edge.**
   A upholds at-most-one-executor (`head_` fenced by the exec CAS), FIFO-by-default
   (`tail_` exchange mod-order), zero-alloc (disassembly), and — with the round-4
   liveness correction (below) — 015's stall rule, honestly scoped. SEG-FAA **abandons
   003's intrusive layout** (by-value 64 B-isolated slots → ~16× over the 023 2 KB
   idle-footprint ceiling before its geometric-sizing revision, which is *design-level*
   and not carried in the executed footprint evidence) and misses the Hard tail
   ceiling. REX-CAS **bends 015's normative stall rule**: LIFO-then-reverse makes the
   oldest, often most latency-critical message pay the full O(batch) reversal — the
   defect its lineage always loses on.

### Honest corrections recorded this round

- **015 liveness overclaim corrected.** ADR-003 and 015 §77–84 assert the incumbent's
  mid-publish window "stalls **only the newest node**." Executed cross-examination
  shows this is **false in general**: because the consumer walks `head_` following
  `.next` and the link `n_{k-1}→n_k` is written by `n_k`'s producer, a **non-last**
  producer preempted in its exchange→link gap strands the entire already-linked
  **suffix** `n_k..n_n`, for a scheduler quantum, under P≫cores oversubscription.
  Corrected wording: *stalls the suffix at and after the preempted node; all strictly-
  older already-linked work drains from `head_` unobstructed.* This is still strictly
  better than REX (which froze the whole actor including the already-drainable prefix),
  so REX's rejection stands on corrected grounds — but the ~708 µs-magnitude tail is
  now honestly acknowledged as a **tail-latency exposure** (not a correctness bug;
  the suffix still dispatches in FIFO order after the quantum), mitigated by `Busy` +
  002 bounded-spin-then-reschedule + 022 admission.
- **The ~48% lost-wakeup magnitude is x86-refuted.** The Dekker control loses ~48%
  only when **both** sides are downgraded to release/acquire (SEG-FAA S2 reproduced
  144054/300000). On the incumbent, whose producer keeps the `acq_rel` exchange (a
  full barrier on x86), removing only the fences leaks ~0.05–0.09% (130–277/300k). The
  StoreLoad barrier is load-bearing either way (lost>0 without it, 0 with it); only the
  *magnitude* claim needed correction so CI guards are not calibrated to a wrong number.

### Confidence and the one residual tie-breaker

Decisive on x86-64 — the **fourth** independent round to reach the same winner. The
single experiment that could still move the *margin* (not the winner) is unchanged:
**A on a weakly-ordered AArch64 box, with the enqueue publication edge, the symmetric
store+fence+load Dekker, and the exec-state handoff encoded as herd7/GenMC litmus**,
plus an ARM produced-vs-dispatched bitset. herd7/GenMC/AArch64 were absent this round;
all sanitizer evidence is x86-TSO, structurally blind to a missing acquire. A's
contention advantage is expected to widen on ARM.

## Spec-update recommendations

See `specRecommendations` in the structured output. Summary:

- **003-Memory.md** — (1) **Widen the generation field to 48 bits** inside the packed
  `{generation:48, state:4, flags:12}` `uint64_t`; `MessageHandle.generation` becomes a
  48-bit-masked value. `u32` wraps in ~24 h at 50 M msg/s over a 1024-slot pool, letting
  a stale handle wrongly cancel a live message (silent lost message); the u8 width
  control fired this exact bug, 48 bits = ~179 years/slot. Keep it a **single packed
  CAS** (no DWCAS, TOCTOU-closed). Update §15–20 and §119–133. (2) **Put the stub
  sentinel on its own cache line** (`alignas(64) Descriptor stub_`), enforced by
  `static_assert(offsetof(Mailbox,stub_) - offsetof(Mailbox,head_) >= 64)`. The embedded
  stub shared `head_`'s line and is producer-written on every idle→active re-arm, false-
  sharing the consumer-private `head_` (perf-c2c-confirmed); the fix isolates the cross-
  core occ-1 tail. Add to §37–82.
- **002-Scheduler.md** — (1) **Specify the close-out rendezvous as the canonical,
  ISA-independent `store + seq_cst fence + load` on BOTH sides** (consumer:
  `exec_state.store(Idle, release); fence(seq_cst); tail_.load(acquire)`; producer:
  `tail_.exchange(acq_rel); fence(seq_cst); exec_state.load(acquire)`), replacing the
  asymmetric seq_cst-atomic/seq_cst-fence mix in §41–68. Proven 0/300k lost, keeps the
  enqueue exchange at `acq_rel` (no per-enqueue seq_cst cost on ARM). (2) **Correct the
  Dekker-control magnitude note** (§58–60): ~48% loss requires downgrading *both* sides;
  with the `acq_rel` producer exchange retained the x86 leak is ~0.05–0.09%. The
  StoreLoad barrier is load-bearing regardless; calibrate the CI guard to `lost==0` vs
  `lost>0`, not to 48%. (3) **Tombstone skips MUST count against the drain budget**
  (§129–150 Fairness): a mass-cancel of N tombstones with budget-free skipping
  monopolizes the lane O(N) (proven: 10M all-cancel → BudgetExhausted after 1024 skips
  in 21 µs once skips are budgeted).
- **001-Actor-Execution-Model.md** — (1) **State the close-out as symmetric
  store+fence+load** consistent with the 002 change (§52–60 currently reads the
  asymmetric form). (2) **Specify the claim-CAS-failure branch**: when a late cancel
  wins the race between the drain's `gen_state.load` and the `Queued→Running` claim-CAS,
  route it to **reclaim-as-tombstone** (one free, no handler, no second reclaim) — add
  to §87–103 near the suspended-handler park rule. (3) Reflect the **48-bit generation**
  in the Cancellation section (§98–103).
- **015-Reentrancy-and-Quiescence.md** — **Correct the head-of-line note (§77–84):**
  replace "stalls **only the newest node**" with "stalls the **suffix at and after** the
  preempted node; strictly-older already-linked work drains from `head_` unobstructed."
  Keep REX as the rejected alternative (it froze the whole actor including the drainable
  prefix). Reaffirm the mitigation (`Busy` + 002 bounded-spin-reschedule) and that
  tombstone skipping consumes drain budget (§124–126), cross-referencing the 002 change.

## Residual risks

1. **Weak-memory (AArch64) remains tool-unproven** for all three designs. A's
   symmetric store+fence+load Dekker, its `acq_rel` enqueue publication edge, and
   SEG-FAA's asymmetric-membarrier EBR are all argued, not machine-checked (herd7/
   GenMC/AArch64 absent). Run the litmus + an ARM produced-vs-dispatched bitset before
   committing cross-platform numbers. Unchanged from ADR-002/003; the single margin-
   moving experiment.
2. **48-bit generation horizon is finite** (~179 years/slot at 50 k reuses/s). Beyond
   any process lifetime, but the packed-word bit budget (`{48,4,12}`) is now fixed —
   any future flag/state expansion must not steal generation bits below 48.
3. **Own-cache-line stub costs one cache line per mailbox.** Trivial next to the
   density budget, but note it in the 003 footprint accounting.
4. **seq_cst StoreLoad + budget-counted tombstone skip are load-bearing.** Reverting
   the fence loses wakeups; reverting the budget count reopens the O(N) lane-monopoly.
   Both must be permanent CI regression guards (alongside the relaxed-CAS `head_`
   guard).
5. **Incumbent mid-publish suffix stall** (the corrected 015 property): a non-last
   producer preempted in its exchange→link gap strands the linked suffix for a
   scheduler quantum under P≫cores oversubscription. Tail exposure, not a correctness
   bug; mitigated by `Busy` + 002 + 022. Preserve `Busy`/bounded-spin in any future
   change.
6. **Unbounded queue = no backpressure** (unchanged; 022 companion policy). SEG-FAA's
   revised bounded-cap-on-overflow is *not* adopted with it and is noted only as a
   022-aligned property the challenger offered.
7. **Harvestable, not adopted.** REX-CAS's RMW-free bulk-detach deep-backlog drain
   (45–59 Mops at B≥64, but O(batch) latency-to-oldest) and SEG-FAA's cacheline-
   isolated by-value slots remain optional, opt-in draining/layout modes behind the
   same `MessageHandle` contract; if pursued they must be re-proven on the isolated 023
   reference core and on AArch64. Same disposition ADR-002/003 gave the REX drain sweep.
