# ADR-035: Worker Park/Wake Backoff Policy

## Status

Accepted (Design 1 adopted as the default; a synthesized hybrid is recommended as the
target for a follow-up round, not yet designed, red-teamed, or measured).

## Question

`Engine::worker_loop` (`include/quark/core/engine.hpp`) parks a worker the instant
`scan_and_run()` returns `false` once, with no spin/backoff. Real instrumented
measurement (the engine's own `metrics_snapshot().wakeups` counter, 1-worker/1-shard,
empty-handler PingActor, matching `bench/caf_comparison/quark_mpsc_bench.cpp`'s shape)
showed 31.0%/15.8%/4.4% of sends at 1/2/4 producers pay a real OS wake syscall
(futex/`WaitOnAddress`) synchronously inside the producer's own `tell()` call — a
plausible contributor to the measured gap vs CAF in `bench/caf_comparison/README.md`
(Tell p999 41,800ns vs CAF's 600ns; single-producer throughput 3.42 vs 5.14 M/s).

Two designs were proposed, red-teamed, revised where required, and proven with real
compiled+executed C++23 (clang++ 22.1.5 on Windows x86-64 — no g++ available in this
environment; TSan unavailable on this Windows/clang target, ASan+UBSan + manual
concurrent stress used as the disclosed fallback by both provers).

## Design summaries

1. **Design 1 — bounded read-only pre-park spin, upstream of `park()`.** Fixed spin: on
   `scan_and_run()==false`, loop up to `cfg_.pre_park_spin_limit` (default 256) times —
   `cpu_relax()` then check `any_work()||stopping_`, returning `true` (re-scan) on a hit,
   else falling through to completely unmodified `park()`. Gated under
   `#if !defined(QUARK_SCHED_BROKEN_WAKEUP)` to preserve the
   `sched_no_lost_wakeup_control` negative-control test's teeth. Never touches
   `idle_mask_` during the spin. No adaptation to load — every idle transition pays the
   same small, fixed cost regardless of recent burstiness.

2. **Design 2 — EWMA-gated exponential spin-then-park backoff.** Adaptive: each
   `Worker` gets a cache-line-isolated `Backoff` struct (`spin_quota_relax`,
   `idle_streak`, `burst_ewma_q16` Q16.16 EWMA, alpha=1/16, `last_hit_ns`), touched only
   by the owning thread. On `scan_and_run()==false`: if EWMA<0.5 ("not bursty"), zero
   spin, park immediately (matches today's behavior for non-bursty workers). If
   EWMA>=0.5, spins in doubling rounds (32->4096 relax-count cap) calling real
   `scan_and_run()` between rounds, until hit/`stopping_`/a 6000ns wall-clock deadline,
   then falls through to unmodified `park()`. `kHardIdleStreak=8` force-resets after 8
   consecutive fully-idle park cycles. Two required fixes came out of red-team before
   the design could be trusted:
   - **Fix #1 (fatal/serious):** no `QUARK_SCHED_BROKEN_WAKEUP` gate at all in the
     original draft — this repo has direct precedent for exactly this failure mode
     (`engine.hpp`'s own comment on an earlier ADR-028 backstop-thread version that
     masked `sched_no_lost_wakeup_control` with extra rescans outside the exact Dekker
     under test). Added and verified.
   - **Fix #2 (serious, possibly fatal to the whole mechanism):** `note_hit` was only
     wired into the internal spin loop, so under sustained load — where the *outer*
     `scan_and_run` succeeds on the first try every time, the common case — the EWMA
     might never engage, making the entire mechanism a silent no-op. Confirmed real;
     fixed by wiring `note_hit` into the outer `worker_loop` success path too, verified
     directly via `ewma_engage_test.cpp` (crosses 0.5 at round 12, saturates to 0.9998).

## Evidence table

Only claims that survived red-teaming (after required fixes) and were then run as
real, compiled, sanitized/instrumented C++23 are counted toward the decision.
fast-2/FAST-2 came back WRONG for **both** designs and is treated as a wash — both
provers independently flagged this host as noisier than the original 41,800-51,000ns
reference figure, so this is recorded as genuinely unproven for both, not a
distinguishing result.

| Design | Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|---|
| D1 | fast-1: wakeups/sends <5%/<2%/<2% @ N=1/2/4 | fast | yes | **CORRECT**, huge margin | 0.26%/0.13%/0.019% (from ~26/15/6% baseline) |
| D1 | fast-2: single-producer Tell p999 <20,000ns | fast | yes | **WRONG** | baseline avg p999 ~51,117ns, spin avg ~47,967ns (~6% lower, noisy, not decisive) |
| D1 | fast-3: single-producer throughput >=10%, no regression @ N=2/4 | fast | yes | **PARTIAL** — WRONG on N=1 magnitude, CORRECT on no-regression | N=1 +7.9% (short of 10% bar, directionally consistent, spin>=baseline 7/8 paired trials); N=2 -0.1%, N=4 -1.3% (noise, no regression) |
| D1 | fast-4: zero heap alloc | fast | yes | **CORRECT** | — |
| D1 | safe-1: race-free | safe | yes | TSan **INCONCLUSIVE** (unavailable, not a negative result); **CORRECT** under ASan+UBSan+manual stress | 11 runs, spin limits 0/1/4/256 explicitly straddling the spin/park boundary |
| D1 | safe-2: single-executor invariant | safe | yes | **CORRECT** | — |
| D1 | safe-3: `QUARK_SCHED_BROKEN_WAKEUP` gate integrity (fatal-risk-class) | safe | yes | **CORRECT**, both halves | spin_limit in {0,1,256,100000}: control strands every run; normal build drains 200,000/200,000 at every limit |
| D1 | correct-1/2: FIFO, no lost/duplicated messages | correct | yes | **CORRECT** | tested at spin limits 0/1/4 to maximize boundary-straddling |
| D1 | correct-4: genuinely idle worker fully parks | correct | yes | **CORRECT** | 0.779% CPU over a 4s zero-traffic window |
| D1 | "byte-identical at limit=0" | fast | n/a (red-team caught overclaim) | **WRONG as originally stated**, corrected honestly | disassembly: 2 extra instructions / 9 extra bytes, zero extra function calls (fully inlined), no behavior change |
| D1 | cache-line-contention attack (isolated probe: 2.3x-9x `tail_.exchange` slowdown under 1-4 spinners) | safe/fast | yes | re-tested on the **real** implementation via fast-3 | real-world impact at N=2/4 was noise-sized (-0.1%/-1.3%) — much smaller than the isolated probe's worst case, though the underlying mechanism (contended read on `tail_`) is real and structurally present |
| D2 | FAST-1: wakeups/sends <5% @ every N | fast | yes | **CORRECT** | 0.14-0.15%/0.04-0.05%/0.01% @ N=1/2/4 (baseline 10.2-31.6%/12.2-12.5%/1.1-3.3%) |
| D2 | FAST-2: Tell p999 improvement | fast | yes | **WRONG** | same-session paired: baseline ~49,060ns, patched ~49,860ns, statistically indistinguishable, sometimes worse |
| D2 | FAST-3: single-producer throughput improvement | fast | yes | **CORRECT**, strong | same-session interleaved: patched beat baseline 9/9 pairs, +15-20% (baseline ~2.45M/s, patched ~2.87-2.92M/s) |
| D2 | FAST-4/5: near-zero idle cost, zero relax on cold start | fast | yes | **CORRECT** | — |
| D2 | SAFE-1: gate integrity (post-fix #1, fatal-class risk) | safe | yes (after required fix) | **CORRECT** | control strands 3/3; normal build drains 200,000/200,000, never stalls, 3/3 |
| D2 | SAFE-2: race-free | safe | yes | TSan **INCONCLUSIVE** (unavailable); **CORRECT** under ASan+UBSan+stress | 15/15 concurrent-stress runs clean, 30/30 total incl. plain runs, exact message-count conservation; PLUS all 5 pre-existing scheduler regression tests re-run unmodified against the patched engine, 5/5 pass |
| D2 | SAFE-3: zero heap alloc | safe | yes | **CORRECT** | — |
| D2 | SAFE-4: `stop()`-under-load latency bounded to one round/<=4096 relax instructions | safe | yes | structurally **CORRECT**, wall-clock consequence flagged as a major risk | early-signal: median 300ns/p99 800ns/worst 4200ns; late-signal: median 2600ns/p99 5000ns/worst 19,100ns; true worst case (signal arriving as a 4096-relax round begins) measured up to **~380,800ns (~380us)** on this host |
| D2 | CORRECT-1/2: FIFO, single-executor, backoff force-engaged via sustained traffic | correct | yes | **CORRECT** | 300,000 sequenced messages, exact FIFO + single-exec, clean under ASan+UBSan |
| D2 | CORRECT-3: bounded disengagement | correct | yes | **CORRECT**, exact | from saturated engagement, disengages at exactly 8 park-decision cycles every run (kHardIdleStreak=8 is the binding bound) |
| D2 | Fix #2 (note_hit outer-path wiring gap) | fast (mechanism validity) | n/a (new gap, found by red-team) | confirmed real, then **fixed and verified** | `ewma_engage_test.cpp`: EWMA crosses 0.5 at round 12, saturates to 0.9998 — without the fix the mechanism would be a silent no-op under the common sustained-load case |

## Decision

**Winner (adopted as the default, as-tested): Design 1 — bounded read-only pre-park
spin.** It clears every safety/correctness claim as **CORRECT** under the best
available tooling on this host (safe-1/2/3, correct-1/2/4 all CORRECT; safe-3's
`QUARK_SCHED_BROKEN_WAKEUP` gate-integrity control — the fatal-risk-class check — held
at four different spin limits including the boundary values 0 and 1), meets its
primary goal (fast-1) with a huge margin (0.26%/0.13%/0.019% wakeup-syscall ratio
against a <5%/<2%/<2% bar), allocates zero heap on the hot path (fast-4, CORRECT), and
— crucially — its worst case is small, fixed, and easy to reason about: 256
non-doubling `cpu_relax()` iterations, no round structure, well under the figures
either design's own p999 measurements ever produced. Its one real shortfall is
fast-3's magnitude at N=1 (+7.9%, short of the 10% target) — a genuine, disclosed miss,
but directionally consistent, with **no regression** at N=2/4, and not a safety
violation.

**Design 2 is not selected, despite a materially stronger FAST-3 result
(+15-20% vs Design 1's +7.9%), because of SAFE-4.** This is a deliberate judgment call,
and it needs to be stated precisely so it is not mistaken for a mechanical
rule-1 gate: SAFE-4 was proven **CORRECT**, not WRONG — the structural bound
("one round / <=4096 relax instructions before a deadline check") genuinely holds.
Design 2 therefore does not trip rule 1's literal disqualification (no `safe`/`correct`
claim was marked WRONG). What it does trip is rule 4's spirit and this project's own,
already-codified machine-safety ethos: the *wall-clock consequence* of that structurally
correct bound is a measured worst case of **~380us for a single spin round** — roughly
**9x worse** than the very OS-wake latency this whole effort was meant to shrink
(Tell p999 was 41,800-51,000ns at baseline) and roughly **25x worse** than Design 1's
comparable worst case. `002-Scheduler.md` already states, for the structurally
analogous `Busy`-spin case, "**bounded spin**, then if still `Busy`, ... (never spin
unbounded)" — Design 2's exponential-doubling-to-4096 shape, checked only *between*
rounds rather than within one, is a much looser bound than anything else on this
hot path, and it lands on exactly the scheduling primitive CLAUDE.md's machine-safety
section singles out as the place where "hang or power off" risk lives. Per the debate
brief's explicit instruction to weigh this under the safety-is-a-gate ranking rule and
this repo's worst-case-first culture (see e.g. ADR-004: "the fence is load-bearing
regardless of the small magnitude"; ADR-033: designs disqualified for tail-latency
regressions even where a real strength coexisted), Design 2 as specified is not adopted
as the default — but it is **not disqualified from ever shipping**: unlike SEG-HP in
ADR-033 (a genuine correctness bug with no stated cheap fix), SAFE-4's flaw has an
obvious, cheap fix (check the deadline *within* a round, or cap the round shape to
something closer to Design 1's small fixed count) that the debate brief itself
anticipates.

**Recommended target for the next round: a synthesized hybrid, not yet built or
proven.** Combine Design 2's EWMA gating (near-zero cost for genuinely-idle workers —
a property Design 1 structurally lacks, since Design 1 always pays its bounded spin
cost on every idle transition regardless of recent burstiness) with Design 1's much
tighter, non-exponential, small-fixed-count spin shape once engaged (avoiding Design
2's exponential-doubling-to-4096 worst case entirely). On the evidence gathered this
round, such a hybrid plausibly dominates both proven designs: it would inherit Design
2's demonstrated ability to detect and exploit burstiness (FAST-3's +15-20%, CORRECT,
9/9 paired trials) while bounding its worst case to something in Design 1's proven
small-and-safe regime. **This is a recommendation, not a verdict** — per rule 2
("proven beats claimed"), it carries zero weight as a design-debate winner because it
has not been built, red-teamed, or measured. It is recorded here as the explicit
scoping target for round 2 of this specific debate.

No core invariant (single-executor, stable placement, zero-cost hot path, std-only
core, PAL isolation) is bent by adopting Design 1: safe-2/SAFE-2 (single-executor)
came back CORRECT for both candidates, and Design 1's change is additive and gated
under a preprocessor flag that leaves the pre-existing `QUARK_SCHED_BROKEN_WAKEUP`
negative control's teeth intact.

## Residual risks

1. **TSan was unavailable in this proving environment for both designs** (Windows/clang
   target) — every `safe`/SAFE race claim for both Design 1 and Design 2 rests on
   ASan+UBSan+manual concurrent stress, honestly disclosed by both provers as not
   equivalent to true race detection. Neither design's race-freedom claim should be
   treated as final until re-run under TSan on the Linux CI matrix, which does have it.
2. **All numeric evidence in this ADR comes from one host** — an unpinned-beyond-
   process-affinity Windows box that both provers independently flagged as noisier
   than the original 41,800-51,000ns Tell-p999 reference figure. Per this repo's own
   `PERFORMANCE.md` conventions, none of these numbers (fast-1/FAST-1 ratios, fast-3/
   FAST-3 throughput deltas, the ~380us SAFE-4 tail) should be treated as canonical
   until re-measured on the Linux CI matrix with pinned cores (`taskset -c 0-3`,
   single-thread microbenches on one core) per CLAUDE.md's machine-safety rules.
3. **fast-2/FAST-2 (Tell p999 improvement) is unproven for both designs** — treated
   here as a wash, but it remains a genuinely open question whether *either* consumer-
   side wait-strategy change has any real lever on producer-side p999 in the
   low-contention single-producer case, or whether the original 41,800ns reference
   figure needs a different mechanism entirely (e.g., the p999 gap may live elsewhere
   in the send path, not in park/wake).
4. **Design 1's fast-3 magnitude miss at N=1 (+7.9% vs. a 10% target) is real** and
   should be re-measured on Linux/pinned hardware before the default `spin_limit=256`
   is treated as tuned; it is plausible a larger or smaller fixed spin count performs
   differently on a quieter host.
5. **Design 2's SAFE-4 finding (~380us single-round worst case) is disclosed but not
   mitigated.** If a future round wants to adopt Design 2's adaptive mechanism wholesale
   (rather than the hybrid), SAFE-4 must be fixed first — e.g., check the deadline
   within a round rather than only between rounds, or cap the round shape well below
   4096 relax instructions — and re-proven with the same rigor as this round's SAFE-4
   measurement.
6. **The hybrid is entirely unproven.** The next round must design it fully, run it
   through red-team (in particular: an EWMA-state-isolation/cache-line check, and a
   `QUARK_SCHED_BROKEN_WAKEUP`-gate-integrity test at the same rigor as safe-3/SAFE-1),
   and measure fast-1/fast-2/fast-3 plus a SAFE-4-equivalent worst-case-tail probe
   before it can be adopted as anything more than a scoping target.
7. **The cache-line-contention mechanism Design 1's red-team isolated (2.3x-9x
   `tail_.exchange` slowdown under 1-4 concurrent spinning readers) is real and
   structurally present**, even though its real-world impact was measured as noise-sized
   (-0.1%/-1.3%) at N=2/4 in this round. Only N<=4 producers were tested; this gap
   between the isolated-probe worst case and the real-implementation measurement should
   be re-checked at higher producer counts on the Linux CI matrix, since the mechanism
   itself was not disproven, only found to matter less than the isolated probe
   suggested at this scale.
8. **`QUARK_SCHED_BROKEN_WAKEUP` gate-integrity coverage is uneven across the two
   designs** — Design 1 was checked at four spin-limit values (0, 1, 256, 100000);
   Design 2 was checked 3/3 runs at its shipped configuration only. Any future round
   that pursues Design 2 or the hybrid toward production should broaden this coverage
   to match Design 1's boundary-value testing discipline before sign-off.

## Forward references

- **[ADR-038](ADR-038-scheduler-oversubscription-tail-latency.md)** evaluated (and rejected) extending
  `pre_park_spin()`'s escalation with an oversubscription-gated `std::this_thread::yield()` tail —
  its F4 claim measured the yield-tail's own wall-clock cost (p50/p99/max) as *worse* than the plain
  256-iteration `cpu_relax()` baseline it would replace, 5/5 runs across both g++ and clang++. This is
  a concrete, measured instance of the same SAFE-4-class hazard this ADR identified in rejecting
  Design 2 (a materially better average case purchased with a worse realistic-workload worst case) —
  the same discipline applies again, on a different axis (oversubscription rather than burst
  detection). ADR-038's own winning fix for the P > core-count tail-latency regime is a different
  mechanism entirely (bounded cooperative `drain_owner` eviction, not a park/wake change) — this
  ADR's Design 1 choice is reconfirmed, not revisited.

## Spec-update recommendations

**`002-Scheduler.md`**
- Add a new subsection under `## Wakeup`, e.g. `### Idle backoff before park
  (ADR-035)`, documenting the adopted mechanism: on `scan_and_run()==false`, a worker
  spins up to `cfg_.pre_park_spin_limit` (default 256) times — `cpu_relax()` then
  re-check `any_work()||stopping_` — before falling through to the unmodified `park()`
  path, gated under `#if !defined(QUARK_SCHED_BROKEN_WAKEUP)` to preserve the
  `sched_no_lost_wakeup_control` negative control. Record the measured wakeup/send
  ratio collapse (from ~26/15/6% baseline to 0.26%/0.13%/0.019% at N=1/2/4), zero heap
  allocation, and that a genuinely idle worker still fully parks (0.779% CPU over a 4s
  zero-traffic window) — this is additive-only and does not change the `Wakeup`
  section's existing normative rule ("wakeup and deschedule ride the exec-state
  machine, never mailbox emptiness").
- Record the disclosed shortfall honestly: single-producer throughput improvement
  measured +7.9%, short of a 10% target, with no regression at N=2/4 — an open item
  for a future round, not a blocking defect.
- Add a "rejected/deferred" note for the EWMA-gated exponential-backoff alternative
  (round-1 of this ADR): its adaptive gating achieved a stronger throughput result
  (+15-20%, CORRECT) but its exponential-doubling-to-4096 spin shape produced a
  measured ~380us single-round worst case — inconsistent with this section's own
  "never spin unbounded" principle for the structurally analogous `Busy`-handling case
  — and was not adopted as specified. Record the recommended next step: an EWMA-gated
  hybrid using a small, fixed (non-exponential) spin shape once engaged, still unbuilt
  and unproven, is the scoping target for a follow-up round.
- Cross-reference ADR-035 next to the existing "Mailbox hot-path baseline" section's
  round-by-round pattern (ADR-029/031/032/033), since this is the first debate round on
  the park/wake side of the scheduler rather than the mailbox itself.

**`001-Actor-Execution-Model.md`**
- Add a short "Reconfirmed (ADR-035)" line to the Mailbox section's existing
  invariant-reconfirmation list (alongside the ADR-029/031/032 entries): the worker
  park/wake backoff round is a scheduler-only, `Worker`-loop-local change — it touches
  neither the exec-state CAS, its memory orders, nor the Dekker close-out rendezvous
  (`002` §close-out). safe-2/SAFE-2 (single-executor invariant) and correct-1/2 (FIFO,
  no lost/duplicated messages) came back CORRECT for both candidate designs in this
  round, so the invariant is unaffected regardless of which backoff shape is later
  chosen.
- No other change is warranted: `001` documents actor-facing execution semantics
  (exec-state lifecycle, hybrid handler execution, cancellation, reentrancy), none of
  which this ADR's designs touch — the park/wake mechanism is entirely below that
  layer, inside `Worker::worker_loop`, and the cross-reference above is sufficient to
  keep `001` accurate without duplicating `002`'s content.

## Tie-breaking / round-2 scoping experiment

Not needed to pick this round's winner among the two as-tested designs — Design 1
clears the safety/correctness bar cleanly and Design 2's one disqualifying-in-spirit
risk (SAFE-4) is well-documented and decisive on its own. For the recommended
follow-up round building the hybrid:

- Implement EWMA gating exactly as Design 2's (post-fix) mechanism, but replace the
  32->4096 doubling-round spin shape with Design 1's small, fixed, non-doubling
  `cpu_relax()` count (or a similarly small fixed bound) once the EWMA has gated a
  worker into "bursty."
- Required proofs before this can win a future round: fast-1/FAST-1-equivalent
  (wakeup/send ratio) at the same <5%/<2%/<2% bar; fast-3/FAST-3-equivalent
  (single-producer throughput) — the real target to beat is Design 2's +15-20%, not
  just Design 1's partial +7.9%; a SAFE-4-equivalent worst-case-tail probe that must
  land materially below Design 2's ~380us figure, ideally within an order of magnitude
  of Design 1's proven small worst case; the same `QUARK_SCHED_BROKEN_WAKEUP`
  gate-integrity check at Design 1's boundary-value rigor (spin_limit-equivalent 0, 1,
  and the shipped default, at minimum); and CORRECT-3-equivalent bounded disengagement.
- All of the above should be measured on the Linux CI matrix with pinned cores, not
  this round's single noisy Windows host, before being treated as decisive.
