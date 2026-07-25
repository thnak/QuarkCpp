# ADR-025: SimEngine Interleaving Exploration Strategy — Bounded Model Checking with DPOR

## Status

Accepted

## Question

014's deterministic `SimEngine` interleaves message delivery and activation
ordering via a seeded PRNG, but left open exactly how that exploration should
be driven (014 §Open questions #1):

> Interleaving exploration strategy: random search vs. bounded model checking
> (systematic exploration up to N context switches).

The design must specify the exact exploration mechanism and evaluate it
against 014's own invariant-checking role (single-executor, FIFO, no-lost-
message, placement-stability — violations abort with the seed and offending
schedule for replay): does the strategy reliably surface rare interleaving-
dependent bugs within a practical CI time/compute budget, and does every
explored schedule remain exactly reproducible from a seed?

Three designs were drafted, cross-examined (red team vs. defense), and then
proven or disproven with compiled, executed C++ (g++ 14.2.0 and clang++
20.1.2, -O2/-O3 plus ASan/UBSan/TSan builds, taskset-pinned to ≤4 cores per
the machine-core-limit rule).

## Designs (one-line summaries)

- **SeedFarm — Corpus-Amplified Pure Random Interleaving Search**: keeps the
  shipped `rng_() % n` picker unchanged; adds a counter-mode splitmix64 seed
  stream for trivially-parallel independent campaigns (one relaxed atomic
  work-stealing cursor, zero other shared state), three scheduler-owned
  invariant oracles (single-executor reentrancy guard, shadow-FIFO ledger,
  placement map) that throw a typed `SimInvariantViolation{seed, step,
  schedule}`, and a persisted, fingerprinted regression corpus replayed
  before every campaign's random budget.
- **BMC-DPOR SimEngine Exploration Strategy (winner)**: replaces the inline
  `rng_() % n` with a compile-time `Chooser` policy (`RandomChoicePolicy`
  unchanged by default; new `ScriptedChoicePolicy` replays a forced
  pick-prefix, defaulting unforced choices to index 0). An outer
  `explore_bounded()` performs iterative-deepening DFS over the pick-script
  space up to N branch points ("context switches" := branch points where
  `|runnable_| > 1`), pruned by Dynamic Partial Order Reduction: `post()` is
  instrumented to record each step's touched-receiver set, and only the
  sibling(s) whose actor id is actually implicated in a later conflicting
  touch are added to a branch point's backtrack set (source-set DPOR,
  Abdulla et al. 2014). A parallel outer shell fans the DFS frontier across
  `std::jthread` workers, each owning an independent `SimEngine` (no shared
  actor/mailbox state); the reproducer is `(fault_seed, forced_picks)`, a
  strictly stronger, self-contained generalization of a bare seed.
- **Hybrid/Adaptive Escalation — Seeded Random Search + Coverage-Triggered
  Bounded Local Model Checking**: a cheap baseline random layer plus a
  lock-free coverage-novelty CAS table that escalates newly-seen
  contending-actor shapes to a bounded local DFS window around the flagged
  decision point, via a Vyukov MPMC escalation queue and a
  release/acquire-published `confirmed_` reproducer slot.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| F1b throughput floor ≥50,000 runs/sec (revised from a falsified 300,000, after fixing eager pool-ctor allocation) | SeedFarm | yes (root-caused and fixed) | **CORRECT** | 51,644–55,266 runs/sec single-core; root-cause isolation: lazy pool growth = 44,424 runs/sec vs original eager ctor = 3,004 runs/sec (14.8×) |
