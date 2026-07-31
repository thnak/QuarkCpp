# ADR-036: Activation Idle-Churn Reduction (Post-Drain Linger vs. wake_one() Cost Cuts)

## Status

**Round 1 finding superseded by Round 3, Round 3's open contention-win question closed
(refuted) by Round 4 (see "## Round 3" / "## Round 4" below — historical content is kept
intact, not erased).** Round 1 accepted Design A's **flat, unconditional** linger
(`activation_linger_spin_limit` defaulted to **32**, spinning the full bound on *every*
empty-mailbox drain exit regardless of any evidence a message was actually imminent). After
merge-in-spirit, that flat default was found to **catastrophically regress** the project's
own 023 perf-gate bench targets (`activation_bench`, `sched_bench`) in the zero-concurrency/
single-core shape those benches exercise — 17x/12.7x throughput loss, ~6x p50 latency —
because Round 1's proof round never ran the actual repo bench-gate targets, only bespoke
synthetic backlog/sparse probes. **The flat default is retired.** Round 3 replaced it with
an **evidence-gated adaptive linger** (same knob name/semantics, but now scales the
*effective* spin bound by a per-activation, lane-only evidence counter that starts at 0, so a
mailbox with no concurrent producers pays the same ~zero cost as `spin_limit=0`). **The
adaptive design is Accepted and ships**, proven to close the regression to within ~1-3% of
the pre-ADR-036 baseline on the real bench-gate targets, with all Round-1 safety/correctness
invariants reconfirmed (ASan, MSVC ASAN, real TSan, full suite) against the new
implementation. Design B's two cache-line/memory-order cuts remain **not adopted** (Round 1
finding, untouched by Rounds 3/4). Round 1's F2/F5 sustained-backlog/contention throughput-win
numbers were left **inconclusive** under the adaptive design at the end of Round 3 (a harness
bug, not evidence either way). **Round 4 fixed that harness bug (plus a second, previously
unknown warm-up/process-order measurement bias it uncovered) and re-measured for real:
the contention/backlog win is REFUTED for the adaptive mechanism as shipped** — no
statistically distinguishable benefit over `spin_limit=0`, cross-validated under g++ and
clang++. Per that result, **`activation_linger_spin_limit`'s default is changed from 32 to
0** (mechanism byte-for-byte disabled by default, same code path as pre-ADR-036, remains
available and correct for opt-in use) — see "## Round 4" for the full harness fix, data, and
decision.

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

> **[SUPERSEDED IN PART BY ROUND 3]** Everything below in this section is the **Round 1**
> decision record, kept verbatim for history. The flat/unconditional spin shape it describes
> (spin the full configured bound on every `Empty` drain exit, no evidence gating) is
> **retired** — see "## Round 3" for why and for what replaced it. The F2/F5 throughput
> numbers quoted below (+26.4%, 23.98%→1.56%, the P∈{1,2,4} churn-reduction table) are
> **Round 1 measurements of the flat design only**. Round 3's attempt to reconfirm them
> against the shipped adaptive design was **inconclusive** (harness limitation, not a
> refutation) — do not read them as proven for the code that ships today. See Round 3's
> "Disposition of the unreconfirmed contention-win claim."

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

   **[ACTIONED IN ROUND 3 — see below, partially superseded.]** An adaptive
   bound was built, red-teamed, and shipped, but it was motivated by a *more
   severe* problem than this risk anticipated: not merely "no reduction under
   idle-ish traffic" but a **catastrophic bench-gate regression under true
   zero-concurrency** (17x/12.7x). The adaptive design fixes that. It does
   **not** fully resolve this risk's original framing (a uniform <5% target
   across both traffic shapes in one run) — see Round 3's own residual risks.
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

---

## Round 3 (2026-07-31): Flat-Linger Regression Discovery and the Adaptive-Evidence Redesign

