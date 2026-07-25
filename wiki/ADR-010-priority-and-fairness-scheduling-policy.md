# ADR-010 — Priority & Fairness Scheduling Policy

Status: **Accepted**
Date: 2026-07-15
Deciders: design-debate-prove judge (closing the 002 priority/fairness open question)
Supersedes/relates: builds on ADR-002/003/004 (mailbox MPSC hot path + `seq_cst`
Dekker close-out), ADR-007 (CRTP policy authoring), ADR-008 (per-actor scheduling
config resolution); resolves the standing **Open question** in
`002-Scheduler.md` ("Priority: how `Priority<P>` (005) interacts with per-shard FIFO
queueing — multiple queues per shard vs. a priority structure"); binds `Priority<P>`
(`005-Developer-Model.md`), stays orthogonal to the deadline wheel
(`011-Timers-and-Scheduled-Work.md`), and is gated by the `023-Performance-Targets-and-Budgets.md`
local-tell (≤100 ns / ≤250 ns) and ≥10 M msg/s/core budgets.

---

## Question

Resolve, **as one configurable, compile-time policy surface** (CRTP, 005 — no
attributes, no reflection, no RTTI/virtual on the hot path), how `Priority<P>`
interacts with per-shard **activation** scheduling. Three shapes were on the table:

1. **Multiple FIFO queues per shard** — K priority bands, top-band-first select.
2. **Weighted-fair / deficit-round-robin (DRR)** — one activation queue per class,
   serviced in weight proportion.
3. **Deadline-aware / EDF** — unify `Priority<P>` with 011 per-message deadlines
   into one scheduling key.

The provable core the winner had to satisfy:
- **(a)** the **uniform-priority case is zero-cost** — it lowers to today's single
  per-shard FIFO activation queue; a design that taxes the common case LOSES;
- **(b)** per-actor **mailbox FIFO is never violated** — priority orders activations
  *across* actors, never messages *within* one mailbox;
- **(c)** **no added cross-core atomic RMW** on the enqueue/select hot path (the
  0-RMW drain gate);
- **(d)** **anti-starvation is GUARANTEED** (bounded, not best-effort) — state the
  mechanism and the bound;
- **(e)** high-priority activation dispatch p99 under mixed load **beats FIFO-only**.

Priority must not break single-executor, the exec-state wakeup, or the `seq_cst`
Dekker close-out (002).

---

## Designs (one-line summaries)

- **D1 — Compile-time K-band run-queue** (WINNER). CRTP scheduling policy replaces
  the shard's single activation run-queue with `std::array<ActivationMpsc, K>` (one
  FIFO band per priority level, band index resolved once at startup from `Priority<P>`).
  Default `UniformFIFO` (K=1) is a **distinct type** that objdumps identical to
  today's single per-shard MPSC. Enqueue picks the band-queue by a compile-time array
  subscript on the **same** single `tail_.exchange` (no added RMW); the draining
  worker does an O(K) **relaxed** non-empty probe + `countr_zero` (TZCNT) to pick the
  top band. Anti-starvation is a consumer-local (non-atomic) `RotatingReserve<M>`
  policy; `WeightedDRR<w…>` is offered as an alternative Anti policy.
- **D2 — BandedDRR (WeightedFair).** K band queues serviced by a consumer-private
  deficit-round-robin scheduler in weight proportion; anti-starvation "by
  construction" (DRR visits every band per round). Uniform default via
  `if constexpr(K==1)`. Deadlines left orthogonal.
- **D3 — EDF-Banded.** `Priority<P>` and 011 deadlines collapse into one scalar key;
  the ready structure is a consumer-private deadline-banded ring bucketed at the 011
  wheel granularity, find-first via `countr_zero`. Banding is consumer-side only.

---

## Evidence (claim → survived red-team? → proven by executed C++? → number)

GCC 14.2 + Clang 20.1, x86-64 Linux, -O2/-O3+LTO, pinned core, TSC-calibrated
rdtscp, percentiles not means. RMW counts by deterministic objdump lock/xchg
counting (perf HW counters were unavailable: `perf_event_paranoid=4`); safety by
TSan/ASan/UBSan with firing positive controls.

