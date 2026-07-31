# ADR-036: Activation Idle-Churn Reduction (Post-Drain Linger vs. wake_one() Cost Cuts)

## Status

Accepted (Design A's linger mechanism adopted, `activation_linger_spin_limit` defaulted to
**32**, not the originally-proposed uniform "<5% everywhere" claim — shipped with corrected,
honest scope). Design B's two cache-line/memory-order cuts are **not adopted** — proven to
carry no measured benefit on the real hot path. A round-3 adaptive-linger hybrid is scoped
but not designed, red-teamed, or measured.

## Question

ADR-035 (shipped) cut the OS-wake-syscall rate to 0.26–1.25% of sends (measured,
pinned Linux/WSL2). But `metrics_snapshot().activations` — full Idle→Scheduled
exec-state transitions per actor (`001-Actor-Execution-Model.md` §Lifecycle) —
remained high: 18–25% of messages in a tight single-producer loop still trigger a
full activation cycle (CAS + run-queue enqueue + `idle_mask_` scan) even when no OS
wake syscall fires. CAF achieves ~4x higher throughput in the same benchmark shape
(`bench/caf_comparison/`). Two designs were proposed, red-teamed, revised against
required fixes, and proven with real compiled+executed C++23 (real g++ 15.2.0 AND
real clang++ 20.1.8 on WSL/Ubuntu, real TSan, `taskset`-pinned).

## Design summaries

1. **Design A — Activation-Scoped Bounded Post-Drain Linger.** When
   `Activation::drain_step` finds its mailbox empty, bound-spin-recheck THIS
   activation's own mailbox (`activation_linger_spin_limit`, tested at 0/32/256)
   before committing to the Running→Idle transition; `exec_` stays `Running`
   throughout, so a racing producer's CAS fails cheaply and the message is picked
   up by the linger's own re-poll instead of a fresh Idle→Scheduled cycle.
   Required fix (fatal-risk-class, closed): the linger→dispatch hand-off was
   originally unspecified and would drop/duplicate the message the linger's own
   re-poll already dequeued; fixed by hoisting `DrainResult` directly into
   dispatch with no second dequeue call. `drain_step_governed_seq`'s admission
   bookkeeping is explicitly **out of scope** this round (not extended, not
   proven either way). `pal::cpu_relax()` gained a real ARM64 `YIELD` (code-reviewed,
   **not** hardware-verified — x86-64-only proving environment).

2. **Design B — Cut the fixed per-transition cost of `wake_one()`, not the
   transition frequency.** Cut 1: cache-line-isolate `idle_mask_` from
   `stopping_`/`running_`. Cut 2: downgrade `wake_one()`'s scouting `idle_mask_`
   load from acquire to relaxed, relying on `producer_wake_fence()`'s existing
   `seq_cst` fence for the happens-before edge. Required fix (closed): Cut 1 as
   originally described left `idle_mask_` and `stopping_`/`running_` on the same
   cache line (measured delta = 4 bytes, not ≥64) — fixed by aligning **both**
   sides plus a `static_assert` guard; standalone repro confirms Original(bug)
   delta=4/NO, NaiveFix delta=4/NO (reproduces the bug), Corrected delta=64/YES.

## Evidence table

Only claims that survived red-teaming (after required fixes) and were then run as
real, compiled, sanitized/instrumented C++23 count toward the decision.
Pacing-dependent results are recorded per-regime, not averaged away.