This section is a **new, additive round** on top of Round 1 above (Round 2 — the
"scoping experiment" Round 1 recommended — was subsumed directly into this round's
work rather than run as a separate cycle). Round 1's content is kept verbatim above,
not deleted; superseded claims are marked inline where they appear. This section is the
current, authoritative state of ADR-036.

### Why this round happened

Round 1 shipped the flat, unconditional linger (`activation_linger_spin_limit=32`,
spins the full bound on every `Empty` drain exit, no gating). After it was merged in
spirit (branch `adr-036-activation-linger`, PR #6, still unmerged as of this round),
it was found to **catastrophically regress the project's own official 023 perf-gate
benches** — `activation_bench` and `sched_bench`, both strictly sequential, single-core,
one-message-per-cycle loops with **no concurrent producer**. Confirmed live, twice, via
git-worktree A/B on the real dev machine:

| Bench | Master (pre-ADR-036) | Flat-linger branch | Regression |
|---|---|---|---|
| `activation_bench` activate/deactivate cycle | 29.9–31.1 M cycles/s/core `[goal]` | 1.7–1.8 M cycles/s/core `[MISS]` | **17x worse** |
| `sched_bench` full-lifecycle throughput | 22.8 M msg/s/core `[goal]` | 1.8 M msg/s/core `[MISS]` | **12.7x worse** |
| p50 latency (both benches) | ~100ns | ~600ns | **6x worse** |

Root cause: an unconditional spin is pure waste in any shape where the mailbox is
*always* genuinely empty (no concurrency) — Round 1's proof round never exercised the
repo's actual bench-gate targets, only bespoke synthetic backlog/sparse probes, so this
was never caught before merge-in-spirit. Per `023-Performance-Targets-and-Budgets.md`'s
own stated tight-loop floor (≥20 M/s/core **Hard** floor for the peak enqueue/dequeue
shape) and its own gate rule ("a change fails the gate when a Hard budget is violated"),
1.8 M msg/s/core is not merely a Goal regression — it is a Hard-floor-class failure on
the project's own written policy, and a direct violation of this project's zero-cost
hot-path invariant (`CONVENTIONS.md`), not just a missed aspiration.

### The adaptive design (what ships)

`include/quark/core/activation.hpp`'s `linger_and_repoll`/`linger_bound` (replacing the
flat spin) add a per-activation, **lane-only** (no atomics — read/written only by the
drain-owning worker, same discipline as `linger_spin_limit_` itself), decayed evidence
counter:

- `linger_evidence_` : `std::uint8_t`, range `0..kLingerEvidenceMax` (4), starts at 0.
- `linger_bound()` computes the **effective** spin bound as a ceiling-division scaling of
  the configured `linger_spin_limit_` by `linger_evidence_ / kLingerEvidenceMax`, in
  64-bit arithmetic:
  `bound = ceil(linger_spin_limit_ * linger_evidence_ / kLingerEvidenceMax)`,
  and returns **0** outright when `linger_spin_limit_ == 0` or `linger_evidence_ == 0`.
  At evidence 0 (a fresh or genuinely-idle activation) this is **byte-for-byte the same
  effective bound as `spin_limit=0`** — the design's central claim, and the one the
  bench-gate numbers below confirm in practice.
- Evidence only ever **grows** from two observed signals, both real activity, never from
  a raw `Busy` mailbox status:
  1. `note_batch_evidence(dispatched_this_call)` — called at *every* `drain_step` exit
     (including the `Busy` exit), but gated on `dispatched_this_call >= kLingerBatchThreshold`
     (2); the bump scales with `std::bit_width` of the dispatched count (2–3 msgs → +1,
     4–7 → +2, 8+ → +3), capped below `kLingerEvidenceMax` per call.
  2. The linger's own re-poll finding a message (`kLingerHitBump = 2`).
- Evidence **decays** (`kLingerMissDecay = 2`) only when a linger spin runs its *full*
  bound and finds nothing.
- The setter clamps to `kLingerSpinLimitMax = 4096` (`n > max ? max : n`), closing the
  overflow risk red-team found at large configured limits.

### Red-team findings this round, and how each was closed