### D1 — Compile-time K-band run-queue (7/7 CORRECT, 0 disproven)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| **F1** uniform zero-cost | yes | **CORRECT** | `UniformFIFO` enqueue+select **byte-identical** to no-priority control, both compilers, -O2 & -O3. tell→dispatch p50 89.7 vs 85.0 ns; 13.5 vs 13.3 M msg/s/core (g++), 10.8 vs 10.9 (clang) — within noise, above 10M floor. +0 RMW. |
| **F2** O(K) select, 0 per-select RMW | yes (scoped) | **CORRECT** | 0 lock-prefixed ops in the K-way probe (both compilers). Cold O(K): K=1..64 = 6.2/5.8/11.5/14.7/28.4/51.5/120.5 ns. Hot (adversarial producers) inflects super-linearly beyond K≈8 → **cap K≤8**. |
| **F3** high-pri p99 beats FIFO | yes (scoped: independent load) | **CORRECT** | High-band dispatch p99 **9.3 µs vs 2.94 ms** (~316×), p999 27 µs, under 10%/90% saturating mixed load, `PriorityBands<2,RotatingReserve<8>>` vs `UniformFIFO`. |
| **S1** 0 added cross-core RMW | yes | **CORRECT** | enqueue = same single `tail_.exchange` (band = compile-time subscript), select probe = 0 RMW. Drain-owner CAS is a per-session cold edge, not per-enqueue/select. TSan-clean. |
| **S2** no lost activation/wakeup, single-executor | yes (revised) | **CORRECT** | 20M msgs, `PriorityBands<4>`, 3 workers/1 shard: lost=0, concurrency_violations=0, double_pop/push=0. TSan+ASan+UBSan clean. `QUARK_NO_FENCE` control leaks 32–37% vs 0.0000% → guard fires. |
| **C1** per-actor mailbox FIFO inviolable | yes | **CORRECT** | 2M msgs to a **middle-band** actor from 4 producers under cross-band saturation: 0 inversions; band constant per type (static_assert). Scrambled-stream control reports inversions → harness bites. |
| **C2** anti-starvation bounded, all bands | yes (revised) | **CORRECT** | Original `ReserveQuantum` **conceded** (0 services for middle bands in 1e8 turns, K≥3). **RotatingReserve<M>** bound `(d+1)·K·M` select turns measured **tight** (K=4,M=8: d=0→32, d=4→160), every band incl. middles serviced. `WeightedDRR` share matches `w_i/Σw`. |

**Fatal red-team finding (C2 middle-band starvation) was conceded and fixed with
`RotatingReserve`, and the fix was re-proven CORRECT with executed evidence.** The
run-queue/stealing race (plain `head_` vs 002 work-stealing) and the non-spinning
select strand were also conceded and fixed (per-shard drain-owner CAS; bounded-spin
on Busy) and are covered by the S1/S2 stress.

### D2 — BandedDRR (7/7 CORRECT, 0 disproven; weaker on the tie-breakers)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| **F1** uniform zero-cost | yes | **CORRECT** | enqueue+select **byte-identical** to control (both compilers, -O2/-O3); `sizeof(Activation)`=16 B identical via `[[no_unique_address]]`. 81.9 ns/op, 12.2 M/s (g++). |
| **F2** 0 added RMW | yes | **CORRECT** | locked-ops/fn: enq=1 (uniform)==1 (weighted), select msg-path=0, charge=0. O(K=8) empty scan p99 43.9–53.5 ns (< 250 ns). Shared-atomic control adds 1 lock → bites. |
| **F3** weight ratio + Hi p99 beats FIFO | yes (caveat) | **CORRECT** | ratio **8.000** (target 8:1); Hi dispatch-wait p99 **2878 vs 5118 msgs** (44% lower). Caveat: the "deficit-carry self-corrects over-service" mechanism is **false as written**; real protection is the `DrainBudget ≤ quantum` startup validator. |
| **S1** single-writer DRR state | yes (revised) | **CORRECT** | Per-shard service token; TSan-clean (712k drained). `MODE_NOTOKEN`/`OWNER_EXEMPT` controls race on `deficit_/rr_/head_` → bite. |
| **S2** close-out preserved | yes | **CORRECT** | lost=0/20M under WeightedFair; `-DNOFENCE` control leaks 4872/20M → non-vacuous. |
| **C1** per-actor FIFO | yes | **CORRECT** | 0 violations over 17–19M msgs, 32 actors across two bands. |
| **C2** anti-starvation by construction | yes (rescoped units) | **CORRECT** | max Lo wait = `quantum_Hi` exactly (512/4096/65536 at weights 8/64/1024), within `B_msg`. Honest caveat: message-count bound; suspend-heavy classes stretch the wall-clock tail to O(Σ quantum) scheduling cycles. |