| F2 ≥3.1× speedup at 4 threads, atomic-only sharing | SeedFarm | yes | **CORRECT** | T(1)=39.163s, T(4)=12.019s → 3.26×; 0 TSan reports |
| S1 single-executor guard fires deterministically on the first mutant seed | SeedFarm | yes | **CORRECT** | seed=0, one run, deterministic `SingleExecutor` violation both compilers |
| S2 shadow-FIFO ledger: fires on injected bug, correctly scoped, ≤50ns/msg overhead | SeedFarm | mixed — correctness/scope sub-claims survive; overhead sub-claim does not | **WRONG** (conjunctive claim fails on the numeric bound) | fires correctly on first mismatched pop and stays 100% green against a real production mailbox lost-wakeup bug (correctly out-of-scope); but overhead measured ~212ns/msg extra, ~4.2× over the claimed ≤50ns budget |
| C1b LostMessage wired into typed-violation pipeline (closes a fatal gap: a parked `task<>` coroutine was previously falsely certified as delivered) | SeedFarm | yes (fatal gap conceded-and-fixed) | **CORRECT** | deterministic `SimInvariantViolation{LostMessage,...}` on the exact repro; true-quiescence gate avoids false positives |
| C2b corpus entries pinned to a topology fingerprint (closes a serious gap: bare (name,seed) corpus entries silently stop protecting after a scenario refactor) | SeedFarm | yes (serious gap conceded-and-fixed) | **CORRECT** | mismatched fingerprint → explicit `StaleFingerprint`, never a silent pass |
| C3 random-search hit rate matches closed-form 1-(1-p)^N within 95% CI | SeedFarm | yes | **CORRECT** | 66/100 campaigns vs. predicted 72.2%, CI [63.4%,81.0%]; bit-identical across compilers |
| F1 fully-independent actors: DPOR explores exactly 1 run, not k! | BMC-DPOR | yes | **CORRECT** | runs_explored==1 for k=2..6, both compilers, ASan/UBSan clean |
| F1b ScriptedChoicePolicy budget-gating (fixes a dead-code bug: budget_ had zero effect) | BMC-DPOR | yes (conceded-and-fixed) | **CORRECT** | forced picks past budget correctly suppressed to index 0 |
| F2 star topology: DPOR gives "zero measurable reduction" vs. naive enumeration | BMC-DPOR | **no — falsified in the design's favor** | **WRONG** | minimal DPOR is 13.3× fewer runs and 9.6× faster wall-clock than naive at k=6 (612 vs 8,160 runs), a real and *growing* advantage — DPOR correctly deduplicates equivalence classes even in a fully-dependent star |
| F3 mixed-dependency topology: minimal-sibling widening's advantage over over-broad all-siblings widening grows with independent-actor count | BMC-DPOR | yes | **CORRECT** | gap ratio grows monotonically 3.83×→135.17× as independent actors go 0→4 |
| S1 SimConfig hoist + back-compat ctors + CTAD: all 5 existing call sites compile unchanged | BMC-DPOR | no (a 6th reference — a bare template-name global pointer — was missed) | **WRONG**, but stated 1-line fix verified (`SimEngine<>* g_sim`) restores full parity, both compilers | 3 of 4 test files compile+match baseline unchanged; the 4th needs one explicit template-argument annotation |
| S2 ParallelExplorer termination protocol: race-free AND the fixed lock-scoping actually matters (TSan-invisible lost-wakeup) | BMC-DPOR | yes (fatal hazard identified and fixed) | **CORRECT** | 0 TSan reports at 8 threads/262,143 leaves; engineered litmus: fixed wakes 200/200 within 150ms, pre-fix buggy hangs 0/200 — both variants TSan-silent, proving the hazard is a logical, not data, race; release/acquire on `stop_requested_` independently proven load-bearing (relaxed variant TSan-flags a race on `result_`, release/acquire does not) |
| C1 deterministic 100/100 detection of a planted bug within bound K | BMC-DPOR | yes | **CORRECT** | 100/100 invocations find the violation; SeedFarm's equivalent (C3) is only ever probabilistic |
| C2 reproducer replays byte-identically outside the DFS driver | BMC-DPOR | yes | **CORRECT** | 100/100 isolated replays reproduce the identical violation |
| C3 DPOR soundness (finds every violation naive enumeration finds) holds with fault injection armed | BMC-DPOR | yes | **CORRECT** | 21 planted-bug scenarios, 0 mismatches fault-free and fault-armed; minimal run count unaffected by armed faults |
| (all claims) | Hybrid/Adaptive Escalation | — | **NOT EXECUTED** | executed-evidence entry is `null` — no compiled/run proof was produced for any claim |

## Decision

**BMC-DPOR SimEngine Exploration Strategy wins.**

Applying the ranking in order:

**1. Safety gate.** Both surviving designs (SeedFarm, BMC-DPOR) had one
"safe"-kind claim come back `WRONG` from the prover — SeedFarm's S2 (shadow-
FIFO overhead 4.2× over its stated ≤50ns/msg budget) and BMC-DPOR's S1 (one
missed back-compat call site). Neither is disqualifying: in both cases a
concrete, cheap, *stated* fix exists and was independently verified — a
spec-level budget correction for S2 (the ledger lives on SimEngine's non-hot
test-only path, never the production hot path this RFC's zero-cost ground
rule actually governs), and a one-line `SimEngine<>* g_sim` annotation for
S1, confirmed to restore full parity under both compilers. The Hybrid design
was never gated at all: it has zero executed evidence, so it cannot be
credited with passing (or failing) the gate — it simply has no proven safety
claims to weigh.