| Design | Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|---|
| A | F-handoff (linger→dispatch, no drop/dup) | safety (fatal-risk-class) | yes, required fix closed it | **CORRECT** | 2440 reps × 4 bounds, 0 loss/dup, pool outstanding→0, g++/clang++/ASan |
| A | F1: activations/sends <5% at default limit=32 | fast | yes | **WRONG as a uniform claim** — pacing-dependent | saturating 6.23%, light 4.98%, medium 6.22%, idle-ish 93.10% (vs 92.31% baseline — no real reduction when the natural gap between sends exceeds the 32-iteration window) |
| A | F1 at limit=256 | fast | yes | **CORRECT** in isolation, but see F2/F5 tradeoff | 0.23–0.48% in all 4 regimes |
| A | F2: ≤2% regression under sustained backlog, limit=32 | fast | yes | **CORRECT**, genuine win | +26.4% throughput under real 3-producer backlog; activations 23.98%→1.56% |
| A | F2 at limit=256 under same backlog | fast | yes | **WRONG direction, badly** | −50.6% throughput; activations 0.22% |
| A | F3: post() latency improves, limit=32 | fast | yes | **CORRECT**, consistent | 27–40% faster across all 4 pacing regimes, both compilers |
| A | F4: zero heap alloc | fast | yes | **CORRECT** | — |
| A | F5: activation-churn reduction, P∈{1,2,4} | fast | yes | **CORRECT**, robust at every P | e.g. P=2: 20.6–37.6% → 2.1–3.2% |
| A | F5: producer-side throughput, P∈{1,2,4} | fast | yes | **MIXED / confirms red-team's contention worry** | limit=32/P=2: real run-to-run variance incl. one −14% regression before improving in later runs; limit=256 trends negative under contention (−19.6%/−3.9%/+0.8% at P=2, one −36% outlier at P=1) |
| A | S1: single-executor, limit∈{0,1,32,256} | safe | yes | **CORRECT**, incl. TSan | 300k rounds/bound, real work-stealing pressure |
| A | S2: no lost wakeup, TSan-clean | safe | yes | **CORRECT** | 200k rounds/bound, 0 stalls, 0 TSan reports |
| A | S3: `QUARK_SCHED_BROKEN_WAKEUP` control still strands | safe | yes | **CORRECT** at every bound | g++ bound=256 stranded after 778 rounds vs bound=0 after 2 rounds — timing shift only, not a masking failure |
| A | C1: activation reduction genuine, not relocated | correct | yes | **CORRECT** | deterministic 21-msg sequence, 21/21 processed at limit=0/32, activations 21→1 |
| A | C2: BudgetExhausted/linger-Empty mutually exclusive | correct | yes | **CORRECT** | structurally guaranteed + verified on 20,000-msg mixed workload |
| A | C3: FIFO preserved | correct | yes | **CORRECT** at every bound | — |
| A | C4: bounded idle→Idle latency, load-independent | correct | yes | **CORRECT** | ~1.4–1.9µs/cycle at limit=32, unaffected by 3 unrelated busy-spin threads |
| B | C1: Cut 1 improves throughput/cache-misses | fast | yes (after alignment fix) | **WRONG** against pre-declared bar (≥3%, ≥80% win rate/5+ trials) | real 14-trial Engine measurement, 4 workers, forced park/wake every round: mean −1.6% (wrong direction), 8/14 (57%, coin-flip) trials won. Isolated synthetic control DOES show the mechanism is real (4.45x, 5/5) — swamped by futex/condvar + mailbox mechanics in the real round-trip |
| B | C2: safe (race-free) | safe | yes | **CORRECT** | TSan-clean, 5/5 on 2 targets + 9 more scheduler/mailbox/metrics TSan targets; 182/183 full suite (1 pre-existing, unrelated, environment-caused failure reproduced identically on unmodified baseline) |
| B | C3: byte-identical codegen for Cut 2 | safe | yes | **CORRECT**, now on real Linux GCC+Clang | 8/8 compiler×opt-level combos byte-identical via objdump diff |
| B | C4: fence-merge rejection | safe | carried forward, not re-tested | **CORRECT** (prior round) | two independent reasons, no new test needed |
| B | C5: wake_one() common case = 1 load, 0 CAS | fast (structural) | yes | **CORRECT** | objdump-confirmed on real compiled function |
| B | C6: relocating the activations metric changes counted edges | correct (prediction) | yes | **CORRECT**, prediction exactly confirmed | consumer-side relocation counts N+1 for N budget-exhaustion resumptions vs 1 at the producer-side placement; identical to baseline in the non-exhausted case |