### D3 — EDF-Banded (6 CORRECT, **1 DISPROVEN** → disqualified from winning)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| **F1/F2/F3** uniform zero-cost, 0 RMW, O(1) select | yes | **CORRECT** | objdump-identical uniform push; O(1) select flat p50 148→149→177 ns to n=1e5; cursor-jump p999 865 ≤ steady 887 ns. |
| **C1** per-actor FIFO | yes | **CORRECT** | 0 reorderings, adversarial interleaved deadlines. |
| **C3** independent high-pri p99 | yes (scoped) | **CORRECT** | 10402 vs 197810 ticks (~19×). |
| **S1** anti-starvation bounded | yes (revised) | **CORRECT** | RR-over-occupied-bands bound `B·K·D`=8192; measured victim waits 16 (mid) / 24 (default). |
| **S2** close-out re-anchored to mailbox | yes (conceded fatal bug fixed) | **CORRECT** | lost=0/5M; `-DQUARK_WEAK_FENCE` control 0.0503% → fires. |
| **C2** deadline-hit-rate **≥ FIFO always** | yes (rescoped) | **DISPROVEN** | Under overload EDF-banded is **consistently below FIFO**: R=1.00 49.3% < 50.0%; R=0.50 22.4% < 25.1% (EDF domino). Benefit is real only in the feasible **within-window** regime (N=2000: 99.2% vs FIFO 70.7%). "≥ FIFO always" is **false**. |

---

## Decision

**Winner: D1 — the compile-time K-band per-shard run-queue** (`UniformFIFO` default,
`PriorityBands<K, Anti>` opt-in, with `RotatingReserve<M>` as the default
anti-starvation policy and `WeightedDRR<w…>` as the configurable alternative).

Rationale, against the closing ranking rules:

1. **Safety gate.** D1 has **zero disproven safety/correctness claims**; all 7
   claims survived red-teaming and were proven CORRECT by executed C++. The one
   originally-fatal finding (C2 middle-band starvation with the naive
   `ReserveQuantum`) was a *mechanism* bug, not a structural one — it was conceded,
   replaced with `RotatingReserve<M>`, and the replacement's `(d+1)·K·M` bound was
   measured tight for **every** band including middles. No per-actor-FIFO violation
   and no unbounded-starvation result survives against D1. **D3 is disqualified**:
   its C2 (a correctness claim) is DISPROVEN — deadline-banding degrades *below*
   FIFO under overload, which is worse than the FIFO baseline it exists to beat, and
   the only "fix" is to narrow the claim to the within-window regime, not repair the
   design.

2. **Proven beats claimed.** D1: 7 proven / 0 disproven. D2: 7 proven / 0 disproven.
   D3: 6 proven / 1 disproven. D1 and D2 tie on this axis; D3 is out.

