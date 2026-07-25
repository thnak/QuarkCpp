# ADR-023: Timer-Wheel Tick/Tier Scheme and Active-Advance / Timekeeper Drift Bound

## Status

Accepted

## Question

011's per-shard hierarchical timing wheel left two open, interacting questions
(011 §Open questions #1 and #3):

1. What base tick size and tier structure should the wheel use, given that
   message deadlines can require microsecond-level precision while many
   timers are far-future (needing coarse, cheap buckets) — a single fixed
   tick, a fine-base + coarse-overflow tier scheme, or a
   configurable/deployment-tunable tick?
2. How much drift can accumulate between a busy shard's own active-advance
   (worker-driven, between drains) and the low-frequency node-level
   timekeeper's targeted wakeup of idle shards, and what mechanism
   bounds/reconciles that drift?

Three designs were drafted, cross-examined (red team vs. defense), and then
proven or disproven with compiled, executed C++ (g++ 14.2.0 and clang++
20.1.2, plain -O2 plus ASan/UBSan/TSan builds, taskset-pinned to ≤4 cores per
the machine-core-limit rule).

## Designs (one-line summaries)

- **D1 — FixedTick256/3 + Occupancy-Skip Catch-Up**: fixed, compile-time 1ms
  base tick, 256 buckets × 3 levels (~4h39m span) + unchanged heap overflow;
  `drain_owner` CAS doubles as the wheel's single-writer gate; occupancy
  bitmap lets `advance_to()` bulk-skip empty ticks.
- **D2 — Fine-Base Cascading Wheel + Adaptive-Poll Timekeeper** (**winner**):
  5 fixed tiers × 64 slots (6-bit) on a 64µs base tick (~19.09h native span)
  + unchanged heap overflow; busy-shard active-advance steps directly to
  each tier-0 wrap boundary via an `occupied_mask` bit-window scan (never
  per-tick); timekeeper touches only one per-shard relaxed atomic
  (`next_due_hint_`) and reprograms its own sleep to the global next-due
  minimum instead of polling on a fixed cadence.
- **D3 — PowerOfTwo-Tick Wheel + SWMR Last-Advanced-Quantum Ledger**: tick
  size is a validated, power-of-two-nanosecond BuildOnly knob (shift, never
  divide); unchanged cascading topology; a per-shard SWMR
  `last_advanced_quantum`/`catchup_requested` ledger lets a fixed-period
  timekeeper compute an exact tick deficit and piggyback its wake on the
  engine's existing producer-wake Dekker.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| F1 catch-up cost ~independent of gap | D1 | yes (revised: bitmap-scan `recompute_next_due_hint` + cyclic-bit-scan `next_mandatory_tick`) | **CORRECT** | empty-wheel 3.6M-tick jump: 20,850×–43,950× faster than naive tick-loop; N=64,000 distinct-tick entries: 6.7–16.0ms (was 5.9s / O(n²) pre-fix) |
| F2 hot-loop overhead <50ns / burst ≤5× steady | D1 | partially (steady sub-claim only) | **WRONG** (conjunctive claim fails) | steady Δp50 = 28–44ns (OK); burst-after-gap Δ = **812×–2114×** vs. the claimed ≤5× — real O(entries-due) work, wrong comparator baseline |
| S1 no TSan race on wheel-private state | D1 | yes (revised: producer-side MPSC intake stack instead of direct insert) | **CORRECT** | 0 TSan reports, 12.8–13.5M ops/25s, both compilers |
| S2 targeted-wake, never broadcast | D1 | yes (narrowed: no liveness promise) | **CORRECT** | exactly 3 `try_wake_worker` calls / 0 broadcasts for 3 distinct due shards |
| C1 tombstone-flip bound (busy/idle) | D1 | conceded as non-exhaustive, revised to a **conditioned** claim | **CORRECT, but contingent on an unmet external precondition** | unpatched: sibling shard starved 497–591ms behind a 10M-msg hot shard (reproduced on real, unmodified `engine.hpp`); only holds once 002/ADR-010 gets a shard-level fairness budget it does not have today |
| C2 no sub-ms precision by construction | D1 | yes | **CORRECT** | 200µs deadline never tombstones before tick 1 (~1ms) |
| F1-rev advance_to() cost flat vs. gap size | D2 | yes (fixes the fatal tier-0 slot-skip drop bug) | **CORRECT** | sparse occ: 100µs gap = 239.5ns/call, 10ms gap = 297.4ns/call (**1.24× for a 100× larger gap**, vs. 78× a linear-in-elapsed-ticks model predicts) |
| F2-rev cascade() invocation count | D2 | yes (concedes recursion-depth-vs-count conflation) | **CORRECT** | 50ms/64µs gap → 12 cascade(1) calls, matches ceil(elapsed/64) exactly for every swept gap; max recursion depth ≤3, never exceeds kTierCount=5 |
| S1 zero heap alloc/lock in wheel span | D2 | yes (adds explicit non-blocking pool-exhaustion policy) | **CORRECT** | 1M ops, 0 allocations; exhaustion test: 64/200 tracked + saturating drop-counter, 0 fallback allocs |
| S2 timekeeper hint-only, no wheel-array race | D2 | yes | **CORRECT** | 0 TSan reports, 10.02M combined ops, both compilers |
| S3 cancel() O(1) regardless of tier/overflow | D2 | yes | **CORRECT** | tier0=1.82ns, tier4=2.25ns (1.24×), overflow=2.47ns (1.36×) — both under the 2× bar |
| C1-rev deadline-fire XOR / no spurious stop | D2 | yes (fixes the fatal missing-generation-check CAS-failure bug) | **CORRECT** | 1M-descriptor stress, 0 XOR violations after adding the generation-first masked-CAS retry |
| C2 idle lateness ≈ f(poll cadence only) | D2 | yes | **CORRECT** | p99 lateness within interval+200µs at 1/5/50ms cadences; kBaseTick sweep {16,64,256}µs shows no systematic shift |
| C3 no schedule_every() drift | D2 | yes (contingent on the F1-rev fix) | **CORRECT** | 1,000,000 periods, fitted slope deviation = 0, 0 dropped periods |
| C4 overflow entries fire exactly once | D2 | yes (contingent on the F1-rev fix) | **CORRECT** | 10,000 far-future entries, exactly 10,000 dispatches, 0 dropped/duplicated |
| F1 zero-timer path cost-invisible | D3 | yes | **CORRECT** | single-thread isolation: Δ = −3.54%/−0.31% (negative, i.e. unmeasurable) |
| F2 already-caught-up cost <20ns | D3 | yes (root-cause identified) | **WRONG** | Δp50 = 37ns both compilers — driven almost entirely by the intrinsic ~36ns cost of one extra `steady_clock::now()` read, baked into every drain-owner acquisition of a timer-active shard, not a corner case |
| F3 idle catch-up cost ∝ 1/tick_ns, ⊥ tiers | D3 | yes | **CORRECT** | tick_ns sweep matches expected count within 2×; (bits,levels) sweep flat at 15 for all 9 combos |
| F4 busy-path cost decoupled from tick_ns | D3 | conceded original constant, revised to trend-only | **CORRECT** (revised) | measured/predicted ratio 0.987–1.000 across 12 combos |
| S1 SWMR ledger race-free | D3 | yes | **CORRECT** | 0 TSan reports, 60.9–73.9M reads/60s |
| C1-rev fenced Dekker, no lost wake | D3 | yes (fixes the fatal x86 store-buffering hazard) | **CORRECT** | broken producer: 5.9–13.2% lost-wake hit rate over 10M rounds; fixed (unconditional fence): 0/10M both compilers |
| C2-rev sweep_all correctness | D3 | yes (fixes two fatal bugs: early-fire of not-due overflow entries, stale-clock re-arm) | **CORRECT** | red team's own bug probes: BUG A fired=0 (was 1), BUG B re-arm sees corrected `now_` (was stale 0) |
| C3 config validated fail-fast, no mutation | D3 | yes | **CORRECT** | reject table matches exactly; byte-for-byte memcmp confirms zero mutation on every call |

## Decision

**D2 — Fine-Base Cascading Wheel + Adaptive-Poll Timekeeper — wins.**

Rationale, in the ranking order given:

1. **Safety gate**: no design's WRONG claim is of `kind: safe`, so no design is
   disqualified outright. But D1's surviving liveness claim (C1) is only
   correct *conditioned on* a fix to 002/ADR-010's `scan_and_run` that does
   not exist in the codebase today — confirmed empirically by starving a
   sibling shard's already-queued message (and, by extension, its due
   timer) for 497–591ms behind a continuously-loaded shard on the same
   worker, using the real, unmodified `engine.hpp` with no timer code
   involved at all. That is a real external dependency this ADR cannot
   close by itself (see Residual risks). D2 and D3 do not carry this
   contingency — their proven claims stand on their own.

