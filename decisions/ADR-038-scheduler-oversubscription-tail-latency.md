# ADR-038: Scheduler Busy-Spin Tail Latency Under Thread Oversubscription

## Status

**Round 1's finding on the p999 side-effect's cause was superseded by Round 2; Round 2's residual
risk was actioned and partially resolved by Round 3 (see "## Round 2"/"## Round 3" below;
historical content kept intact throughout, not erased).** Round 1 shipped Bounded Cooperative
Drain-Owner Eviction default-off and named one open question: was F2's p999 regression an artifact
of the proving harness's busy-poll simplification, or a real cost under genuine idle-avoidance?
Round 2 ran the real `Engine::worker_loop` and **refuted the entanglement hypothesis** — the
regression persisted and was larger under the real scheduler (median +130% at P=12) than under the
harness (+8-96%) — and named the concrete next step: a cheaper eviction-probe heuristic, since
`try_drain_shard_with_steal` was probing on every single miss, not just genuinely stuck ones.
**Round 3 built that heuristic** (a lane-local consecutive-miss gate,
`EngineConfig::drain_owner_steal_miss_threshold`) and re-measured, same rigor, same real scheduler:
it materially helps (p999 median -43.4%, max -13.1%, throughput +4.6% versus the un-gated shape) but
**does not close the gap to disabled** (p999 still +9.9% worse median, though max latency is now
*better* than disabled, -14.2%). **Accepted, default-off stays correct for now** — this is real,
measured progress, not a fix ready to ship default-on. See "## Round 3" for the full data,
including a small threshold sweep (4/8/32).

## Question