3. **Zero-cost + high-pri latency + starvation bound (D1 vs D2).**
   - *Zero-cost:* effectively tied — both are objdump-byte-identical to the
     no-priority control on both compilers, both -O2/-O3. (D1 carries one caveat:
     the disable path must select the `UniformFIFO` **type**, never
     `PriorityBands<1>`, which retains the turn-counter store — a startup/validation
     rule, below. D2's `if constexpr(K==1)` avoids that footgun; this is D2's single
     genuine edge and it is minor.)
   - *High-priority latency:* **D1 wins decisively.** Strict top-band-first select
     gives **~316× lower** high-band dispatch p99 (9.3 µs vs 2.94 ms), measured in
     time. D2's weighted-fair design deliberately keeps serving the low class, so it
     only cuts the hot tail **44%** (2878 vs 5118 msgs) — a smaller win, in
     message-units, by construction. Target (e) rewards beating FIFO on high-pri
     p99; D1 beats it far harder.
   - *Starvation bound:* both provable. D1's `(d+1)·K·M` is **tunable via M
     independent of how much priority separation you want**; D2's `B_msg = Σ(quantum_c
     + DrainBudget)` **couples the low-class wait to the weight ratio** — high Hi
     weight (e.g. 1024) buys a 65536-message low-class tail. D1's bound is the more
     controllable one.

4. **Configurability & no bent invariant.** D1 is the **most configurable** of the
   three and the one that best matches "resolve it as a configurable policy": the
   band structure is fixed but the anti-starvation behaviour is itself a policy knob
   — `RotatingReserve<M>` for strict-priority-with-reserved-fairness, or
   `WeightedDRR<w…>` for proportional fairness. **D1 subsumes D2** (WeightedDRR is
   offered as D1's Anti policy). D1 bends no core invariant: uniform zero-cost
   (proven byte-identical), per-actor FIFO (proven, band constant per type), 0-RMW
   hot path (proven), single-executor + exec-state wakeup + `seq_cst` Dekker
   close-out (proven untouched — they live on the mailbox, orthogonal to the
   run-queue).

D2 is a **safe, fully-proven runner-up** and is retained as the `WeightedDRR` Anti
policy inside the winning surface — no work is lost. D3's EDF unification is
**deferred**: its consumer-side banding is elegant and its independent-high-pri and
O(1)-select claims proved out, but as a general scheduling policy it can lose to
FIFO under overload, so it is not adopted as the default resolution; `band_of()` is
kept as the single extension point for a future opt-in `EdfBanded` policy that maps
deadlines into bands **without** touching per-actor FIFO (C1).

---

## Resolution — the configurable scheduling-policy surface

```cpp
// Engine-level compile-time scheduling policy (005 CRTP; deducing-this, no virtual).
struct UniformFIFO;                                  // DEFAULT — K==1, == today
template <std::size_t K, class Anti = RotatingReserve<8>>
struct PriorityBands;                                // opt-in, 1 <= K <= 8