2. **Proven beats claimed**: D2 is the only design where **every** claim that
   survived red-teaming was also proven CORRECT by executed evidence —
   9 for 9 (F1-rev, F2-rev, S1, S2, S3, C1-rev, C2, C3, C4), zero disproven.
   D1 has one disproven fast-claim (F2's burst sub-case: 812×–2114× over its
   own stated bound). D3 also has one disproven fast-claim (F2: 37ns vs. a
   claimed <20ns budget) whose root cause — the intrinsic cost of one
   `steady_clock::now()` read — is baked into *every* drain-owner
   acquisition of a timer-active shard under D3's scheme, not a rare edge
   case. Per the ranking rule, disproven claims count against a design;
   D2 is the only one with none.

3. **Measured hot-path numbers among safe survivors**: D2's `advance_to()`
   cost is the closest to genuinely gap-independent of the three — a
   100× larger elapsed gap (100µs → 10ms) cost only 1.24× more
   (239.5ns → 297.4ns), against a 78× cost increase a naive
   linear-in-elapsed-ticks model would predict. That is the strongest
   direct, executed evidence of solving OQ2 (drift/catch-up cost) as
   asked. D2's `cancel()` stays O(1) regardless of tier/overflow residency
   (1.82–2.47ns, all within 1.36×), and its idle-shard lateness tracks the
   timekeeper's own adaptive cadence, not tick fineness, exactly as the
   target requires ("a finer tick raises the cost of both active-advance
   catch-up and timekeeper-driven correction" — D2 empirically decouples
   these).