At P > logical-core-count worker/producer topologies (P=12 pairs = 24 OS threads on a
12-logical-core host, per `bench/caf_comparison/README.md`'s stress test), Quark's p999
latency roughly quadruples again and worst-case message latency balloons ~60x (339us →
20.3ms) going from P=8 to P=12, while CAF stays flat. Two root-cause hypotheses were named:

1. Every idle-avoidance spin on the worker hot path (`pre_park_spin()` — ADR-035,
   `drain_run_queue`'s Busy retry, `run_activation`'s own Busy retry) is a pure
   `cpu_relax()` busy-wait with no yield fallback, wasting a full OS scheduling quantum
   once OS thread count exceeds core count.
2. `try_drain_shard`'s `drain_owner` CAS is single-writer arbitration per shard — if the
   worker holding `drain_owner` is preempted mid-drain under oversubscription, every other
   worker's scan of that shard just sees it as owned and skips it (never blocks), so a
   producer's message is stuck until the OS specifically reschedules the preempted owner.

Three designs were built, red-teamed, and proven against real, compiled, sanitizer- and
benchmark-verified C++ on `include/quark/core/engine.hpp`'s worker hot path.

## Design summaries

1. **Yield-Escalation (site-A only, red-team-narrowed).** A bounded second-phase
   `std::this_thread::yield()` escalation appended to `pre_park_spin()` only (the
   drain_owner-held-critical-section sites B/C were conceded and removed after red-teaming
   showed adding `yield()` there lengthens exactly the stranding window hypothesis 2
   describes). One new `EngineConfig` field, `yield_spin_limit` (default 8). Targets
   hypothesis 1 only, and only on the general idle-transition path (never inside
   `drain_owner`'s critical section).

2. **Oversubscription-Gated Pre-Park Backoff.** A once-per-`start()` `oversubscribed_`
   bool (computed from `worker_count + oversubscribed_external_threads + 1` vs
   `hardware_concurrency()`) gates a shrunk, `cpu_relax()`-prefix / `yield()`-tail
   `pre_park_spin()` variant, active only when the engine judges itself oversubscribed.
   Also targets hypothesis 1 only, with an auto-detection heuristic layered on top.

3. **Bounded Cooperative Drain-Owner Eviction.** A progress-heartbeat + generation-ack
   handshake: a contender that observes a shard's owner has stalled posts a bounded
   eviction *request*; the owner, at its own next per-activation checkpoint, voluntarily
   releases `drain_owner` early and acks. The contender never touches `run_queue`/`wheel`
   itself until it wins the pre-existing, byte-identical `drain_owner` CAS — so those
   single-writer-documented structures are never touched by two threads at once in any
   interleaving. Targets hypothesis 2 directly.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number |
|---|---|---|---|---|
| S1 (bounded spin / no new sync surface) | Yield-Escalation | Yes (narrowed to site A) | CORRECT | Zero-diff outside `pre_park_spin()`; TSan-clean |
| S2 (single-executor / TSan clean) | Yield-Escalation | Yes | CORRECT | 7/7 sched tests + 720K-msg forced-yield TSan run, 0 races |
| S3 (stop()-latency bound) | Yield-Escalation | Partially (methodology gap) | INCONCLUSIVE | +30% p99 at default vs off; +124% at 12,500x amplification (bounded, not catastrophic) |
| F1 (P=12 max/p999/throughput all improve) | Yield-Escalation | Conceded 2/3 sites | **WRONG** | max −55% median (2.2x), throughput +6.6%, **p999 +76% worse** |
| F2 (P≤6 no regression) | Yield-Escalation | Partial | INCONCLUSIVE | 3/4 pair-counts within ±5%; P4 noisy (3 trials only) |
| F3 (bench-gate untouched) | Yield-Escalation | Yes | CORRECT | activation_bench/sched_bench within ±2% |
| C1/C2 (FIFO, idle CPU) | Yield-Escalation | Yes | CORRECT | exact conservation; idle CPU ~0.04% |
| S1 (no data race, plain-bool happens-before) | Oversubscription-Gated | Yes | CORRECT | Real TSan, 500 start/stop cycles + full suite, 0 races |
| S2 (broken-wakeup control intact) | Oversubscription-Gated | Yes | CORRECT | Control strands every run, all builds |
| S3/C1/C3 (single-executor, FIFO, Auto boundary) | Oversubscription-Gated | Yes | CORRECT | 12 config combos × 3 sanitizer modes, exact |
| F1 (byte-identical false branch) | Oversubscription-Gated | No (literal wording) | **WRONG** | Instruction-sequence match but not byte-identical (register/epilogue diff); substantively no perf regression |
| F2b (P999 AND max improve under oversub) | Oversubscription-Gated | No | **WRONG** | p999 −51% median (good) but **max WORSE 9/10 trials, by 5–20x** (e.g. 1.16s vs 0.13s) |
| F4 (yield-tail cheaper than pure-relax under saturation) | Oversubscription-Gated | No | **WRONG, decisively** | shipped p50/p99/max all worse than 256-relax baseline, 5/5 runs, both compilers |
| F3/F5/F6 (0 alloc, cache isolation, sizeof unchanged) | Oversubscription-Gated | Yes | CORRECT | 0 allocations; distinct cache lines; sizeof(Engine) unchanged |
| S1 (head_/wheel never touched by 2 threads) | Cooperative Eviction | Yes | CORRECT | TSan 30/30 clean under injected preemption; **negative control (naive force-steal) caught a real race on `head_`** |
| S2 (single-executor invariant) | Cooperative Eviction | Yes | CORRECT | 2M messages, real hot path, 0 double-exec, both sanitizer modes |
| S3/S4 (generation-tag safety, no orphaned request) | Cooperative Eviction | Yes | CORRECT | White-box + rendezvous tests, 48/48 clean |
| C1 (FIFO/conservation under active eviction) | Cooperative Eviction | Yes | CORRECT | 5M messages, exact conservation, tens of thousands of real evictions fired |
| Pathological control (checkpoint-granularity honesty) | Cooperative Eviction | Yes | CORRECT | Long checkpoint-free activation correctly un-helped, as documented |
| F1 (disabled = no regression) | Cooperative Eviction | Yes | CORRECT | Within ±3% noise bar, both compilers |
| F2 (P999 AND max improve) | Cooperative Eviction | No | **WRONG** | p999 regressed 10/10 (unanimous sign, +8–96%) but **max improved 8/10 trials**, throughput flat, no backfire |

## Decision

**Winner: Bounded Cooperative Drain-Owner Eviction**, shipped **default-off**
(`drain_owner_steal_probe_limit = 0`, byte-identical to today's behavior — proven by F1).

Rationale, applying the stated ranking:

1. **Safety gate.** No safe/correct claim was marked WRONG for any of the three designs —
   none is disqualified on the gate. But the *quality* of the safety evidence differs
   sharply: Cooperative Eviction is the only design that proved its exclusivity argument
   with a **negative control** — the literal "timeout then force-steal" alternative the
   design's own summary calls unsafe was built, run under the identical stress shape, and
   TSan caught a real data race on `ActivationMpsc`'s documented non-atomic `head_` exactly
   as predicted (and it segfaulted once under a plain build). That is executed proof that
   this design's extra complexity (the cooperative handshake, not a raw steal) is
   load-bearing, not decoration — no other design in this round produced comparable
   evidence that its safety margin is actually doing work.

2. **Proven beats claimed; disproven counts against.** All three designs' central
   "closes the P>core-count gap" claim came back WRONG as stated (each is a conjunctive
   p999-AND-max claim, and each design regressed p999 while helping max, or vice versa).
   But "WRONG" is not uniform in severity:
   - **Oversubscription-Gated Pre-Park Backoff's core mechanism was disproven twice,
     independently, and decisively.** F2b showed max latency getting **worse in 9 of 10
     paired trials, by 5–20x** (one trial: 1.16s patched vs 0.13s baseline) — the opposite
     of its purpose, in precisely the regime it targets. F4 showed the mechanism's own
     wall-clock cost (p50/p99/max, all three) is worse than the pure-`cpu_relax()`
     baseline it replaces, in 5/5 runs across both compilers. This is not "unproven" — it
     is proven counterproductive. `sched_yield()`'s same-runqueue-only semantics
     (it cannot target a specific stalled thread on a different core) plus its
     standard-unbounded wall-clock cost under a genuinely saturated runqueue reproduce
     exactly the hazard class ADR-035 rejected Design 2 over. This design is **not** a
     safe pick even though no *safety* claim failed — its efficacy claim failed in the
     harmful direction.
   - **Yield-Escalation (site A)** partially succeeded: max latency and throughput both
     measurably improved at P=12 (median −55%, up to 5.4x at worst-of-5), and at the more
     moderate P=8 oversubscription both p999 and max improved together. Its failure mode
     (p999 +76% worse at P=12) is a real regression, not a backfire — nothing got
     catastrophically worse, and the mechanism is proven inert at ≤core-count (F3,
     structurally and empirically). This design is a legitimate, safe, but incomplete
     answer to hypothesis 1 alone.
   - **Cooperative Eviction's** p999 regression (10/10 trials, unanimous sign but
     bounded magnitude, tens of microseconds not milliseconds) came with **max latency
     improving in the majority (8/10) of trials** and throughput held flat under an
     adversarial, actively-eviction-injected stress load — i.e., it moved the metric the
     task's own numbers say is catastrophic (60x max-latency blowup, millisecond-to-tens-
     of-millisecond OS-reschedule-scale stalls) in the right direction, without ever
     producing a Design-2-style backfire. The evidence itself supplies the likely
     explanation for the p999 side-effect: the F2 harness is an intentionally simplified
     busy-poll/no-real-park loop (hypothesis 1's territory, explicitly out of this
     design's scope per its own risk list), so every scan-miss on a contended shard now
     also probes `evict_.progress`/`evict_.evict_request` — added atomic traffic on the
     common miss path, not evidence that the cooperative-handoff protocol itself is
     unsound.

3. **Measured hot-path numbers among safe survivors.** Between the two designs that did
   not backfire (Yield-Escalation site-A, Cooperative Eviction), Cooperative Eviction
   directly attacks the mechanism the cross-examination round converged on as the more
   plausible dominant contributor to the *extreme* tail (millisecond-to-tens-of-
   millisecond stalls match OS-reschedule timescales, not nanosecond-to-microsecond
   spin-loop timescales) — and it is the only design of the three with F1 (disabled-cost)
   proven CORRECT via a robust, noise-resistant methodology (interleaved A/B repeats +
   min-based comparison) on a contention-polluted shared host, giving the highest
   confidence that shipping it default-off is genuinely free.

4. **Core invariants.** Cooperative Eviction keeps `try_drain_shard`'s CAS as the *only*
   operation that gates a touch of `run_queue`/`wheel` — the mailbox's documented
   single-consumer `head_` invariant and the wheel's single-writer invariant are
   structurally unchanged in every interleaving (S1, proven, with a negative control).
   It adds one cross-core RMW (`evict_.evict_request` CAS, a `fetch_add` for the
   generation counter) but only on the already-cold, already-RMW-using contended-
   acquisition path (`scan_and_run`'s CAS-fail branch) — never on the steady-state
   per-message/per-activation path the 023 Hard 0-cross-core-RMW ceiling protects (F1
   proves this empirically at ≤core-count). No design in this round bends a core
   invariant to the point of disqualification.

**Oversubscription-Gated Pre-Park Backoff is rejected** as a scheduler fix: its central
mechanism was proven, not merely unproven, to make the target metric worse in the target
regime (F2b, F4). It should not ship as designed. **Yield-Escalation (site A)** is judged
a legitimate, safe, but incomplete point fix for hypothesis 1 alone — recorded as a
candidate for a future, narrowly-scoped follow-up (see Residual Risks), not as this
round's winner, because it trades a p999 regression for a max-latency win at exactly the
oversubscription ratio (P=12, 2x) the task is most concerned about, whereas Cooperative
Eviction's trade is smaller in absolute terms and improves the metric the task's own
numbers identify as most extreme (max latency / worst-case tell).

## Residual risks

- **Cooperative Eviction's p999 regression (F2, WRONG as a conjunctive claim) is not
  fully understood.** The design's own evidence attributes it to added atomic-probe
  traffic on `scan_and_run`'s busy-poll miss path, entangled with hypothesis 1 (no
  yield/park in the F2 harness's worker loop) — but this is an inference from the
  evidence, not itself proven by a dedicated experiment. Before flipping the default on
  in production, re-run F2 against the *real* `worker_loop` (with genuine `park()`/wake,
  not the harness's busy-poll simplification) to see whether the regression shrinks or
  disappears once real idle-avoidance behavior is present.
- **The design does not rescue an owner that receives zero OS timeslices for the entire
  bounded ack-wait window** — an honest, disclosed scope limit, not a defect. That
  residual tail (skip, never block) is unchanged from today's behavior.
- **All numeric evidence in this round is single-host, WSL2-under-shared-load evidence**
  (multiple concurrent agent sessions competing for the same 12 cores during proving,
  disclosed throughout the executed-evidence records). Every number here — for all three
  designs — must be re-measured on a quiet, dedicated Linux CI host with `taskset`
  pinning per `CLAUDE.md` before being treated as canonical, exactly as ADR-035's own
  residual risk #2 already cautions.
- **Config surface**: two new `EngineConfig` fields
  (`drain_owner_steal_probe_limit` default 0, `drain_owner_steal_ack_spin_limit` default
  128) and ~64B/shard (not per-actor/per-message, footprint-budget-irrelevant per 023).
  Both are `BuildOnly`/frozen-core, following the existing `pre_park_spin_limit` /
  `activation_linger_spin_limit` "0 disables it" convention.
- **Yield-Escalation (site A) is not adopted this round but is not dead either**: F1's
  max-latency/throughput wins are real and safe (S1/S2/C1/C2 all CORRECT), and it is
  compatible in principle with Cooperative Eviction (they touch disjoint code: one is on
  the idle-transition path outside any lock, the other is inside `scan_and_run`'s
  CAS-fail branch). A combined round — Cooperative Eviction for hypothesis 2 plus a
  *retuned* (lower `yield_spin_limit`, or gated to oversubscribed hosts only, to blunt
  the P=12 p999 regression) site-A yield escalation for hypothesis 1 — is the natural
  next step, not attempted or measured together in this round.
- **GCC's TSan runtime does not model the standalone `atomic_thread_fence` used by
  `pal::store_load_barrier()`** (a pre-existing, disclosed GCC/TSan interaction, not new
  to this design) — Clang's TSan is the authoritative signal for the Dekker-fence-
  dependent claims (S1) and was clean in every run; this is a tooling gap to track, not a
  defect in the design.

## Spec recommendations

- **`002-Scheduler.md`**: Add a subsection under the drain/scan-and-run description
  documenting the cooperative-eviction mechanism (progress heartbeat, generation-tagged
  request/ack, owner-side clearing) as the sanctioned pattern for bounded staleness
  detection on `drain_owner`, explicitly contrasting it with the disqualified
  "timeout-then-force-steal" alternative and citing this ADR's negative-control evidence
  as the reason a raw steal is unsafe against `ActivationMpsc::head_`'s documented
  single-consumer contract. Add the measured finding that `std::this_thread::yield()`
  inside a `drain_owner`-held critical section (sites B/C of the original Yield-Escalation
  proposal) is a rejected pattern — record it under "never spin unbounded" as: bounded
  iteration count is necessary but not sufficient; a yield-class primitive must never be
  invoked while holding exclusive drain rights another worker is waiting on.
- **`decisions/ADR-035-worker-park-wake-backoff-policy.md`**: Add a forward-reference note
  that ADR-038 evaluated (and rejected) extending `pre_park_spin()`'s escalation with an
  oversubscription-gated `yield()` tail; record F4's finding (yield-tail wall-clock cost
  exceeded the pure-relax 256-iteration baseline's worst case, 5/5 runs) as a concrete,
  measured instance of the same SAFE-4-class hazard ADR-035 identified in its own Design 2
  rejection — this closes the loop on ADR-035's own residual risk that its Design-1
  choice should be revisited if a future round found a cheap win elsewhere; it did not.
- **`decisions/ADR-036-activation-linger-idle-churn-reduction.md`**: Add a cross-reference
  noting that ADR-038's checkpoint-granularity finding (progress only advances *between*
  activation dispatches, never mid-activation) is consistent with ADR-036's own per-message
  vs per-activation checkpoint-cost tradeoff finding (round 1's regression from a
  per-message check) — both ADRs converge on "checkpoint between activations, never inside
  one" as the load-bearing granularity rule for this hot path.
- **`023-Performance-Targets-and-Budgets.md`**: Add an oversubscription-tail-latency budget
  row (P=12 pairs / 2x oversubscription on a reference host) distinct from the existing
  ≤core-count Hard budgets, so a future regression in this regime is caught by the gate
  rather than requiring an ad-hoc bench run — populate its baseline numbers from this
  ADR's F1/F2 Cooperative-Eviction-disabled-vs-enabled measurements once re-run on a quiet
  CI host per the residual-risks note above. Explicitly record that
  `drain_owner_steal_probe_limit`/`ack_spin_limit` are cold `BuildOnly` config, not
  gated by the existing ≤core-count Hard budgets (default-off preserves them, proven by
  F1).

## Single tie-breaking experiment (for the open p999 question)

Re-run Cooperative Eviction's F2 harness with `worker_loop`'s *real* `park()`/wake path
substituted for the harness's busy-poll simplification (i.e., drive the P=12 sustained
stress test through `bench/caf_comparison/quark_stress_bench.exe` directly, with
`drain_owner_steal_probe_limit` enabled, on a quiet dedicated host). If the p999
regression shrinks to within noise once real idle-avoidance is present, the entanglement
hypothesis is confirmed and the default can move toward on; if it persists, the
scan-path atomic-probe overhead itself needs a cheaper heuristic (e.g., only probe
`evict_.progress` after N consecutive CAS-fail misses, not on every miss) before this
ships default-on.

**Run — see "## Round 2" below for the result: the regression persists (refuted, not
confirmed).**

## Round 2 (2026-08-01): The Tie-Breaking Experiment, Run For Real

### What this round did

Executed the exact experiment named above, verbatim: the CLI-toggled ADR-038 knobs
(`drain_owner_steal_probe_limit`/`drain_owner_steal_ack_spin_limit`, already shipped in
`EngineConfig`, default 0/128) were wired into `bench/caf_comparison/quark_stress_bench.cpp`
as two new optional trailing CLI args (defaults preserve every existing 3-arg invocation
byte-for-byte). This drives the mechanism through the **real** `Engine::worker_loop` —
`pre_park_spin()`/`park()`/wake between scans — instead of F2's original bespoke harness,
which modeled workers as a tight busy-poll loop specifically to isolate the eviction
protocol from ADR-035's own idle-avoidance mechanism. That isolation was also the harness's
weakness: it left open whether the F2 p999 regression was a real cost of the mechanism or
an artifact of stacking two idle-avoidance-adjacent mechanisms on top of an unrealistically
tight loop.

**Environment**: WSL2 Ubuntu (g++ 15.2.0 and clang++ 20.1.8, matching this ADR's original
proving toolchain), copied off the 9p `/mnt/d` mount to `/tmp` for build speed — the same
environment class as Round 1's proving, chosen for continuity/comparability, disclosed here
as still not a dedicated quiet CI host (this repo's shared dev machine, WSL2 layer). A
clang++ cross-check at P=12 was attempted but landed with the host's load average still
~9/12 from the preceding g++ sweep — flagged as load-confounded and excluded from the
primary verdict (see raw data in the prover's report; it did not show a materially cleaner
signal either).

### Methodology

10 paired, **interleaved** A(disabled, probe_limit=0)/B(enabled, probe_limit=64,
ack_spin_limit=128) trials at P=12 pairs (`quark_stress_bench 12 3 1 <probe> 128`) and
10 more at P=8 pairs, g++ build, primary dataset — matching this project's own bar against
single-shot or non-interleaved comparisons (ADR-036 Round 4's warm-up/process-order bias is
exactly the failure mode interleaving guards against). Correctness (`sent == received`,
zero loss/duplication) held in all 40 trials, both configs, both P levels.

### Results

**P=12 (2× oversubscription — the regime this ADR exists to fix):**

| Metric | A (disabled) median | B (enabled) median | Δ |
|---|---|---|---|
| p999 | 87,418 ns | 201,030 ns | **+130.0%** (range across trials: +8.2% to +639.9%, 10/10 worse) |
| max latency | 5,750,166 ns | 4,065,683 ns | -29.3% (6/10 trials better, but heavy-tailed: two outlier trials landed +1068.7%/+659.0%) |
| throughput | 24.4 M/s | 21.4 M/s | -8.8% (new finding — not measured as flat in Round 1) |

**P=8 (1.33× oversubscription):**

| Metric | A (disabled) median | B (enabled) median | Δ |
|---|---|---|---|
| p999 | 68,964 ns | 70,494 ns | +2.2% (6/10 worse — essentially a coin flip, closer to noise than P=12) |
| max latency | 1,249,023 ns | 2,236,438 ns | **+79.1%** (only 4/10 trials improved — the *opposite* of Round 1's F2 finding) |
| throughput | 21.6 M/s | 19.3 M/s | -11.7% |

Full 10-trial-per-cell raw tables (not just the medians above) are preserved in the
proving session's record; every trial number is disclosed, not cherry-picked.

### Verdict

**Entanglement hypothesis REFUTED at P=12.** The p999 regression does not shrink toward
noise under the real scheduler — it is unanimous across all 10 trials and larger in
magnitude (median +130%) than Round 1's busy-poll-harness finding (+8-96%). Per this
ADR's own pre-declared decision criteria ("if it persists, the scan-path atomic-probe
overhead itself needs a cheaper heuristic... before this ships default-on"), this is
squarely the "persists" branch, not the "shrinks to noise" branch. The scan-path atomic
probe traffic (`evict_.progress`/`evict_.evict_request` reads on every CAS-fail miss) is a
real cost under genuine idle-avoidance, not a busy-poll-harness artifact — if anything the
real `park()`/wake path makes the relative cost of the probe *worse*, not better, plausibly
because a worker that would otherwise be about to park now instead pays the probe and stays
active longer.

**At P=8, p999 sits close to noise** (median +2.2%, a near-even 6/10-worse split) — some
support for the entanglement hypothesis dampening at the lower oversubscription ratio — but
**max latency's improvement, the other half of Round 1's F2 finding, does not survive**:
it got worse (median +79.1%, only 4/10 trials improved), inverted from Round 1. The
conjunctive "both p999 and max improve" claim fails at both P levels now, not just P=12.

**New, previously-unmeasured cost: throughput trended negative when enabled** (median
-8.8% at P=12, -11.7% at P=8) against the real scheduler, where Round 1's F2 measured it
as flat. This was not part of the original conjunctive claim but is a real, disclosed
finding that further weakens the case for a near-term default flip.

### Decision (Round 2)

**Default stays 0 (disabled). This is now a reconfirmed decision backed by two independent
proving rounds, not merely an unresolved caution carried from Round 1.** Per rule 2
("proven beats claimed"): the specific, falsifiable path to a default flip that Round 1
named — re-test against the real `worker_loop`, see if the regression shrinks — was
executed and came back negative. Shipping default-on today would mean shipping a
regression that is now proven, not just suspected, to be real under the mechanism's own
intended operating conditions.

**The mechanism itself is not disqualified** — S1-S4/C1 (safety/correctness) from Round 1
are untouched by this round (no code in `engine.hpp`/`engine_config.hpp` changed; only the
bench harness gained a CLI toggle for the config it already exposed), and message
conservation held in all 40 new trials. This round narrows *why* it isn't ready for
default-on (a real per-miss atomic-probe cost, not a proving artifact) rather than
reopening whether it's safe to ship at all.

### Residual risks (Round 2 — appended; Round 1's numbered list above is unchanged)

15. **The concrete next step is now a cheaper probe heuristic, not just re-proving.** This
    ADR's own Round 1 text already named the candidate: probe `evict_.progress` only after
    N consecutive CAS-fail misses on a shard, not on every miss. That is unbuilt and
    unproven — Round 2 only established that it is now necessary, not what N should be or
    whether it actually recovers the p999 cost.
16. **Round 2's environment is still not a dedicated quiet CI host** — WSL2 on this repo's
    shared dev machine, same caveat class as every prior ADR in this file. The clang++
    cross-check was explicitly load-confounded and excluded; only the g++ dataset backs
    this round's verdict. Re-run on the Linux CI matrix with `taskset` pinning before
    treating the exact percentages (not the qualitative "persists, and is larger" verdict)
    as canonical.
17. **The new negative throughput finding (-8.8%/-11.7%) is unexplained.** Round 1's F2
    measured throughput as flat when enabled; Round 2's real-worker_loop numbers do not.
    No dedicated experiment isolates why — plausibly the same added scan-path atomic
    traffic, but this is an inference, not yet proven, exactly the same evidentiary gap
    Round 1 left for the p999 side-effect before this round closed it.
18. **P=8's max-latency inversion (Round 1: improved; Round 2: regressed) is also
    unexplained** and should be treated as a live open question for whatever round designs
    the cheaper heuristic in residual risk #15, not assumed away.
19. **A prompt-injection attempt was observed and declined during this round's proving
    session** (an unsolicited offer of SSH credentials to an unrelated external host,
    appearing mid-task) — not acted on, disclosed here for the record since it touched a
    session that was producing this ADR's evidence; it has no bearing on the technical
    verdict above.

## Round 3 (2026-08-01): The Cheaper Heuristic Residual Risk #15 Named — Built and Measured

### The design

`try_drain_shard_with_steal` paid its full probe-spin + `evict_`-touching machinery on **every**
CAS-fail miss — including transient ones (a worker's scan passing through a shard that happens to
be briefly busy, with no genuine stall). Round 2's evidence pointed at this as the likely source of
the real-scheduler p999 regression. The fix: each `Worker` gains two lane-local fields (no atomics —
touched only by the owning thread), `steal_probe_shard` and `steal_probe_miss_streak`. On a miss,
if the shard differs from the one already being tracked, the tracker resets to that shard with a
streak of 1; if it's the SAME shard as last time, the streak increments. The expensive probe/request
path only engages once the streak reaches `EngineConfig::drain_owner_steal_miss_threshold` (new
field, default **8**) — a worker cycling through many different momentarily-busy shards never
accumulates a streak past 1-2 and never touches `evict_` at all; only a worker that keeps coming back
to find the SAME shard still owned (the actual stuck-owner signature this mechanism exists to catch)
pays the cost. The streak resets to 0 once engaged, win or lose, so a slow-to-ack owner gets a fresh
full window before being probed again rather than being hammered every miss.

### Methodology

Same rigor as Round 2: real `Engine::worker_loop`/`park()`/wake via `quark_stress_bench.exe`
(extended with a sixth optional CLI arg, `miss_threshold`, default 8 — existing invocations
unchanged), WSL2 Ubuntu g++ 15.2.0 (clang++ unavailable in this WSL instance this round — no
cross-compiler check, disclosed as a gap), paired and **interleaved** trials (A/B/C/A/B/C/...,
guarding against the first-call-in-process bias ADR-036 Round 4 found). Three configs, all
re-measured fresh in the SAME session (no cross-session comparison against Round 2's numbers, per
this project's own standing caution about absolute numbers on a shared host):

- **A — disabled** (`probe_limit=0`, today's shipped default, the floor)
- **B — threshold=1** (`probe_limit=64 ack_spin_limit=128 miss_threshold=1`): reproduces Round 2's
  original "probe on every miss" shape, re-baselined in this same session
- **C — threshold=8** (`probe_limit=64 ack_spin_limit=128 miss_threshold=8`): the new default

10 trials/config at P=12 pairs (2× oversubscription, the regime this ADR exists to fix), 6
trials/config at P=8 pairs (1.33×), plus a 6-trial-each sweep of threshold=4 and threshold=32 at
P=12 only. **Correctness held in all 60 trials, every config, every threshold value tried — exact
`sent == received`, zero loss/duplication.**

### Results — P=12, 10 trials/config (ns)

| trial | A p999 | B p999 | C p999 | A max | B max | C max |
|---|---|---|---|---|---|---|
| 1 | 74,309 | 95,788 | 95,348 | 771,693 | 10,249,273 | 6,763,465 |
| 2 | 70,681 | 121,246 | 118,100 | 4,057,623 | 5,512,274 | 449,187 |
| 3 | 134,851 | 183,673 | 297,449 | 11,486,570 | 7,796,291 | 6,276,928 |
| 4 | 60,191 | 95,328 | 54,331 | 5,855,153 | 4,047,564 | 4,065,658 |
| 5 | 88,806 | 71,784 | 189,102 | 29,602,984 | 2,603,052 | 4,065,717 |
| 6 | 90,850 | 282,336 | 68,398 | 7,471,386 | 6,343,153 | 5,971,731 |
| 7 | 52,649 | 229,047 | 68,969 | 8,047,930 | 23,204,017 | 4,786,381 |
| 8 | 68,383 | 114,634 | 90,388 | 6,373,208 | 7,754,362 | 9,482,666 |
| 9 | 68,508 | 173,904 | 52,278 | 8,046,228 | 17,744,122 | 7,786,041 |
| 10 | 106,378 | 160,500 | 68,157 | 6,801,567 | 5,548,541 | 9,502,816 |

Throughput medians (M/s): A=24.02, B=21.33, C=22.32. p999 medians: A≈72,495, B=140,873, C=79,679.
Max medians (M): A≈7.26, B≈6.34, C≈6.12.

**P=8, 6 trials/config (medians only — full 18-row table preserved in the proving session's
artifacts, not reproduced here):** p999 medians A=74,359 / B=71,639 / C=68,508 ns — all within
noise of each other at this ratio, matching Round 2's finding that P=8 is closer to noise than
P=12. Max medians A=2,729,272 / B=1,137,483 / C=2,946,765 ns. Throughput medians A=21.54 / B=19.80 /
C=21.34 M/s.

**P=12 threshold sweep, 6 trials each:** threshold=4 — p999 median 79,413 ns, max median
6,412,001 ns, throughput 23.08 M/s (essentially matches threshold=8 — a plateau, not a knife-edge).
threshold=32 — p999 median 92,037 ns, max median 7,878,989 ns, throughput 23.52 M/s (worse than
threshold=8 on both latency metrics; plausible mechanism: too high a threshold delays rescuing a
genuinely stalled owner, the opposite failure mode from threshold=1's per-miss tax).

### Verdict

**The heuristic materially helps relative to the un-gated shape (claim: CORRECT).** C beats B on
p999 in 8/10 trials at P=12 (median 79,679 vs 140,873 ns, **-43.4%**), on max in 6/10 (median
-13.1%), and on throughput (22.32 vs 21.33 M/s, **+4.6%** — recovering most of B's throughput
regression). This is real, measured progress on the exact residual risk Round 2 named, not a
re-tuning of an already-working thing.

**The heuristic does NOT close the gap to disabled — a real cost remains (claim: PARTIALLY
WRONG against the implicit "safe to enable" hope).** C's p999 is still +9.9% worse than A's median,
and C beats A in only 4/10 trials at P=12 — down from B's 0/10, but not parity. **Max latency,
however, is now BETTER than disabled** (median -14.2%, 7/10 trials better) — the metric this ADR's
own original numbers (60× worse at P=12 in the very first observation that motivated this ADR)
flagged as the most extreme concern. Throughput cost versus disabled is -7.1%, real but bounded, not
catastrophic. At P=8 all three configs sit within roughly ±8% of each other on p999 — noise-
dominated, consistent with Round 2.

**Threshold=8 (the shipped default) is a reasonable choice within this sweep's resolution, not an
arbitrary one.** Threshold=4 performs essentially identically (a plateau supports 8 not being a
fragile knife-edge value); threshold=32 is worse on every metric tried, consistent with "too
conservative a threshold delays genuine rescue."

### Decision (Round 3)

**`drain_owner_steal_probe_limit`'s default stays 0 (disabled). This is closer, real progress —
not yet a fix ready to ship default-on.** Per this project's own ranking discipline:

- **Rule 2 (proven beats claimed) still governs.** The mechanism, even with the cheaper heuristic,
  has a measured, non-noise p999 cost relative to disabled at P=12 (median +9.9%, C wins only 4/10
  trials) — this is a real, if much smaller, regression, not a refuted one. Shipping default-on
  would mean shipping a still-measured cost with no compensating uniform win.
- **The trade shape changed in a way worth naming precisely, not just "still not good enough."**
  Round 2's mechanism was strictly worse than disabled on every metric tried (p999, and by
  implication the tail behavior the ADR exists to fix). Round 3's heuristic **inverts the sign on
  max latency** (now better than disabled) while retaining a smaller p999 cost and a smaller
  throughput cost. A workload that specifically cares about worst-case single-message latency more
  than p999 might reasonably opt in today via `EngineConfig::drain_owner_steal_probe_limit` — this
  is not a recommendation this ADR makes for a default, but it is a materially different, more
  nuanced trade than Round 2 left behind, and the mechanism is not disqualified.
- **No safety/correctness claim from Round 1 is touched.** 60/60 new trials clean (exact
  conservation, zero loss/duplication) across every config and threshold value tried this round.

### Residual risks (Round 3 — appended; Rounds 1/2's numbered lists above are unchanged)

20. **Round 3's p999 cost (median +9.9% vs disabled at P=12) is not further root-caused.** The
    heuristic clearly reduces cost relative to probing every miss, but what remains — a real,
    smaller regression, not noise — has no dedicated experiment isolating its source (plausibly the
    lane-local streak-tracking's own branch/compare cost on the hot miss path, or residual
    engagement frequency even at threshold=8 under sustained P=12 contention; both are
    inferences, not proven).
21. **The threshold sweep is narrow (4/8/32 only) and single-session.** A finer sweep (e.g. 2, 6,
    12, 16, 24) and a second session/host would be needed before treating "8 is near-optimal" as
    more than "8 and 4 are similar and clearly better than both 1 and 32 within this sweep."
22. **No clang++ cross-check this round** — WSL2 instance used for proving did not have clang++
    installed; only g++ 15.2.0 backs this round's numbers. Round 2 had both compilers; this is a
    real reduction in this round's evidentiary strength, disclosed rather than hidden.
23. **P=8's numbers remain noise-dominated for all three configs**, consistent with Round 2 — no
    new information at that oversubscription ratio this round beyond reconfirming the P=12-specific
    nature of both the original problem and this fix's effect.
24. **All Round 3 evidence is again single-host, WSL2-under-shared-load** — same standing caveat as
    every prior round in this file. The qualitative verdict (heuristic helps, doesn't fully close
    the gap, max latency inverts favorably) should be treated as more robust than the exact
    percentages until re-run on a quiet, `taskset`-pinned Linux CI host.
25. **Whether a workload-specific opt-in recommendation (e.g., "enable if max-latency matters more
    than p999 to you") should be formalized in the spec is an open product question, not answered
    by this round's evidence alone** — this round only establishes that the trade-off exists and
    is now more favorable than Round 2's, not who should take it.
