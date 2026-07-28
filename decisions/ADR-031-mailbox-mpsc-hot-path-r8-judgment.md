# ADR-031: Mailbox MPSC Hot Path — Round 8 Judgment

## Status

Accepted

## Question

Design the actor Mailbox: the MPSC queue that owns FIFO message ordering,
storing fixed-size MessageHandles (never payloads). Many producer threads
(tell/ask from any worker) enqueue; exactly one worker drains it at a time,
guaranteed by the actor exec-state CAS (the single-executor invariant). The
design must be proven, with executed C++:

- **fast**: allocation-free on the steady-state hot path, scales under
  high producer contention
- **safe**: free of data races / UB / ABA under TSan + ASan + UBSan
- **correct**: strictly FIFO, no lost/duplicated handles, tombstones
  skipped exactly once

Ground rules that no design may violate: at most one executor per actor at
any instant; mailbox FIFO by default; scheduler schedules activations, not
messages; stable placement; workers are lanes not owners; zero-cost hot
path (no heap allocation, no reflection, no virtual dispatch for policy, no
dynamic resource resolution while a message is processed); C++23 std-only
core; OS specifics only behind the PAL.

This is round 8 of an ongoing design-debate series (ADR-001..004, ADR-020,
ADR-027, ADR-029 precede this one). Rounds 1–7 already settled on the
intrusive Vyukov mailbox as the shipped baseline; this round re-litigated
that decision against two fresh challengers to see if either could
dislodge it.

## Design summaries

1. **Intrusive Vyukov MPSC Mailbox — pooled-descriptor hot path (Quark
   core, as shipped).** The pooled `Descriptor` *is* the queue node
   (intrusive `MailNode` link at offset 0, pointer-interconvertible).
   Producers publish wait-free with one unconditional
   `tail_.exchange(acq_rel)` + one release link-store — no CAS, no retry
   loop, ABA-free by construction. The single consumer walks a
   consumer-private `head_` with plain loads; the one unavoidable RMW is
   confined to the empty-boundary stub re-arm. Vyukov's non-linearizable
   emptiness is surfaced as a first-class `Busy` result, never folded into
   `Empty`. Cross-worker `head_` handoff and the wakeup rendezvous both
   ride the actor's single exec-state atomic, split into two obligations: a
   plain release/acquire publish, and a seq_cst-fenced Dekker StoreLoad.
   Cancellation is a generation-gated single packed CAS on `gen_state`.
   This is exactly the code already at `include/quark/core/{descriptor.hpp,
   mailbox.hpp,exec_state.hpp}`, pushed to its strongest form, not a
   variant of it.

2. **SBR-v4 — Per-Mailbox Private Segmented Bounded-Ring MPSC
   (epoch-gated, freelist-recycled).** A Michael–Scott chain of
   fixed-capacity ring segments, made per-mailbox-private (never shared
   shard-wide) to structurally close ADR-029's disqualifying cross-mailbox
   corruption, with a packed `{segment-pointer:48, epoch:16}` word gating
   claims to close the same-mailbox ABA-of-reuse hazard. Cross-examination
   found two independent, executable-confirmed fatal bugs in the epoch
   mechanism itself (a fetch-before-check ordering that permanently orphans
   a slot on reincarnation, and a 16-bit epoch that wraps in seconds at
   realistic throughput — reproducing the exact ADR-004 undersized-counter
   anti-pattern), plus a non-compiling close-out (`seq_no` undefined, an
   rvalue passed where `compare_exchange_strong` needs an lvalue) and a
   black-boxed freelist recycling mechanism sitting on the same ABA hazard
   class that already broke a sibling design in ADR-027. The design
   conceded all three and proposed a structural rewrite (global monotonic
   ticket + full 64-bit `base_ticket` identity, bitmask-based segment
   recycling instead of a Treiber freelist). **The rewrite was never
   compiled or run** — no entry exists in the executed-evidence set for
   this design.

3. **REX-CAS/C — Treiber-Push / Uninterruptible-Batch-Reversal Mailbox
   (4th round of the REX lineage).** Producers push onto a Treiber LIFO
   with a single link-then-CAS (no partial-publish window). The consumer
   detaches the whole chain with one `head_.exchange(nullptr, acquire)` and
   reverses it once, uninterruptibly, into a private FIFO list before
   draining. The design is explicit up front that this lineage (REX:
   ADR-002/003; REX-BIR: ADR-020; REX-CAS/B: ADR-029) lost three
   consecutive rounds on the same structural ground — O(batch) reversal
   breaches the 015/023 50µs p999 tail-latency ceiling — and that no fix
   for the structural flaw is offered here; the design only argues its
   admission-ceiling number is now honestly quantified and its ABA/safety
   story is clean.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / verdict |