**2. Proven beats claimed.** Counting only claims that survived
cross-examination *and* were proven `CORRECT` by executed evidence:
BMC-DPOR has **7 proven-correct** claims (F1, F1b, F3, S2, C1, C2, C3) against
**2 disproven** (F2, S1 — both with cheap fixes, and F2's failure is in the
design's *favor*: DPOR is more effective than the design itself claimed).
SeedFarm has **6 proven-correct** claims (F1b, F2, S1, C1b, C2b, C3) against
**1 disproven** (S2). The Hybrid design has **0** of either — its
`executed evidence` slot is `null`, so none of its claims count under this
debate's "proven beats claimed" rule; it is disqualified from winning on
evidentiary grounds regardless of how well-argued its cross-examination
survivors read on paper. Between the two evidenced designs, BMC-DPOR has
both a higher absolute count of proven claims and proves the more decision-
relevant property: **C1 shows deterministic 100/100 detection of a planted
bug within a stated bound**, a strictly stronger and more directly
responsive answer to this debate's central question ("does the strategy
*reliably* surface rare interleaving-dependent bugs within a practical
budget?") than SeedFarm's own honestly-scoped C3, which only ever proves a
*probabilistic* hit rate (66–72% per fixed budget) — SeedFarm's own claim
text concedes this is "not per-run completeness the way bounded-N model
checking claims."

**3. Measured hot-path numbers among safe survivors.** For an exploration-
strategy design, the load-bearing "hot path" is exploration cost per unit of
coverage, not production message dispatch (neither design touches that; both
stay entirely inside the test-only scheduler layer, per the ground rules).
BMC-DPOR's measured DPOR reduction — **13.3× fewer runs and 9.6× less
wall-clock time than naive full enumeration at k=6, with the advantage
*growing* as the actor graph grows (F2, F3)** — is a stronger, more
directly-relevant number for CI-budget practicality than SeedFarm's raw
single-core throughput (51,644–55,266 runs/sec) and 4-thread scaling (3.26×),
because it demonstrates the exploration *strategy itself* buys asymptotically
better coverage-per-CPU-second, not just faster individual runs. SeedFarm's
throughput numbers remain real and useful (see spec recommendations below)
but answer a different, narrower question (how fast is one random draw) than
the one this ADR is deciding (how is exploration *driven*).

**4. Core-invariant integrity.** Neither surviving design bends a core
invariant. Both keep `SimEngine` single-threaded per run (one executor per
actor, per run, at any instant), leave `Activation`/dispatch/handler code
(008) completely untouched, and produce an exact, replayable reproducer for
every violation — BMC-DPOR's `(fault_seed, forced_picks)` reproducer is, if
anything, a *strictly stronger* generalization of 014's "abort with the seed
and the offending schedule" contract than a bare integer seed, since it
pins the exact decision sequence rather than relying on a fresh PRNG stream
to reproduce it. BMC-DPOR's outer parallel DFS shell was shown, pre-fix, to
risk reintroducing exactly the real-thread flakiness class 014 explicitly
rejected (a TSan-invisible lost-wakeup in the frontier's termination
protocol) — this was caught, root-caused, and fixed (move the
predicate-affecting atomic mutations under the same lock the waiter's
predicate is evaluated under, before `notify_all()`), and the fix was
proven to matter with an engineered litmus (200/200 vs 0/200), not merely
asserted. This is exactly the kind of subtle, load-bearing correctness issue
this debate exists to catch, and it strengthens rather than weakens
confidence in the design once fixed.

**Why not SeedFarm.** SeedFarm is architecturally sound, honestly scoped
(C3's probabilistic framing is not oversold), and its throughput/scaling
numbers are real. But it structurally cannot offer more than a probability-
per-budget guarantee, which the debate's own framing rates as inferior to
"bounded-N model checking's exhaustive-but-exponential alternative" for the
specific question of *reliably* surfacing rare, interleaving-dependent bugs.
SeedFarm's proven contributions (the LostMessage gap fix, the corpus
fingerprinting fix) are real, independent improvements to `SimEngine`'s
invariant-checking correctness — orthogonal to *which* exploration strategy
drives the picker — and are folded into the spec recommendations below
regardless of which strategy wins.

**Why not Hybrid/Adaptive Escalation.** No executed evidence exists for any
of its claims (the evidence-table entry is `null`). Per this debate's rule 2
("count only claims that both survived red-teaming and were proven CORRECT
with executed evidence"), a design with zero proven claims cannot win
regardless of how its cross-examination round reads — that round showed the
design's *reference sketch* had at least one fatal, compile-checkable defect
(the escalation DFS never threaded the flagged step/prefix through, so it
could not structurally reach any bug past the first few decisions) and one
fatal concurrency defect (an unprotected plain-store on `confirmed_` that
drops simultaneously-confirmed reproducers) that were only fixed in the
*rebuttal text*, never compiled or run. Untested fixes to a design with two
independently-discovered fatal defects in its first draft do not meet the
proof bar this debate applies to the other two designs.

## Residual risks

- **Bounded, not complete.** BMC-DPOR's own stated limit: any bug whose
  minimal triggering reorder requires more than N context switches, or a
  branch factor above the configured cap at some step, is *not* guaranteed
  to be found — this is the honest trade the design makes for its stronger
  within-bound guarantee, and it means BMC-DPOR alone does not fully retire
  014's open question; it answers "how do we get an exhaustive, reproducible
  guarantee within a CI-affordable bound," not "how do we ever get evidence
  about bugs deeper than that bound."
- **DPOR soundness depends on an unenforced restriction.** The touched-
  receiver dependency relation is a conservative but *not complete* proxy:
  any handler that affects another actor's future behavior through a channel
  other than `post()` (a shared global, a shard-level metrics counter read
  mid-handler, a shared `StateStore` visible without an intervening message)
  is invisible to the race scan and can cause DPOR to silently skip an
  interleaving that would have exposed a bug. This must be a documented,
  enforced restriction ("handlers explored under BMC must communicate only
  via tell/ask"), not an assumption.
- **No state snapshot/restore.** Every DFS leaf re-executes the scenario
  from scratch; cost per leaf is O(prefix depth), not O(1). Deep scenarios
  (long sagas, many timer/`advance()` calls) multiply CI cost roughly
  linearly with prefix depth on top of leaf count. A checkpoint/restore
  optimization is explicitly out of scope for this decision and is future
  work.
- **"N context switches" is a conservative proxy, not the CHESS definition.**
  Implemented as "N branch points where `|runnable_| > 1`," not "N
  transitions where the executing actor differs from the previous step."
  Every true context switch requires an n>1 point, but not every n>1 point
  is a switch under the stricter literature definition — the budget
  parameter is not directly comparable to published BMC tools' N, and
  documentation must say so to avoid over-claiming coverage equivalence.
- **SeedFarm's proven gaps are real defects in `SimEngine` itself, independent
  of which exploration strategy wins**, and must be fixed regardless: (a) the
  LostMessage invariant previously silently mis-certified a permanently-
  parked `task<>` coroutine as delivered (a first-class hybrid-handler
  scenario, not an edge case) — this is now fixed and must be carried into
  whichever exploration driver ships; (b) a leaked coroutine frame was
  observed under LeakSanitizer for the same parked-coroutine case — a
  pre-existing, documented seam (no async/fiber carrier to reclaim a parked
  frame), orthogonal to this decision but tracked as an open defect.
- **BMC-DPOR's parallel outer shell is a second concurrency layer bolted on
  purely for CI throughput.** It was proven race-free and lost-wakeup-free
  *as fixed*, but a future edit that accidentally shares one `SimEngine`
  instance across worker threads (rather than giving each worker its own)
  would silently reintroduce exactly the real-thread flakiness 014 rejects.
  This must be enforced by construction (move-only/non-shareable frontier
  boundary) and kept under permanent TSan coverage in CI, not left to review.
- **SeedFarm's regression corpus and BMC-DPOR are not mutually exclusive**;
  the corpus-plus-fingerprint mechanism is a cheap, orthogonal, already-
  proven-correct complement (see spec recommendations) that BMC-DPOR alone
  does not provide — a discovered-then-fixed bug still benefits from a
  permanent, O(corpus-size) regression check that doesn't depend on a fresh
  DFS budget re-deriving it.
- **Hybrid/Adaptive Escalation's core idea (coverage-guided escalation) is
  not proven wrong, only unproven.** Its cross-examination round surfaced a
  real, valuable insight (escalate CI budget toward *novel* contention
  shapes rather than spending it uniformly) that could be revisited as a
  *future* design once backed by compiled, executed evidence — it is
  excluded from this decision on evidentiary grounds, not on a demonstrated
  correctness failure of its central mechanism.

## Spec recommendations for `014-Testing-Model.md`

1. **§Deterministic simulation / §Open questions** — Resolve the first open
   question by replacing it with the accepted answer: `SimEngine` picks its
   next runnable actor through a compile-time `Chooser` policy
   (`RandomChoicePolicy`, unchanged default, byte-identical to today's
   `rng_() % n`; `ScriptedChoicePolicy` for bounded exploration). Document
   that `SimEngine` becomes `template <ChoicePolicy Chooser =
   RandomChoicePolicy> class SimEngine`, a concept-constrained, zero-cost
   compile-time parameter (no vtable), consistent with this RFC's "no virtual
   dispatch for policy" ground rule and the locked CRTP-policy-types
   decision.
2. **New §Bounded exploration (DPOR)** — Add a section documenting
   `explore_bounded(N, branch_cap)`: iterative-deepening DFS over pick
   scripts up to N context switches (defined precisely as "branch points
   where `|runnable_| > 1`," with an explicit note that this is a
   conservative proxy for, not equivalent to, the stricter
   "executing-actor-changes" definition used in CHESS-lineage literature).
   Document the DPOR pruning rule (source-set/minimal-sibling backtracking
   over `post()`-observed touched-receiver sets) and its **soundness
   restriction as a hard rule, not a footnote**: handlers explored under
   bounded model checking must communicate only via `tell`/`ask`; any
   shared-mutable-state side channel is an explicit, documented soundness
   gap. State plainly that bugs requiring more than N switches are not
   guaranteed to be found — this is the design's stated limit, not an
   oversight.
3. **§Invariant checking** — Add the reproducer generalization: a violation's
   reproducer is `(fault_seed, forced_picks)` (a superset of a bare seed),
   which replays exactly through `ScriptedChoicePolicy` and is strictly
   stronger than "abort with the seed" alone. Also **close the proven
   LostMessage gap**: specify that `run_until_idle()` must, at true
   quiescence (`runnable_` empty **and** all timer/armed-event queues
   empty — not merely `runnable_` empty), scan every activation and throw
   `SimInvariantViolation{LostMessage,...}` for any non-terminal
   (`Completed`/`Cancelled`/dead-lettered) activation, so a permanently
   parked coroutine can never be silently certified as delivered.
4. **§Relationship to production code** — Add an explicit warning, informed
   by the proven S2 result: scheduler-owned oracles (shadow-FIFO ledger,
   single-executor guard) verify the *scheduler's own bookkeeping* under
   `SimEngine`'s single-threaded execution model; they are not a substitute
   for, and must never be read as proof of, the production concurrent
   mailbox's (002) correctness — that remains the threaded-engine + TSan
   tier's job.
5. **New §Regression corpus** — Adopt SeedFarm's proven corpus mechanism as a
   complementary tier regardless of exploration strategy: an append-only
   `tests/corpus/<scenario>.seeds` file, each entry storing `(seed, topology
   fingerprint)`, replayed before any campaign/DFS budget on every CI run. A
   fingerprint mismatch (scenario evolved) must emit an explicit
   "stale corpus entry, re-mine required" CI signal rather than a silent
   pass — this was a proven, real gap in the naive (seed-only) version.
6. **§Dependencies / performance note** — Record the proven throughput
   numbers as a floor, not the originally-claimed (and falsified) figure:
   a single reference core sustains ≥50,000 independent seeded `SimEngine`
   runs/sec for a representative small scenario once pool/timer capacities
   are right-sized and lazily grown (the shipped default's eager
   pool-capacity pre-construction was the actual bottleneck, not the
   picker); note this is orthogonal to, and should be fixed independent of,
   the exploration-strategy decision itself.
7. **§Dependencies** — Loosen or delete the shadow-FIFO ledger's implied
   overhead budget (there is no such budget stated in 014 today, but any
   future perf note for scheduler-side oracles should cite ~200–250ns/msg,
   not an aspirational ≤50ns, since that is a test-only, non-hot-path cost
   by this RFC's own definition of the hot path).