## Decision

**Design A ships — but not as originally claimed.** The mechanism (bounded
post-drain linger with the `DrainResult`-hoist hand-off fix) is adopted with
`activation_linger_spin_limit` defaulted to **32**, exposed as an
`EngineConfig` knob exactly like ADR-035's `pre_park_spin_limit`. This is a
**corrected-scope win, not the design as pitched**:

- F1's own headline claim ("<5% activations/sends, default limit=32, uniformly")
  is **WRONG** and must not be restated as true in the spec. Under idle-ish
  traffic the natural gap between sends already exceeds the 32-iteration spin
  window, so the linger simply never engages there (93.10% vs. 92.31% baseline
  — negligible, but also **not harmful**: it is a no-op in that regime, not a
  regression).
- Where it matters most for this debate's own stated motivation — closing the
  ~4x gap to CAF, which shows up under real producer load, not idle silence —
  Design A delivers genuine, proven wins: F2's **+26.4% throughput** under
  3-producer sustained backlog (activations 23.98%→1.56%) and F3's **27–40%
  latency improvement**, consistent across every pacing regime and both
  compilers. F5 confirms the activation-churn reduction itself is robust at
  every producer count tested (P∈{1,2,4}).
- **limit=256 is explicitly rejected as a general default or alternative.**
  It clears F1 uniformly (0.23–0.48% in all 4 regimes) but regresses **−50.6%**
  under the same sustained-backlog condition where limit=32 gains +26.4%, and
  trends negative under contention at every P tested. There is no single fixed
  bound in the tested range that is simultaneously good under sparse traffic
  (wants large) and good under contended/backlog traffic (wants small); the
  debate brief's premise is confirmed, not resolved.
- **F5's throughput half is a disclosed, unresolved risk, not a disqualifier.**
  Red-team's specific worry — that the linger's re-poll contends with the
  *same* `tail_` cache line a producer is about to write, a structurally
  tighter overlap than ADR-035's pre-park spin (which polls a shard-wide
  probe, not the specific hot line a producer needs) — is **confirmed real**,
  not overblown: limit=32/P=2 showed a genuine −14% regression in one run
  before improving in others. This is real-run variance under contention, not
  a repeatable, structural failure (S1–S3/C1–C4 hold at every bound tested,
  incl. under TSan), so it does not trip rule 1 (no `safe`/`correct` claim was
  marked WRONG for Design A). It is carried forward as residual risk #2 below.