|---|---|---|---|---|
| F1 — zero cross-core RMW on interior drain | Vyukov (incumbent) | Yes | **CORRECT** | 0 lock/RMW instructions on interior branch, both compilers (objdump) |
| F2 — occupancy-1 latency ≤250ns p50 / ≤50µs p999 (revised, latency-only) | Vyukov | Yes | **CORRECT** | p50=60.0ns, p999=178.0ns (g++); p50=60.0ns, p999=129.0ns (clang++) |
| F3 — zero heap allocations on steady hot path | Vyukov | Yes | **CORRECT** | 0 allocations / 1M cycles, 4 build configs |
| F4 — sub-linear P-scaling caused by tail_ contention (revised, mechanism isolated) | Vyukov | Yes | **CORRECT** | Shared-tail_: P3/P1=0.73–0.88x; independent-cache-line control: P3/P1=2.5–2.6x |
| S1 — full protocol race/UB-free under TSan+ASan+UBSan (x86-64 scoped) | Vyukov | Yes | **CORRECT** | 18 clean compiles × 3 build modes, 0 sanitizer reports |
| S2 — fence-removal AND relaxed-CAS mutants both detectable | Vyukov | Split | **WRONG** (compound) | Fence-removal half CONFIRMED (lost=366k–618k unfenced vs. 0/50M fenced); relaxed-exec-state-CAS half FALSIFIED (0 TSan reports, test still passed, lost==0) |
| C1 — strict per-producer FIFO, no loss/dup, 400k messages | Vyukov | Yes | **CORRECT** | received=400000, dup=0, torn=0, fifo_violation=0 |
| C2 — tombstone reclaimed exactly once, claim-vs-cancel race safe | Vyukov | Yes | **CORRECT** | double_free=0 at 200k and 8M-iteration stress, both compilers |
| F1–S4/C1–C3 (rewritten protocol) | SBR-v4 | Yes (conceded + rewrote) | **NOT PROVEN — never compiled/run** | no executed-evidence entry exists |
| F1 — zero allocation | REX-CAS/C | Yes | **CORRECT** | 0 allocations at P∈{1,2,4,8,16,32} |
| F2 — loses to Vyukov at every P≥2 (self-predicted) | REX-CAS/C | Yes | **WRONG** | Rex *wins* at P=2,4 (55–60M vs 24–29M msg/s); Vyukov wins only at P≥8 |
| F3 — B* admission ceiling ≈500–2000 nodes, 2–5ns/node | REX-CAS/C | Yes | **WRONG** (numeric) | flat 2–10ns/node to B=262144 (no L2/L3 knee); empirical B*=2048(g++)/4096(clang); p999 breaches 50µs at B=4096 |
| S1 — no Busy transient, chain always well-formed | REX-CAS/C | Yes | **CORRECT** (with methodology caveat: prescribed negative control doesn't fire under TSan on atomics) | 0 TSan reports, full walkability, 3M×3 P-values |
| S2 — no ABA despite pool reuse | REX-CAS/C | Yes | **CORRECT** | dispatched=pushed=2M, dup=0, double_free=0 |
| S3 — single-executor invariant unaffected | REX-CAS/C | Yes | **CORRECT** | max_concurrent_drainers never >1, 4M cycles |
| S4 — Dekker fence load-bearing | REX-CAS/C | Yes | **CORRECT** | fenced lost=0/500k; unfenced lost=65,889–91,639/500k |
| C1 — strict FIFO / no loss / no dup (core invariant) | REX-CAS/C | Yes | **INCONCLUSIVE** | both prescribed tagging methodologies show reproducible, real inversions (2.9–7.3%); the check as specified is unsound and cannot certify the claim |
| C2 — tombstone exactly-once | REX-CAS/C | Yes | **CORRECT** | double_free=0, 8M messages, 40% cancel rate |

## Decision

**Winner: Design 1 — Intrusive Vyukov MPSC Mailbox (the shipped
`include/quark/core/{descriptor.hpp,mailbox.hpp,exec_state.hpp}`
lineage), unchanged.**

Applying the ranking in order:

**(1) Safety is a gate.** SBR-v4 never reached the Prove stage at all — no
compiled/executed artifact exists for its (conceded, rewritten) protocol,
so per "proven beats claimed," none of its claims count as proven and it
cannot win on evidence that doesn't exist. REX-CAS/C's own C1 — one of the
assignment's four ground-truth invariants, "mailbox ordering is FIFO by
default" — came back **INCONCLUSIVE**, not CORRECT: the executed harness
found real, reproducible cross-producer ordering inversions under both
tagging methodologies tried (2.9%–7.3% of 20M messages), and traced the
second ("corrected") methodology's inversions to a genuine memory-model
gap in the fix itself. Per the ranking rule, INCONCLUSIVE carries no
weight — REX-CAS/C therefore has **no proven claim for a core invariant
this task explicitly requires**, which disqualifies it from winning
regardless of its other passing safety claims or its F2 throughput wins at
low producer counts. (REX-CAS/C's own design summary already concedes it
"was never going to win" and is offered as an honest negative data point,
not a contender — the executed evidence bears that out.)

The Vyukov incumbent's one WRONG mark (S2) is examined and judged
non-disqualifying: it is a compound claim about two *mutant regression
controls*, not about the shipped design's actual behavior. The
fence-removal half — which corresponds to the real seq_cst StoreLoad fence
actually shipped in `Mailbox::producer_close_out_fence` /
`consumer_close_out_fence` — was CONFIRMED load-bearing (0 lost/50M fenced
vs. 366k–618k lost/50M unfenced): that fence stays in the design and stays
proven necessary. The half that was falsified is a claim that a *further*,
never-shipped downgrade (relaxing `ExecStateCell`'s own release/acquire
ordering to `relaxed`) would be independently caught by TSan; it was not,
on this workload — most plausibly because `run_queue.hpp`'s own,
untouched, Vyukov-style MPSC already transitively publishes the same data
via its own acq_rel exchange whenever an activation crosses threads. This
means one belt-and-suspenders ordering claim was over-stated in the
debate, not that the shipped code (proven race/UB-free under S1 across 18
clean build×test combinations, including a real 3-worker Engine run) has a
live defect. No core invariant is violated by this finding — see spec
recommendations below for how to close the gap in documentation rather
than in code.

**(2) Proven beats claimed.** Among designs with a live, race-free,
core-invariant-satisfying artifact, only the Vyukov incumbent has all four
assignment invariants (single executor, FIFO, no allocation, no
race/UB) proven CORRECT with executed evidence, not merely claimed or
partially proven.

**(3) Measured hot-path numbers.** The incumbent's numbers, all executed on
this host, both compilers: p50=60.0ns / p999=129–178ns occupancy-1
latency (far inside the 250ns/50µs hard budgets), 0 heap allocations over
1M cycles, 0 cross-core RMW on the interior drain path, and an honestly
characterized (not hidden) sub-linear multi-producer scaling curve whose
mechanism (shared `tail_` cache-line contention, not an OS confound) is
now isolated by a same-shape independent-cache-line control
(P3/P1=0.73–0.88x contended vs. 2.5–2.6x uncontended) — closing the "my
hypothesis, not yet the profiled cause" gap this same design's risk
register flagged going in.

**(4) Core invariants.** The incumbent bends none of them. Both
challengers either never got compiled (SBR-v4) or self-admittedly bend the
tail-latency invariant and failed to prove FIFO (REX-CAS/C).

No cheap fix is needed to un-disqualify anything, because nothing
disqualifying survived scrutiny against the actual shipped Vyukov design.

## Residual risks

- **ExecStateCell ordering redundancy is undemonstrated, not disproven.**
  The relaxed-CAS mutant passing clean under TSan on the real 3-worker
  `sched_no_lost_wakeup_test` workload suggests `ExecStateCell`'s own
  release/acquire may be redundant with `run_queue.hpp`'s independent
  Vyukov-style publish — but this was only shown for one workload/topology.
  Do not relax the ordering in the shipped code on the strength of this one
  negative result; the claim needs a dedicated, adversarial
  cross-thread-handoff litmus (not reliant on the run-queue's incidental
  publication) before any ordering is safely downgraded.
- **No ARM64 / weak-memory-model executed proof exists for any design in
  this round.** All fence/ordering reasoning (including the successful
  Dekker-fence controls) was executed only on x86-64 TSO hardware; the PAL
  seam (`quark::pal::store_load_barrier()`) is exercised in source but has
  no herd7/cppmem litmus or real aarch64 run backing it. This is a
  pre-existing, acknowledged gap (001, 019), not newly introduced.
- **P=4+ multi-producer scaling remains a real, now-explained-but-unfixed
  cost.** F4 confirms the mechanism (shared `tail_` line) but does not
  propose or measure a fix; any future round that wants to close the gap
  must budget for changing the producer-side contention point itself
  (e.g., sharded tails, per-producer batching), which is a materially
  different design, not a tuning knob on this one.
- **perf hardware counters were unavailable in the sandbox**
  (`perf_event_paranoid=4`, no `CAP_PERFMON`); F4's mechanism attribution
  rests on a same-shape independent-cache-line control rather than direct
  `mem_uop_retired.lock_loads` / cache-miss counters as originally
  specified. The control is sound but a follow-up with real hardware
  counters (or root/CAP_PERFMON) would strengthen the causal claim further.
- **SBR-v4's rewritten protocol is an open question, not a closed one.**
  Its structural fixes (global monotonic ticket + full-width identity,
  bitmask freelist) look sound on paper and were not re-attacked after the
  rewrite, but "never compiled" means literally nothing about it is
  proven. A future round should not re-litigate the already-disproven
  original epoch/Treiber-freelist mechanism, only the rewritten one, and
  it must actually be built this time.
- **REX-CAS/C's F2 result (winning at low P) is a genuinely new data
  point** that contradicts this lineage's own 3-for-3 prior losing record,
  attributable to Vyukov's `link_push` writing into the *previous*
  producer's node (cross-core ping-pong) versus Rex's push touching only
  its own thread-local node. This is worth remembering if a future round
  ever targets a workload dominated by P∈{2,4} producer counts specifically
  — but it does not rescue this round's REX-CAS/C, whose own core-FIFO
  claim (C1) came back inconclusive and whose tail-latency ceiling breach
  (F3) was confirmed at realistic backlog depths.

## Spec recommendations

- **`002-Scheduler.md`** — Close the "P=4 scaling gap — tracked, not
  closed" open item (currently lines ~100–103) with this round's finding:
  replace "still undiagnosed" with the isolated mechanism (shared `tail_`
  cache-line contention, confirmed via a same-shape independent-cache-line
  control showing near-linear P3/P1≈2.5–2.6x vs. contended
  P3/P1≈0.73–0.88x) and cite this ADR. Add the new occupancy-1 latency
  numbers (p50=60ns, p999=129–178ns) alongside the existing ADR-029
  baseline (p50 374–434ns) as a *host-dependent* data point — note
  explicitly that both are real measurements on different hardware/load
  conditions, so the spec should record the budget-compliance verdict
  (both pass) rather than a single canonical number.
- **`003-Memory.md`** — Add a short subsection documenting that the
  Dekker seq_cst fence in the exec-state close-out is proven load-bearing
  (ADR-031: 0/50M lost fenced vs. 366k–618k/50M unfenced) while the
  necessity of `ExecStateCell`'s own release/acquire ordering independent
  of `run_queue.hpp`'s transitive publication is *not yet* independently
  demonstrated — flag this as an open item for a dedicated litmus rather
  than let a future reader assume both orderings were proven equally
  necessary.
- **`001-Actor-Execution-Model.md`** — No invariant changes required; add
  a citation to ADR-031 alongside the existing ADR-029 citation confirming
  the single-executor invariant continues to hold (S3-equivalent result)
  under this round's fresh adversarial attempts, and note that both
  challenger designs in this round failed to dislodge the incumbent.
- **`015-Reentrancy-and-Quiescence.md`** — Strengthen the standing warning
  against the LIFO-push/batch-reversal lineage: this is now the *fourth*
  consecutive round (REX, REX-BIR, REX-CAS/B, REX-CAS/C) confirming the
  same O(batch) tail-latency ceiling breach (this round: p999 breaches 50µs
  at backlog depth as low as B=4096, non-resumable reversal cost flat at
  2–10ns/node with no cache-capacity knee up to B=262144). Also record that
  this round's harness uncovered a genuinely unsound cross-producer FIFO
  verification methodology (tag-before-CAS and tag-after-CAS both produce
  real, non-mailbox-bug inversions under contention) — any future design
  needing to prove cross-producer total ordering should be warned that a
  single shared monotonic counter sampled outside the actual publish
  operation is not by itself a valid ground truth, and a sound methodology
  should be worked out *before* the next round budgets time against it.

## Tie-breaking experiment (if ever revisited)

None needed to close this round — the evidence is decisive. If a future
round wants to seriously challenge the incumbent on throughput at P∈{2,4}
(the one regime where REX-CAS/C's Treiber push measurably won), the single
experiment worth running is: instrument Vyukov's `tail_.exchange` path
with real `perf` hardware counters (requires root/CAP_PERFMON, unavailable
in this sandbox) at P∈{2,4} to quantify exactly how much of the loss is
cross-core cache-line ping-pong on the shared `tail_` versus other
confounds, then decide whether a sharded-tail or per-producer-batching
variant of Vyukov is worth a fresh design round — rather than reviving the
LIFO-push/reversal lineage a fifth time, which 015 already treats as a
closed structural dead end absent a genuinely new O(1)-oldest-discovery
mechanism.