4. **Core invariants**: D2 preserves every ground-rule invariant unmodified —
   O(1) insert, O(1) lazy cancel, no locks, no heap alloc in the wheel span,
   single-writer per shard, targeted (never broadcast) timekeeper wake,
   deadline tombstoning via the normal `tell`/mailbox path, priority
   orthogonality, and the overflow-heap far-future tier is kept verbatim.
   The bugs the red team found (tier-0 slot-skip drop, missing
   generation-check on the CAS-failure branch, cascade-invocation-count
   mental model) were all algorithmic implementation defects in the
   original sketch, not violations of an invariant, and all three were
   fixed and re-verified without weakening any invariant.

## Residual risks

- **Scheduler shard-fairness gap (cross-cutting, not fixed by this ADR)**:
  `scan_and_run` restarts from the top of `scan_order` on every productive
  drain, so a continuously-loaded shard can starve sibling shards on the
  same worker (shard_count > worker_count) unboundedly — reproduced on
  real, unmodified `engine.hpp` (497–591ms for a 10M-message burst; the
  window is unbounded under a continuous producer). This affects timer
  *liveness* under any of the three designs, since the timekeeper's
  targeted wake is a documented no-op against a worker that is busy
  elsewhere rather than parked. Must be closed in 002/ADR-010, independent
  of which timer design is adopted.
- **No tested pathological "gap exceeds full wheel span" backstop for D2**:
  D3's design needed (and, after two fatal bugs, fixed) an explicit
  `sweep_all` safety net for a shard unserviced longer than its own span.
  D2's `advance_to()` was shown flat-cost up to ~113.8h-equivalent gaps in
  testing, but no dedicated pathological-path test exists for a gap that
  exceeds D2's ~19.09h native span while the shard is still under active
  (non-timekeeper) advance. Recommend adding one before Accepted → shipped.
- **TimerEntry pool sizing under lazy-cancellation**: a Deadline entry
  occupies its pool slot until its *original* fire tick even if the message
  completed early (011's own lazy-cancellation contract, not introduced by
  D2). Under a workload with generous deadlines and fast completions, pool
  occupancy tracks in-flight-deadline count, not concurrency, and can
  exceed it. D2's proven fix (a saturating `deadline_tracking_dropped`
  counter, no allocation, no block) keeps the zero-alloc/zero-lock
  guarantee intact but silently degrades deadline *enforcement* (not
  message delivery) for dropped entries once the pool is exhausted — needs
  a 023-style capacity-planning note and an alertable metric.
- **Fine-base tick still forces rounding**: D2's 64µs base tick means any
  sub-tick deadline still ceil-rounds up to the next tick, same class of
  contract as D1/D3 — needs the same explicit "never early, ceil-rounded to
  base tick" precision contract documented for callers.
- **Thundering-herd cascade risk**: many `schedule_every()` timers phase-
  aligned onto the same coarse tier slot (e.g., started together) all
  cascade down in the same `advance_to_now()` call, producing a latency
  spike on that one drain. Not measured in the executed evidence; workloads
  with large synchronized-periodic populations should add caller-side phase
  jitter.
- **`idle_check_interval` floor is a single node-wide knob**: pushing it very
  low trades idle-shard promptness for extra wakeup/CPU cost across the
  whole node, not per-actor; the executed C2 evidence recommends ≥1ms as a
  safe floor on this hardware but that is not a portable guarantee across
  deployment hardware.

