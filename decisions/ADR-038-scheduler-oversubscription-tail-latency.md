# ADR-038: Scheduler Busy-Spin Tail Latency Under Thread Oversubscription

## Status

Accepted (partial fix; default-off pending tuning round) — see Residual Risks and Follow-Up.

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