// Anti-starvation is itself a policy knob (consumer-local, non-atomic):
template <std::size_t M> struct RotatingReserve;     // default; bound (d+1)*K*M turns
template <std::uint16_t... W> struct WeightedDRR;     // proportional; share = w_i/Sum(w)
```

- `Priority<P>` (005) names a compile-time priority **class**; startup resolves it
  to a `Band` index (0 = highest). More distinct `Priority<P>` values than `K` bands
  is a Validation error (005, `std::expected<Metadata, ValidationError>`).
- **Disable = `UniformFIFO` (the type), never `PriorityBands<1>`** — enforced as a
  Validation rule; `PriorityBands<1>` retains the anti-starvation turn-counter store
  and is *not* byte-identical to control.
- **K is capped at 8** — beyond that the consumer's per-select K-way relaxed probe
  hits cross-core coherence-miss inflation that can breach the 100 ns local-tell
  budget under adversarial producer pressure.

---

## Spec recommendations

### `002-Scheduler.md`
- Replace the **Open question** ("Priority … multiple queues vs. a priority
  structure") with a **Resolved** subsection pointing to this ADR: the shard's
  single activation run-queue generalizes to `std::array<ActivationMpsc, K>` under
  the `PriorityBands<K, Anti>` scheduling policy; `UniformFIFO` (K=1, the default)
  lowers byte-for-byte to today's single per-shard MPSC (proven identical, both
  compilers).
- Under **Fairness**, add anti-starvation: `RotatingReserve<M>` services the next
  non-empty band under a round-robin cursor every M-th select turn (bound
  `(d+1)·K·M` select turns, proven tight); `WeightedDRR<w…>` gives per-band share
  `w_i/Σw`. State the mechanism and bound as **guaranteed, not best-effort**.
- Under **Work stealing**, make explicit that the K-band run-queue is
  **single-consumer** and a stealer must win a per-shard **drain-owner CAS**
  (`std::atomic<Worker*>`, null→self on entering a drain session, per session — a
  cold edge, not per-enqueue/select) before popping. This is the arbitration the
  single-consumer Vyukov queue already requires; banding adds queues, not servicers.
- Add a normative note: banded `select` **bounded-spins on Busy** (exactly like the
  mailbox drain) — a single-pass non-spinning probe can return a spurious `nullptr`
  and strand a `Scheduled` activation after its lone targeted wakeup.
- State that the **exec-state wakeup and the `seq_cst` Dekker close-out are
  unchanged** — priority lives on the per-shard run-queue, orthogonal to the
  per-actor mailbox where the close-out rendezvous lives. The consumer non-empty
  probe compares the atomic `tail_` to the **constant `&stub_`**, never the moving
  `head_`.

### `005-Developer-Model.md`
- In the **Scheduling** policy table, expand `Priority<P>` to note it names a
  compile-time priority **class** consumed by the engine-level `PriorityBands<K,
  Anti>` scheduling policy (default `UniformFIFO`); priority orders activations
  **across** actors and **never** reorders an actor's own mailbox.
- Under **Validation (fail-fast)**, add scheduling checks: (i) distinct
  `Priority<P>` classes ≤ K; (ii) "disable priority" resolves to the `UniformFIFO`
  type, never `PriorityBands<1>`; (iii) for `WeightedDRR`, per-actor `DrainBudget<N>`
  ≤ class quantum (the deficit-carry self-correction does not hold without it).
- Note that band is **constant per actor type** (startup-resolved metadata, no
  per-message recompute) — this is what makes C1 (per-actor FIFO) hold by
  construction.

### `011-Timers-and-Scheduled-Work.md`
- Add a short **Priority interaction** note: the deadline wheel stays **orthogonal**
  to priority banding — deadlines fire via the wheel and `tell` onto the mailbox
  (single-executor preserved); priority orders **activations**, not messages, so it
  never reorders a deadline-carrying message within its actor's mailbox.
- Record that a **deadline-unified `EdfBanded` policy** (folding 011 deadlines into
  a band key) was **evaluated and deferred**: it can degrade below FIFO under
  overload (ADR-010, D3 C2 disproven). `band_of()` is the extension point if a
  future opt-in EDF policy is pursued, and it must preserve per-actor FIFO.

### `023-Performance-Targets-and-Budgets.md`
- Add priority scheduling to the **budget/regression suite**: (i) `UniformFIFO`
  enqueue+select must remain **objdump-byte-identical** to the no-priority control on
  both toolchains (a hard gate, the zero-cost-when-uniform contract); (ii) banded
  enqueue and per-select add **0 cross-core RMW** (0-RMW drain gate); (iii) high-band
  dispatch p99 under mixed saturating load must beat `UniformFIFO` p99; (iv) the
  anti-starvation bound `(d+1)·K·M` select turns is a reported, enforced number;
  (v) **cap K≤8** and track the hot-producer K-way-probe select p99 against the
  100 ns local-tell budget. Report percentiles, not means.

---

## Residual risks

- **Ask-chain priority inversion (unmitigated, documented).** Static per-type
  priority has no inheritance/donation: a high-band actor that `ask()`s a low-band
  actor inherits the low band's dispatch latency. All three designs share this; it is
  a stated limitation, not a claim. `band_of()` is the future extension point
  (inheritance / deadline propagation) and must not be adopted without preserving C1.
- **`UniformFIFO`-only zero-cost (footgun).** The guarantee holds only for the
  distinct `UniformFIFO` type; `PriorityBands<1>` retains the turn-counter store
  (23/17 vs 8 select insns). Mitigated by the Validation rule, but a hand-rolled
  `PriorityBands<1>` silently pays.
- **Hot-select coherence-miss inflection.** Under adversarial cross-core producer
  saturation the K-way relaxed probe's isolated select p99 exceeds 100 ns for K≥2;
  the end-to-end 100 ns headroom depends on non-adversarial pressure. Mitigated by
  K≤8 and by the fact that the probe adds **0 RMW** (gate c honestly holds).
- **Reserved-turn tail spike.** `RotatingReserve<M>` injects a periodic high-band
  p999 spike (~one low-band drain budget, frequency ~1/M) — the honest fairness tax
  (high-band p999 measured 27 µs). Tunable via M; report p999, not just p99.
- **K is a build-time constant.** Runtime-dynamic priority levels require recompiling
  K; fine-grained priorities quantize into K coarse bands.
- **Perf HW RMW counters unavailable** (`perf_event_paranoid=4`) in the proof
  environment — RMW counts were established by deterministic objdump lock/xchg
  counting and TSan, not `perf c2c` HITM. Re-confirm with hardware counters on a CI
  box with `CAP_PERFMON` before promoting the 023 gate.
- **Suspend-heavy `WeightedDRR`.** If `WeightedDRR` is chosen, the message-count
  starvation bound stretches to O(Σ quantum) scheduling cycles in wall-clock for
  handlers that suspend after ~1 message (015). `RotatingReserve` (turn-based) is not
  subject to this and is the safer default.