1. **SERIOUS — 32-bit overflow / monotonicity inversion** in the bound formula at large
   `linger_spin_limit_` values. **Closed** by moving the multiply to 64-bit and by the
   `kLingerSpinLimitMax=4096` setter clamp.
2. **SERIOUS — silent truncation-to-zero** for small `linger_spin_limit_` values,
   collapsing graduated evidence into a step function. **Closed** by ceiling-division
   arithmetic, which guarantees `spin_limit >= 1 && evidence >= 1 => bound >= 1`.
3. **SERIOUS, most substantive — "evidence hangover."** The pre-revision design also
   bumped evidence on a raw `Busy` mailbox status. Since `Busy` can repeat from ordinary,
   non-adversarial timing jitter (two producers briefly overlapping a poll —
   `Engine::run_activation` itself retries on `Busy` up to `busy_spin_limit`=64 times),
   just 1–2 ordinary races could saturate evidence to max and reproduce near the original
   catastrophic spin cost on the *next* genuinely idle transition — meaning the design's
   "fast" claims would only have held for zero-concurrency mailboxes, not the engine's
   normal use case (actors with real concurrent producers). **Closed structurally, not
   just bounded**: `Busy` was removed from the evidence-write path entirely. Verified by
   code inspection (confirmed independently by this judge reading `drain_step` and
   `linger_and_repoll`): the outer `drain_step` `Busy` exit only calls
   `note_batch_evidence(dispatched_this_call)`, which is a no-op whenever
   `dispatched_this_call == 0` — structurally the case on an immediate-`Busy` call before
   anything has been dispatched — and the inner `linger_and_repoll` loop's own `Busy`
   observation just keeps spinning without ever touching `linger_evidence_`. There is no
   remaining code path that writes evidence from a `Busy` result with zero real dispatch.
4. Flagged, not fully closed this round: **unmeasured/backwards bump-constant
   calibration** (`kLingerHitBump`, `kLingerMissDecay`, `kLingerBatchThreshold`,
   `kLingerBatchBumpMax`, `kLingerEvidenceMax` were chosen by design judgment, not swept)
   — carried to Round 3 residual risks below.
5. Disclosed, accepted as an explicit design trade-off: **isolated bursts after a long
   idle gap get zero linger benefit** (evidence has fully decayed) — same shape as Round
   1's F1 sparse-traffic disclosure, now a formalized boundary of the adaptive mechanism
   rather than a uniform-target miss.

### Evidence table (Round 3)

Same evidentiary bar as Round 1: only claims that survived red-teaming (after required
fixes) and were then run as real, compiled, executed evidence count.

| Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| Bound-formula arithmetic (no overflow, monotonic both directions, no zero-truncation) | correct | yes, after fixes #1/#2 | **CORRECT** | exhaustive table, `spin_limit ∈ {0,1,2,3,4,100,4096} × evidence ∈ {0..4}` |
| `activation_bench` regression fixed | fast (regression-fix) | yes | **CORRECT** | 29.53 / 29.54 / 29.65 M cycles/s/core `[goal]` (3 runs) — within ~1–3% of master's 29.9–31.1; flat-linger was 1.7–1.8 `[MISS]` |
| `sched_bench` regression fixed | fast (regression-fix) | yes | **CORRECT** | 22.1 / 21.9 / 22.2 M msg/s/core `[goal]` (3 runs) vs master's 22.8; flat-linger was 1.8 `[MISS]` |
| p50 latency parity (both benches) | fast | yes | **CORRECT** | 100.0ns on every run, both benches — matches master; flat-linger was ~600ns |
| Evidence-hangover eliminated (fix #3) | safe | yes | **CORRECT** | code-inspection-verified — no write path reaches `linger_evidence_` from a `Busy` result with `dispatched_this_call==0`; independently re-confirmed by this judge |
| Full correctness suite | correct | yes | **CORRECT** | 181/181 (Release, Windows, clang++, dev machine), incl. explicit `sched_no_lost_wakeup_test`/`_control`, `sched_work_stealing_test` |
| MSVC ASAN | safe | yes | **CORRECT** | 182/182 |
| Real TSan (WSL/g++14, `taskset -c 0-3`, `-j1` build, pinned run) | safe | yes | **CORRECT** | 166/166, clean, no races; one pre-existing, unrelated, already-known-class compiler warning noted during build, not a test failure |
| Footprint under 023 hard ceiling | correct | yes | **CORRECT** | `sizeof(Activation)` 640B (flat) → 704B (adaptive, one new field pushes past an alignment boundary); both under the 2048B/idle hard ceiling; `activation_bench`'s own footprint report confirms `[under ceiling]` |
| Sustained-backlog contention win (F2-equivalent) reconfirmed for the adaptive design | fast | attempted; harness did not exercise the target regime | **INCONCLUSIVE** — neither confirmed nor refuted | unmodified replay of Round 1's F2 3-producer harness (N=6M, limits {0,32,256}, 4 runs): activation-per-send ratio came out 0.05–0.18% across **all** configs (vs Round 1's original 23.98%→1.56% for limit 0→32) — two orders of magnitude lower, meaning the mailbox essentially never truly empties in this replay for any config (a large `drain_budget` lets one `drain_step` call absorb most of the backlog per call); throughput flat/noisy across limit=0/32/256 (5.3–5.8 M msg/s), no config consistently ahead |

### Decision (Round 3)

**The adaptive design ships as ADR-036's new default, replacing the flat design in
full.** Per this project's own ranking discipline:

- **Rule 4 (respect the invariants) is the deciding rule here.** The flat design's
  17x/12.7x regression on `activation_bench`/`sched_bench` — both zero-concurrency,
  single-core, sequential loops — is a direct violation of the zero-cost hot-path
  invariant this project treats as load-bearing (`CONVENTIONS.md`; `023`'s own Hard-floor
  gate language). A design that pays real, non-negligible cost on a mailbox that is
  *always* empty is not a "safe but slow" tradeoff; it is a broken invariant on the exact
  path (`Activation::drain_step`'s sequential hot path) this codebase's ADR history has
  iterated on more than any other. The flat design is therefore **disqualified as a
  default**, independent of anything it got right elsewhere.
- **Rule 2 (proven beats claimed) is cleanly satisfied for the fix itself.** The
  regression-fix claim was tested against the *correct* evidence this time — the actual
  repo bench-gate targets, not synthetic substitutes — and came back CORRECT with tight,
  repeated (3-run) numbers within 1–3% of the pre-ADR-036 baseline on both benches, plus
  matching p50 latency.
- **Rule 1 (safety is a gate) is fully cleared.** Every safety-relevant finding
  (arithmetic overflow/truncation, evidence-hangover) was closed with a real fix, not a
  workaround, and re-verified: 181/181 suite, 182/182 MSVC ASAN, 166/166 real TSan clean
  (including the two named lost-wakeup/work-stealing invariant tests). No `safe`/`correct`
  claim is WRONG for the adaptive design.
- **Rule 3 (then optimize fast) does not apply as a tiebreak here** — there is no other
  safe survivor to compare against. The flat design is disqualified by rule 4; the
  adaptive design is the only design this round that both stops the regression and
  passes every safety check.

**Footprint cost is accepted, not free, and should be disclosed:** `sizeof(Activation)`
grows 640B → 704B (10%) to carry `linger_evidence_` and its alignment padding — still
comfortably inside the 2048B/idle hard ceiling, but a real, non-zero cost that the spec
update below must state plainly rather than omit.

### Disposition of the unreconfirmed contention-win claim

**Chosen: option (c) — ship now, explicitly demote the Round 1 F2/F5 contention/backlog
numbers to "unverified under the adaptive design, carried forward from the flat design's
proof as a plausibility argument only, not a measured result."**

Justification, against this ADR's own stated methodology ("Only claims that survived
red-teaming... and were then run as real, compiled, sanitized/instrumented C++23 count
toward the decision"):

- **Why not (a) ship anyway and let the old numbers stand unqualified.** The mechanism
  changed materially between the two proofs: Round 1's F2/F5 numbers were measured
  against an **unconditional** spin_limit=32 that engages the full bound from message
  one, with no ramp-up. The shipped adaptive design starts every activation at
  evidence=0 (bound=0) and must earn evidence via `note_batch_evidence`'s
  `dispatched_this_call >= 2` gate or a linger hit before the spin engages at all — a
  materially different ramp-up profile under exactly the sustained-backlog shape F2
  measured. Restating Round 1's numbers as still-true for this different code would
  violate this ADR's own "proven beats claimed" bar; an inconclusive re-test is not a
  reconfirmation, and this project does not let unproven claims carry decision weight.
- **Why not (b) hold shipping until the F2 harness is fixed and rerun.** The regression
  this round exists to fix is **proven, severe, and currently live** on the unmerged
  branch (17x/12.7x on the project's own primary sequential-path benches) — a rule-4
  invariant violation with zero safe alternative on the table. The F2 harness's failure
  to exercise the target regime is a **test-harness bug** (an oversized `drain_budget`
  letting one `drain_step` call absorb the whole backlog), not evidence of a problem with
  the adaptive mechanism itself, and not a safety concern under rule 1. Blocking a proven,
  severe, safety-clean regression-fix on an unrelated and merely inconclusive (not
  refuted) upside claim inverts this project's own priority order (rule 1/4 gate before
  rule 3 optimizes) and leaves the catastrophic regression live on `PR #6`'s branch for
  no offsetting benefit.
- **Precedent within this very ADR.** Round 1 itself did exactly this shape of
  disposition for F1's "<5% everywhere" claim — shipped the mechanism, demoted the
  overclaimed uniform result to its actual, regime-qualified scope, and recorded the gap
  as a residual risk rather than either hiding it or blocking the ship. Round 3 applies
  the identical discipline to F2/F5.

**Practical effect on the spec:** any spec text stating F2's "+26.4% throughput" or F5's
churn-reduction table as a property of the *shipped* mechanism must be rewritten to
attribute those numbers explicitly to the retired flat design and labeled unverified for
the adaptive one — see spec recommendations below.

### Residual risks (Round 3 — appended; Round 1's numbered list above is unchanged)

7. **Round 1's F2/F5 sustained-backlog/contention throughput win is not reconfirmed for
   the adaptive design.** See disposition above. The next round must first fix the F2
   harness (cap or parameterize `drain_budget` so a single `drain_step` call cannot
   absorb an entire backlog wave, or otherwise force genuine empty-mailbox transitions
   between waves) and only then rerun the comparison before this claim can be restated as
   proven for the mechanism that actually ships. Until then, treat the contention-win
   story as a plausibility argument carried forward from different code, not a property
   of `include/quark/core/activation.hpp` as it exists today.
8. **Bump/decay/threshold constants are not independently calibrated or swept.**
   `kLingerHitBump=2`, `kLingerMissDecay=2`, `kLingerBatchThreshold=2`,
   `kLingerBatchBumpMax=3`, `kLingerEvidenceMax=4` were chosen by design judgment and
   validated only indirectly — via the bench-gate parity result (which mainly exercises
   the evidence=0 floor) and the arithmetic correctness table (which is regime-agnostic).
   No dedicated ablation swept these against a real contention/backlog regime. This
   should be folded into the same follow-up round as risk #7, ideally the same corrected
   harness exercising both the zero-concurrency floor and a genuine backlog regime in one
   run.
9. **Isolated post-idle-gap bursts get zero linger benefit, by design.** Evidence must be
   earned before the linger helps; a burst immediately after a long idle period pays the
   same (near-zero) cost as no linger at all for its first ~1–2 messages. This is the
   same trade-off shape as Round 1's F1 sparse-traffic disclosure, now a formalized,
   intentional boundary of the adaptive mechanism rather than an accidental uniform-target
   miss — disclosed here so it is not later mistaken for a bug.
10. **`Activation`'s footprint grew 640B → 704B (10%)**, still under the 2048B/idle hard
    ceiling but a real, permanent cost of the fix — should be stated in the spec, not
    silently absorbed.
11. **`pal::cpu_relax()`'s ARM64 `YIELD` remains unverified on real hardware** — Round 1
    residual risk #3, untouched this round; the proving environment (this round: Windows
    dev machine + WSL/g++14) remains x86-64-only.