## Spec recommendations for `011-Timers-and-Scheduled-Work.md`

1. **§Data structure — close OQ1.** Replace the current single-sentence
   wheel description with the concrete, proven tier scheme: 5 fixed tiers ×
   64 slots (6-bit indexing) on a 64µs base tick, giving a ~19.09h native
   span before the (unchanged) heap-overflow tier takes over. State this as
   a BuildOnly constant set (consistent with 013's Reconfig-class table),
   not a runtime knob.
2. **§Advancing the clock.** Document the corrected `advance_to()` algorithm
   explicitly: a busy shard's active-advance must scan an `occupied_mask`
   bit-window (`std::countr_zero` over the masked range) at each tier-0 wrap
   segment, firing every occupied slot in that window before jumping —
   *not* sample a single slot per iteration and jump past the rest (the
   fatal bug found and fixed in this ADR's proof). Note the guard needed
   when the jump step equals the full slot count (avoid shift-by-bit-width
   UB). Record the measured near-flat cost (1.24× cost for 100× gap) as the
   spec's own evidence that this closes OQ2's "finer tick raises catch-up
   cost" tension.
3. **§Deadlines.** Add an explicit note that the wheel's fire-time CAS on a
   message descriptor's `gen_state` must check the *generation* first and
   treat a mismatch as an unconditional no-op, before branching on state —
   the fatal bug found in this ADR's proof was a CAS-failure branch that
   inspected only state bits, letting a stale deadline spuriously
   `request_stop()` an unrelated, later message recycled into the same
   descriptor slot. Also require the CAS to preserve any legitimate nonzero
   `flags` bits on the Queued word rather than hardcoding them to zero.
4. **§Advancing the clock / new §Drift bound — close OQ3.** Document the
   proven drift-bounding mechanism: the wheel publishes exactly one
   `next_due_hint_` relaxed atomic per shard (no seq_cst fence needed — it's
   a one-directional publish, not a Dekker); the node-level timekeeper reads
   only that hint across all shards and **reprograms its own sleep to the
   global next-due minimum** (`sleep_for(min(next_due - now, idle_check_interval))`)
   rather than polling on a fixed cadence alone. State the proven bound:
   idle-shard lateness ≈ `idle_check_interval` + OS-wake jitter (measured
   headroom 250–700µs at a 1ms floor on this hardware), independent of base
   tick size.
5. **§Idle-timeout deactivation.** Note explicitly that the re-arm-with-
   cancel token mechanism (ADR-008) rides the same tier scheme and was
   re-verified O(1) under flap with this tier/tick change (S3 cancel-cost
   parity across tier0/tier4/overflow).
6. **§Dependencies / new note.** Document the TimerEntry pool-exhaustion
   policy: non-blocking, non-allocating, a saturating
   `deadline_tracking_dropped` counter (009-style), never a heap fallback —
   this was previously unspecified.
7. **§Open questions.** Remove item 1 (wheel granularity/tier sizing) and
   item 3 (timekeeper/active-advance drift) as closed by this ADR; retain a
   new item noting the unresolved 002/ADR-010 shard-fairness dependency
   (starvation of sibling shards on a saturated worker) as the actual
   remaining open liveness gap, to be tracked against 002/ADR-010 rather
   than 011.
8. **Cross-reference recommendation for `002-Scheduler.md` / ADR-010**: flag
   the reproduced shard-starvation gap (a continuously-loaded shard can
   starve sibling shards' drain-owner acquisitions — and hence their timer
   advance — unboundedly when shard_count > worker_count) as a required fix
   independent of which timer design is adopted, since the timekeeper's
   targeted wake cannot rescue a worker that is busy rather than parked.

## Tie-breaking experiment (if evidence had been insufficient)

Not needed — D2 has zero disproven surviving claims against one each for D1
and D3, and its measured catch-up-cost-vs-gap-size numbers are the strongest,
directly executed evidence of solving the stated OQ2 tension. If a future
revision wants to contest this, the single experiment that would matter most
is: re-run D2's F1-rev sparse/full-occupancy sweep and D1's F1 sweep on the
*same* harness/hardware with occupancy densities and gap sizes drawn from a
real production timer-population trace (mix of message deadlines,
`schedule_every` reminders, and ADR-008 idle-timeout re-arms) rather than the
synthetic uniform/sparse distributions used here, since D1's degenerate-case
sensitivity (occupancy-dependent forced-stop cost) and D2's near-flat
behavior were both demonstrated only under synthetic, not production-shaped,
occupancy patterns.