**Why 32 and not 256, and how the ADR-035 precedent applies:** ADR-035 chose
the smaller, tighter, non-adaptive bound (Design 1's 256-iteration fixed spin)
over a materially faster adaptive design specifically because the adaptive
design's worst case was large and loosely bounded (~380µs). That precedent
**does not disqualify Design A's fixed-bound shape** — a small fixed bound
(32) is exactly the shape ADR-035 preferred, and Design A's own worst-case
latency is proven small and bounded regardless of bound size (C4: ~1.4–1.9µs
at limit=32, unaffected by concurrent busy-spin). What the precedent *does*
support is the same "prefer the smaller, tighter bound over the larger,
more aggressive one absent a uniform case for the larger one" discipline —
which is exactly why 256 is rejected here: it is the "256"-shaped choice that
regresses hardest under the workload this scheduler exists to serve
(sustained backlog), the same category of bet ADR-035 penalized Design 2 for
making (a materially better average case purchased with a worse
realistic-workload outcome). Framed this way, the precedent cuts **for**
shipping Design A's small fixed bound, and **against** entertaining the
uniform-target 256 or holding this shipment for a not-yet-built adaptive
mechanism — no adaptive linger design has been proposed, red-teamed, or
measured this round (unlike ADR-035, where an adaptive alternative existed
and was measured before being rejected).

**`drain_step_governed_seq` (the `DrainBudget<N>`/admission-bookkeeping drain
variant) does NOT receive this mechanism.** It was scoped out of red-team
entirely (option b, matching how `drain_step_reentrant` is already excluded)
— actors using that drain variant see no change, positive or negative, from
this ADR. This must be stated explicitly in the spec so it is not assumed to
apply uniformly.

**Design B is not adopted.** Its own prover's honest conclusion — "not worth
shipping as a performance change" — is correct and this ADR affirms it: C1
(the entire point of Cut 1) came back **WRONG** against a pre-declared bar
chosen specifically to avoid over-crediting noise (mean −1.6%, 57% win rate —
statistically indistinguishable from a coin flip, and the isolated synthetic
control's 4.45x effect confirms the underlying false-sharing mechanism is
real but **completely swamped** by futex/condvar and mailbox mechanics in the
actual `wake_one()` round-trip). Cut 2's own evidence (C3: byte-identical
codegen across 8/8 compiler×opt-level combinations on real Linux GCC+Clang)
proves it has **zero effect** on the code actually generated for x86-64 —
acquire loads are already free on this ISA, so there is nothing for the
relaxed downgrade to save here. Per rule 2 ("proven beats claimed... disproven
claims count against the design") and this round's explicit "don't ship things
with no measured benefit" framing: the `static_assert` regression-guard has no
independent value to ship on its own — its entire purpose is to protect an
alignment change (Cut 1's cache-line isolation) that this ADR is declining to
adopt because it has no measured benefit and adds permanent padding to a hot
`Engine` struct. Shipping unused, harmless-but-pointless padding "because it's
cheap and safe" is exactly the kind of unproven-benefit change rule 2 exists
to filter out; C2–C6 being CORRECT establishes Design B is *safe*, not that it
is *worth shipping*. Design B is recorded as a closed, rejected sub-branch of
this round — revisit only if (a) a future workload profile shows real
contention specifically on `idle_mask_`'s cache line outside the measured
park/wake round-trip, or (b) Cut 2 is re-measured on an ISA where acquire
loads are not free (e.g. ARM64), where C3's byte-identical result would not
hold and a real saving might exist.

**No core invariant is bent.** Single-executor (S1, CORRECT at every bound
including under TSan), FIFO (C3, CORRECT at every bound), zero heap allocation
(F4, CORRECT), and the `BudgetExhausted`/linger-`Empty` structural exclusivity
(C2, CORRECT, empirically verified on 20,000 messages) all hold. The linger
never writes `exec_` during its spin, so the exec-state CAS contract
(`001-Actor-Execution-Model.md` §Mailbox) is untouched by construction, not by
luck — matching the pattern of every prior mailbox-hot-path ADR back through
ADR-029/031/032/035.

## Residual risks

1. **F1's pacing-dependency is disclosed, not resolved.** No single fixed
   `activation_linger_spin_limit` clears the original <5% target under both
   sparse and contended traffic; the shipped default (32) is chosen to win the
   contended/backlog case this debate was motivated by, at the acknowledged
   cost of doing nothing under idle-ish traffic. A future round should target
   an adaptive bound (widen when recent inter-send gaps are small, shrink or
   disengage when they are large) — the same shape of recommendation ADR-035
   made for its own park/wake backoff, now doubly motivated since both rounds
   independently converged on "a fixed bound cannot serve both traffic
   shapes well."
2. **The tail_-cache-line contention risk (F5) is confirmed real but
   unresolved.** Red-team's worry that the linger's own re-poll structurally
   contends harder with producers than ADR-035's pre-park spin was proven
   correct, not overblown: a genuine −14% regression was observed in one
   P=2/limit=32 run before later runs improved. This is disclosed variance
   under contention, not a repeatable failure — S1–S3/C1–C4 held at every
   bound including limit=32 under TSan — but it means limit=32's throughput
   benefit under producer contention (as opposed to under backlog, which is
   proven clean at +26.4%) is not yet a settled, always-positive result. Needs
   a wider trial count at P=2/4 before being treated as tuned.
3. **`pal::cpu_relax()`'s ARM64 `YIELD` instruction is unverified on real
   hardware.** The proving environment this round was x86-64-only; the
   implementation is code-reviewed correct but has zero executed evidence on
   an actual ARM64 target. Must be verified (compiled + run, ideally under
   TSan) on real ARM64 before this ADR's claims are treated as portable, not
   just x86-64-proven.
4. **`drain_step_governed_seq` remains entirely out of scope.** Actors
   configured with `DrainBudget<N>`-style admission bookkeeping get no
   activation-churn benefit from this ADR, and — because the interaction was
   never designed or red-teamed — no proof that the linger mechanism would
   even be safe to extend there. Any future round doing so starts from zero,
   not from this ADR's evidence.
5. **Design B's underlying false-sharing mechanism is real but currently has
   no home.** The isolated synthetic control's 4.45x effect (5/5 trials) is a
   genuine, reproducible measurement of `idle_mask_`/`stopping_`/`running_`
   false sharing — it just doesn't matter in the current `wake_one()`
   round-trip, which is dominated by futex/condvar cost. If a future change
   removes or shrinks that syscall-bound cost (e.g., a much cheaper wake path),
   Cut 1 should be re-measured, since the effect it targets has not been
   disproven, only shown to not matter at the current bottleneck.
6. **All numeric evidence this round is single-host (WSL/Ubuntu on the same
   dev box).** Per `PERFORMANCE.md` convention and ADR-035's own residual-risk
   #2, none of the percentages above (F1–F5, C1) should be treated as
   canonical across hardware/kernel combinations until re-measured on the
   Linux CI matrix with the same `taskset`-pinning discipline.

## Spec-update recommendations

**`002-Scheduler.md`**
- Add a new subsection immediately after the existing `### Idle backoff before
  park (ADR-035)`, titled `### Activation-scoped post-drain linger (ADR-036)`.
  Document: on an `Empty` drain result, before the `Running → Idle` transition,
  the worker bound-spin-rechecks the *same activation's own* mailbox up to
  `EngineConfig::activation_linger_spin_limit` (default **32**) using the
  `DrainResult`-hoisted hand-off (no second dequeue call — this is load-bearing,
  see F-handoff); `exec_` is never written during the linger, so a racing
  producer's CAS fails cheaply and its message is picked up by the linger's
  own re-poll rather than paying a fresh Idle→Scheduled activation cycle.
- Record the measured numbers **with their regime qualifiers, not as
  unconditional claims**: under sustained 3-producer backlog, activations
  23.98%→1.56% and **+26.4%** throughput at the default (32); `post()` latency
  27–40% faster across all measured pacing regimes; under idle-ish/sparse
  traffic the mechanism is a near-no-op (93.10% vs. 92.31% baseline) — this is
  disclosed as an honest scope limit, not silently dropped.
- Add an explicit "rejected" note for `activation_linger_spin_limit=256` as a
  general default: it clears the activations/sends ratio target uniformly
  (0.23–0.48% in every regime) but regresses **−50.6%** throughput under the
  same sustained-backlog condition where 32 gains +26.4%, and trends negative
  under producer contention at every tested P. Cross-reference the same
  "prefer the smaller, tighter bound" discipline used in ADR-035's Design-1-
  over-Design-2 choice.
- State explicitly that `drain_step_governed_seq` (the `DrainBudget<N>`
  admission-bookkeeping drain variant) does **not** receive this mechanism —
  scoped out of this round entirely, matching `drain_step_reentrant`'s
  existing exclusion.
- Record the disclosed contention risk: the linger's re-poll structurally
  overlaps the producer's own `tail_` cache line more directly than the
  ADR-035 pre-park spin; measured throughput impact under contention (as
  opposed to backlog) is mixed, including one observed −14% regression run at
  P=2/limit=32 — flagged as an open item for the next round, not a masked
  defect.
- Do **not** add a section for Design B's cuts — record only a one-line
  "considered and rejected, ADR-036" pointer near the `wake_one()` discussion
  (if one exists) so a future author doesn't re-propose the same cache-line
  isolation without first reading why it was measured to have no effect on
  the real round-trip.

**`001-Actor-Execution-Model.md`**
- Add a "Reconfirmed a fifth time (ADR-036)" line to the Mailbox section's
  existing invariant-reconfirmation list (after the ADR-035 entry): the
  post-drain linger never writes `exec_` during its bounded spin — the
  Running state is held, not re-acquired — so the exec-state CAS contract,
  single-executor invariant (S1), and FIFO (C3) are unaffected by
  construction. Note the one implementation detail worth cross-referencing:
  the `DrainResult` hoist (F-handoff fix) means a linger-driven `Empty→
  message-found` transition flows the already-dequeued result straight into
  dispatch rather than issuing a second dequeue — future edits to
  `drain_step`/dispatch must preserve this single-dequeue property or
  reintroduce the drop/duplicate bug the fix closed.
- Add a short note next to the Lifecycle diagram (`Idle → Scheduled → Running
  → Idle`) that the linger is intentionally invisible at this diagram's level
  of abstraction: it delays *when* the `Running → Idle` transition is
  committed, it does not add a new state or change the transition set. This
  keeps `001` accurate without duplicating `002`'s mechanism-level detail.
- No change needed for `drain_step_governed_seq`/`DrainBudget<N>` beyond
  confirming (already true) that this ADR does not touch it.

## Tie-breaking / round-3 scoping experiment

Not needed to pick this round's winner between Design A and Design B — Design
A clears every safety/correctness claim and has real, proven throughput and
latency wins under the workload shape this debate exists to address (backlog/
contention), while Design B's own headline claim came back WRONG on the real
Engine. For the recommended follow-up round on the disclosed F1/F5 tension:

- Build an adaptive `activation_linger_spin_limit` (e.g., EWMA- or
  recent-inter-send-gap-gated, in the spirit of ADR-035's own round-2
  recommendation) that widens toward something like 256 under detected
  contended/bursty traffic and collapses toward 0 under detected sparse
  traffic, rather than shipping a single fixed number.
- Required proofs before such a hybrid can win a future round: an F1-
  equivalent (activations/sends ratio) that clears <5% in **both** sparse and
  contended regimes in the same run; an F2/F5-equivalent throughput result
  that does not reproduce the -50.6%-under-backlog or the −14%-under-
  contention regressions seen at the two fixed bounds tested this round; a
  cache-line-contention-specific probe isolating whether the adaptive
  mechanism's own state (EWMA counters, gap timers) can itself be made to
  avoid the `tail_`-line overlap red-team identified; and the same
  `QUARK_SCHED_BROKEN_WAKEUP`/S1–S3 gate-integrity rigor this round used at
  every boundary bound value.
- Verify `pal::cpu_relax()`'s ARM64 `YIELD` on real ARM64 hardware (compiled +
  run, TSan where available) — currently code-reviewed only.
- If a future change reduces `wake_one()`'s dominant cost (currently
  futex/condvar-bound, per Design B's own finding), re-run Design B's Cut 1
  isolated-vs-real comparison — the underlying false-sharing mechanism (4.45x,
  5/5 trials) was never disproven, only shown to not matter at the current
  bottleneck.
- All of the above on the Linux CI matrix with pinned cores
  (`taskset -c 0-3`), not a single dev-box run, before being treated as
  canonical, per `CLAUDE.md`'s machine-safety rules and `PERFORMANCE.md`'s
  own conventions.