12. **Round 3's evidence is single-machine.** The bench-gate numbers are from this
    session's real dev machine (the same machine that reproduced the original regression
    twice via git-worktree A/B); TSan is from WSL/g++14 on the same box. Not yet re-run on
    the Linux CI matrix — same caveat as Round 1's residual risk #6.

### Round 3 verdict summary

**The adaptive evidence-gated linger ships as ADR-036's default, fully replacing the flat
design.** The catastrophic bench-gate regression that made the flat design unshippable is
conclusively fixed and proven on the project's own primary sequential-path benches (within
1–3% of the pre-ADR-036 baseline, 3 runs each, matching p50 latency); every safety/
correctness claim red-team raised was closed with a real fix and re-verified under ASan,
MSVC ASAN, real TSan, and the full correctness suite. Round 1's sustained-backlog
throughput win is carried forward only as a disclosed, unverified plausibility argument,
not restated as a measured property of the shipped code — the next round's first job is
fixing the F2 harness and reconfirming (or refuting) it for real.

## Round 4 (2026-07-31): F2/F5 Harness Fix and Contention-Win Refutation

### Why this round happened

Round 3 shipped the adaptive design and closed the catastrophic bench-gate regression, but
left residual risk #7 open: the F2 (3-producer sustained-backlog) and F5 (P∈{1,2,4}
continuous-producer) harnesses, unmodified-replayed against the new adaptive mechanism, came
back inconclusive (0.05–0.18% activation/send ratio across every `linger_spin_limit`
config — two orders of magnitude below Round 1's original 23.98%/1.56% numbers). This round's
job, per residual risk #7's own instructions, was to fix that harness and get a real answer.

### Root cause of the harness bug

Both `run_backlog()` (F2) and `run()` (F5) construct `Engine<> eng(EngineConfig{1, 1, 1024,
64})`. `EngineConfig`'s fields are `{worker_count, shard_count, drain_budget,
busy_spin_limit}` — so `drain_budget=1024`. With 3-4 producer threads posting continuously at
full speed against a single worker, the backlog routinely exceeds 1024 queued messages, so
`Activation::drain_step` almost always returns `BudgetExhausted` (dispatch up to budget,
re-scheduled directly) rather than ever observing a genuine `Empty` mailbox. Since the
`activations` metric only increments on a real `Idle->Scheduled` edge, and the linger
mechanism only ever engages on the `Empty` exit path, a harness whose mailbox is
backlogged-by-construction for its entire duration can't exercise the linger's code path at
all, for any config — exactly the flat, indistinguishable 0.05–0.18% result Round 3 saw.

### The fix: wave/burst workload, not just a smaller budget

Shrinking `drain_budget` alone does not fix this — as long as producers saturate the
consumer continuously, the mailbox is backlogged by construction regardless of batch size.
The actual fix, per residual risk #7's own text ("force genuine empty-mailbox transitions
between waves"), restructures the workload: N producers, synchronized by `std::barrier`, send
a burst of `K` messages each, then all pause for a calibrated gap (busy-spun via
`pal::cpu_relax()` against `steady_clock`, not `sleep_for` — OS scheduling-quantum jitter
can't resolve microsecond-scale gaps) before the next wave. This models real bursty backlog
traffic — the regime the linger targets — instead of a workload that is saturated for its
entire duration by construction.

**Validated, not assumed**: the fixed harness was proven to actually exercise the target
regime before any comparison number was trusted from it, the same rigor this ADR has applied
throughout. Activation/send ratio rose from the old harness's 0.05–0.18% to **8–16%** at gap
≥ ~800ns and plateaued there — a two-orders-of-magnitude change confirming real per-wave
`Idle->Scheduled` transitions are now genuinely happening.

### A second bug found during proving: warm-up/process-order bias

The first pass at a "fixed" comparison (batching `limit ∈ {0, 32, 256}` calls inside one
process, one after another) showed a clean, cross-compiler-reproducible ~10-20%
activation-ratio reduction and throughput win for `limit=32/256` — indistinguishable in shape
from a reconfirmed Round-1 win. A reversed-run-order control and a same-limit-repeated-5x-
in-one-process control proved this was **entirely a first-call-in-process artifact**
(thread/Engine/pool construction overhead): whichever config happened to run *first* in a
process scored ~2 points worse on the activation ratio, independent of its actual
`linger_spin_limit` value. This is exactly the class of measurement error this ADR's own
"proven beats claimed" discipline exists to catch — a plausible, reproducible, and *wrong*
result. Fixed by adding a `single` mode: one discarded primer run, then one measured run,
always exactly one config per fresh process. Every number below is bias-controlled this way.

### Results (bias-controlled, gap=2000ns, burst=8/producer, waves=20000, n=8 trials/cell — 4 g++ 15.2.0 + 4 clang++ 20.1.8, WSL2 Ubuntu, `taskset -c 0-4` pinned, ≤5 threads)

| P | limit | activ/send % (mean±sd) | throughput msgs/s (mean±sd) |
|---|-------|------------------------|------------------------------|
| 1 | 0   | 15.48 ± 0.32 | 1,449,314 ± 49,486 |
| 1 | 32  | 15.68 ± 0.19 | 1,435,542 ± 70,288 |
| 1 | 256 | 15.47 ± 0.46 | 1,383,620 ± 127,431 |
| 2 | 0   | 10.43 ± 0.34 | 711,508 ± 160,786 |
| 2 | 32  | 10.31 ± 0.25 | 662,663 ± 77,105 |
| 2 | 256 | 10.17 ± 0.44 | 542,319 ± 128,437 |
| 3 (F2) | 0   | 8.71 ± 0.08 | 176,266 ± 21,592 |
| 3 (F2) | 32  | 8.44 ± 0.23 | 151,191 ± 31,278 |
| 3 (F2) | 256 | 8.58 ± 0.18 | 180,383 ± 15,883 |
| 4 | 0   | 8.44 ± 0.09 | 201,163 ± 19,679 |
| 4 | 32  | 8.47 ± 0.12 | 213,264 ± 26,876 |
| 4 | 256 | 8.44 ± 0.16 | 216,059 ± 18,618 |

Every `limit=0` vs `limit=32/256` gap sits within ~1 combined standard deviation, with no
consistent direction — `limit=0` is the *fastest* throughput at both P=1 and P=2. A gap=0
(back-to-back bursts, no idle window at all) sweep shows the same pattern. No config shows a
reproducible edge on either the activation-churn or throughput metric.

### Why the true effect is plausibly near-zero, not just noisy

`Engine::pre_park_spin` (ADR-035, `pre_park_spin_limit=256` default, unconditional and
independent of ADR-036) already spins on `any_work()` before a worker calls the OS-blocking
`park()`, and ADR-035 already measured this cutting OS-wake syscalls to 0.26-1.25% of sends.
The expensive part of an idle transition (a futex-class wait) was therefore already mostly
eliminated *before* ADR-036 shipped; the `activations` counter ADR-036 targets is comparatively
cheap (one CAS + one run-queue push) in the single-worker/single-shard topology F2/F5 use "to
isolate the linger's effect from work-stealing" — leaving little further win available for the
linger to extract in exactly the topology built to showcase it.

### Evidence table (Round 4)

| Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| Harness fix exercises genuine empty-mailbox transitions | correct (harness) | n/a (measurement fix, not a design claim) | **CORRECT** | activation/send ratio 0.05-0.18% (old, broken) → 8-16% (fixed, plateaued at gap≥800ns) |
| Warm-up/process-order bias identified and closed | correct (measurement) | n/a | **CORRECT** | reversed-order + same-limit-repeated-5x controls isolate a ~2-point first-call-in-process penalty, independent of `linger_spin_limit` |
| Round-1 contention/backlog win reconfirmed for the adaptive design | fast | attempted, bias-controlled | **REFUTED** | all `limit=0` vs `32/256` deltas within ~1 combined sd, P∈{1,2,3,4}, cross-validated g++15.2.0/clang++20.1.8, n=8/cell |

### Decision (Round 4)

**`activation_linger_spin_limit`'s default changes from 32 to 0.** Per this project's own
ranking discipline:

- **Rule 2 (proven beats claimed) is the deciding rule here.** The only justification for a
  non-zero default was the round-1 contention/backlog win. That claim is now proven false for
  the mechanism that actually ships, under a harness built specifically to exercise the regime
  it depends on and cross-validated across two compilers. Carrying a non-zero default forward
  on a refuted claim would itself violate this ADR's own evidentiary bar.
- **The cost is real even though the regression is fixed.** `sizeof(Activation)` already
  carries the unconditional +64B `linger_evidence_`/constants footprint regardless of this
  default (that cost is not recoverable by changing the default — the field exists whenever
  the mechanism is compiled in). But the *runtime* cost of a non-zero default — however small
  after Round 3's adaptive gating — is no longer offset by any proven benefit, so there is no
  remaining reason to pay it by default.
- **The mechanism is not removed.** It is correct, safety-clean (Round 3's full ASan/MSVC
  ASAN/TSan/suite reconfirmation stands unchanged — no code in `activation.hpp` changed this
  round, only the `EngineConfig` default), and available to any caller with a measured need
  that this round's topology and traffic shape didn't happen to cover. `0` reproduces the
  pre-ADR-036 `drain_step` byte-for-byte, so this default change carries zero regression risk
  on top of Round 3's already-verified bench-gate parity.

### Residual risks (Round 4 — appended; Rounds 1/3's numbered lists above are unchanged)

13. **Round 4's evidence is from WSL2 (a VM), not bare metal.** No native g++/clang++ was
    available on the Windows dev host for this round, so F2/F5 were rebuilt and run under
    WSL2 Ubuntu (g++ 15.2.0, clang++ 20.1.8) rather than natively. Sub-microsecond gap-timing
    questions (the 800ns validation threshold, the ~2000ns headline gap) are close enough to a
    VM scheduler's noise floor that one confirming run on bare-metal Linux (e.g. the CI
    matrix) would strengthen this result, though the refutation itself rests on relative
    (`limit=0` vs `32/256`) comparisons within the same environment, which is far more robust
    to absolute VM timing noise than any single absolute number would be.
14. **Residual risk #7 is now closed** (harness fixed, contention win measured and refuted for
    the adaptive design) but residual risk #8 (bump/decay/threshold constants not
    independently calibrated) is now moot for the *default* configuration — those constants
    only matter to a caller who opts back into a non-zero `linger_spin_limit`, which is no
    longer the shipped default.

### Round 4 verdict summary

**Residual risk #7 is closed.** The F2/F5 harness bug (a backlog workload that was saturated
by construction, plus a second warm-up/process-order measurement bias found while fixing it)
is fixed and validated to genuinely exercise empty-mailbox transitions. Re-measured against
the adaptive mechanism as shipped, cross-validated under two compilers with the measurement
bias controlled: **the round-1 contention/backlog throughput win does not reproduce** —
`activation_linger_spin_limit`'s default changes from 32 to 0 as a direct consequence, per
this ADR's own "proven beats claimed" bar. The mechanism itself is unchanged, remains fully
verified (Round 3's ASan/MSVC ASAN/TSan/suite evidence stands), and stays available for opt-in
use by any caller with a real, measured backlog-churn problem this round's benches didn't
happen to cover.
