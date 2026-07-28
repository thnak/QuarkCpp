# ADR-032: Mailbox MPSC Hot Path — Round 9 Judgment

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

This is round 9 of an ongoing design-debate series (ADR-001..004, ADR-020,
ADR-027, ADR-029, ADR-031 precede this one). Rounds 1–8 already settled on
the intrusive Vyukov mailbox as the shipped baseline, most recently
defending it in ADR-031 against a segmented-ring design that never
compiled and a Treiber-push design that came back INCONCLUSIVE on FIFO.
This round re-litigated the decision against two fresh challengers,
purpose-built to attack the specific gap 015/ADR-031 named (bounding
oldest-message-discovery cost) — and, for the first time, ran every
design's claims through actual compiled, sanitized, executed C++.

## Design summaries

1. **Incumbent intrusive Vyukov MPSC Mailbox — pooled-descriptor hot path
   (Quark core, as shipped).** The pooled `Descriptor` *is* the queue node
   (intrusive `MailNode` link at offset 0, pointer-interconvertible).
   Producers publish wait-free with one unconditional
   `tail_.exchange(acq_rel)` + one release link-store — no CAS, no retry
   loop, ABA-free by construction. The single consumer walks a
   consumer-private `head_` with plain loads; the one unavoidable RMW is
   confined to the empty-boundary stub re-arm. Vyukov's non-linearizable
   emptiness is surfaced as a first-class `Busy` result, never folded into
   `Empty`. Cross-worker `head_` handoff and the wakeup rendezvous both
   ride the actor's exec-state atomic: a plain release/acquire publish,
   plus a seq_cst-fenced Dekker StoreLoad on close-out. Cancellation is a
   generation-gated single packed CAS on `gen_state`. This is exactly the
   code at `include/quark/core/{descriptor.hpp,mailbox.hpp,exec_state.hpp}`.

2. **SBR-v5 — Resident Sequence-Numbered Ring + Proven-Mailbox Overflow
   Valve.** A per-mailbox bounded Vyukov-style sequence-numbered ring
   (N=64, dense 16-byte cells) is the steady-state hot path; enqueue is one
   `fetch_add` into a ticket space mod-N, drain is a monotonic prefetchable
   sweep. When the ring saturates, a one-shot `sealed_at_` boundary
   permanently diverts that ticket and all later ones to an embedded,
   *unmodified* instance of the incumbent's own Vyukov mailbox as overflow
   — deliberately avoiding SBR-v4's fatal epoch/freelist-recycling defect
   (ADR-031) by introducing no second novel reclamation mechanism.
   Cross-examination found the original producer-raced seal CAS could
   permanently deadlock the mailbox (a smaller ticket losing the seal race
   to a larger one); the design was revised mid-round to make sealing a
   single-writer consumer-side decision, closing that specific bug.

3. **SEG-REX — Segmented Treiber-Push / Bounded-Batch-Reversal Mailbox.**
   Producers CAS-push onto a small, compile-time-bounded (`SEG_CAP=256`)
   Treiber-stack "Segment"; once full, the sealing producer swaps in a
   fresh segment and links the full one into a Vyukov-style FIFO of
   segments (reusing 002's proven Busy/Dekker protocol at segment
   granularity), so the consumer never reverses more than one bounded
   segment to find the oldest message — a direct, honest attempt at the
   O(1)-ish oldest-discovery mechanism 015/ADR-031 say this whole lineage
   (REX/REX-BIR/REX-CAS/B/REX-CAS/C, four straight losing rounds) has
   lacked. ABA closure was originally a 128-bit stamped-pointer DWCAS;
   cross-examination found this both fails to compile as a lock-free op on
   g++ and does not actually close cross-generation ABA on segment-pool
   recycling, forcing a mid-round revision to a hazard-pointer-style
   announce/re-validate/quiesce protocol (S4) instead.

## Evidence table

Only claims that survived cross-examination and were then run as real,
compiled, sanitized C++ are listed. "Survived" = red-team round left it
standing (possibly revised). "Proven" = prover's executed verdict.

| Design | Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|---|
| Incumbent | F1 zero cross-core RMW on steady-state dequeue | fast | yes | **CORRECT** | asm gate: 1 bare `xchg`, 0 `cmpxchg` in try_dequeue on gcc/clang ×2 opt levels; pop cost 8.99–10.74 ns/op, within noise of a zero-atomic baseline (8.92–9.20 ns/op) |
| Incumbent | F2 enqueue = 1 xchg + 2 stores, no alloc | fast | yes | **CORRECT** | asm gate: 1 xchg, 0 cmpxchg, 0 calls; noalloc test: 0 allocations / 1e6 cycles |
| Incumbent | F3 0 TSan/ASan/UBSan reports, P≤4, ≥400k msgs | safe | yes | **CORRECT** | 0 reports across mailbox_mpsc_test + mailbox_cancel_test, gcc+clang, TSan and ASan+UBSan |
| Incumbent | F3b cancel racing concurrent release/recycle | safe | yes (new, added on rebuttal) | **CORRECT** | 1,000,000 iterations, 0 UAF/double-free, 0 TSan/ASan reports, both compilers |
| Incumbent | F4-revised: ExecStateCell ordering is independently load-bearing (not redundant given run_queue.hpp) | safe | yes (replaces conceded F4) | **CORRECT** | isolated 2-thread litmus (no run_queue.hpp hop): acquire/release clean (0/5M, both compilers); relaxed mutant fires a real TSan race on both compilers — question 003-Memory.md left open is now closed |
| Incumbent | F5 seq_cst Dekker fence prevents lost wakeup | safe | yes | **CORRECT** | 50M trials: fenced lost=0/50M both compilers; no-fence control lost=441,292 (gcc) / 576,097 (clang) per 50M |
| Incumbent | C1 strict per-producer FIFO, dup=0, lost=0 | correct | yes | **CORRECT** | 400,000 msgs (4×100k), both compilers, plain+TSan+ASan/UBSan: dup=0 missing=0 torn=0 fifo_violation=0 in every run |
| Incumbent | C2 cancel-vs-claim resolves to exactly one outcome, no double-free | correct | yes | **CORRECT** | 200,000-race block, both compilers, TSan+ASan/UBSan: double_free=0, tombstoned==cancel_won every run |
| Incumbent | C2b 48-bit generation wraps correctly at boundary | correct | yes (new) | **CORRECT** | seeded at gen_max, release()→generation()==0; stale gen_max handle safely rejected post-wrap, both compilers |
| Incumbent | C3 single-executor invariant never violated | correct | yes | **CORRECT** | max_concurrent_drainers=1 over 100k–300k cycles, 3 workers, both compilers |
| SBR-v5 | F1v2 steady-state enqueue = 1 RMW + 2 acquire loads + 2 stores, no CAS | fast | yes (corrected wording) | **CORRECT** | objdump confirms exactly 2 acquire loads + 1 RMW, vs incumbent's 0 loads + 1 RMW |
| SBR-v5 | F2 drain-only cache locality beats incumbent | fast | yes (narrowed to drain-only) | **INCONCLUSIVE** | perf L1-dcache counters unavailable in sandbox (no CAP_PERFMON); wall-clock proxy only (not the specified metric) |
| SBR-v5 | F3 N=64 never engages overflow under declared steady-state load | fast | yes | **WRONG** | ever_sealed=1 (100%) at P∈{1,2,4,8}; even paced P=4 at 800k msg/s (a fraction of peak) seals within 1000–3000 tickets; once sealed, ring is *slower* than incumbent alone (8.1M vs 13.5M ops/s at P=4) |
| SBR-v5 | S1 ring is ABA-free by construction | safe | yes | **CORRECT** | TSan/ASan clean P∈{1,2,4,8,16}; naive address-compare positive control independently fails with a real race |
| SBR-v5 | S2 seq/desc release-acquire handoff race-free | safe | yes | **CORRECT** | clean both compilers/sanitizers; relaxed-seq mutant fails with a real race on `desc` |
| SBR-v5 | S3v2 (revised single-writer seal) overflow engagement safe | safe | yes (revised mechanism) | **CORRECT** | 1000+ episodes, P∈{4,8}, TSan+ASan clean, 0 double-free/leak/deadlock after 2 harness bugs fixed |
| SBR-v5 | C1v2 (revised) strict global-ticket dispatch order | correct | conceded-then-revised | **WRONG** | 5/5 repeats, P∈{2,4,8,16,32}: 58,284–858,983 ticket-order inversions once overflow engages with ≥2 concurrent producers; per-producer FIFO and completeness (0 gaps/dups) hold, but the design's own stated global-order claim does not |
| SBR-v5 | C2 paused-producer Busy/Empty correctness | correct | yes | **CORRECT** | 300+ trials, TSan+ASan clean, both compilers |
| SBR-v5 | C3v2 ring/overflow boundary source-routing + monotonicity | correct | revised | **WRONG** | source-routing sub-property holds (0 violations) but overall monotonicity fails: 3,917–858,983 out-of-order deliveries once overflow concurrently engaged |
| SBR-v5 | C4 composite wakeup predicate, 0 lost wakeups | correct | yes (new) | **CORRECT** | 100+ rounds, ring-only and ring→overflow regimes, 0 lost wakeups |
| SEG-REX | F1 zero heap alloc on hot path | fast | yes | **CORRECT (but unreliable)** | 0 allocations in every run that *completed*; most runs at even tiny scale hang before completing (gated by S2/S4 defect) |
| SEG-REX | F2 P=2/P=4 throughput beats Vyukov | fast | yes | **WRONG** | SEG-REX loses to incumbent by 3–4× at *every* P on *both* compilers (e.g. gcc P=4: 5.77M vs 20.20M msg/s) and degrades further as P grows |
| SEG-REX | F3 SEG_CAP=256 bounds p999 reversal latency independent of backlog depth | fast | yes | **CORRECT (narrow)** | p999 stayed 1.3–17.9µs across B∈{4096,16384,262144}; SEG_CAP sweep confirms SEG_CAP=4096 breaches the 50µs budget while 256 stays under it — but only the reversal-walk component was isolated; quiesce-wait component left INCONCLUSIVE |
| SEG-REX | S2 segment-seal coordination race-free | safe | yes | **WRONG** | reproducible `inflight` unsigned underflow / permanent deadlock at both tiny (6-segment) and generously-sized pools; sanitizer-invisible (logical/ABA bug, not a data race) |
| SEG-REX | S4 (revised, DWCAS-free) hazard-pointer-style ABA closure | safe | yes (replaces conceded S1) | **WRONG** | same failure as S2; attempted repair reduced but did not eliminate hangs — a second distinct deadlock mode persists; true unbounded-preemption hazard requires real RCU/hazard-pointer deferred reclamation, not implemented |
| SEG-REX | S3 single-executor invariant untouched | safe | yes | **CORRECT** | max_concurrent_drainers=1 over 4M cycles, both compilers, clean under TSan |
| SEG-REX | C1 per-producer FIFO, 0 dup | correct | yes | **CORRECT (narrow)** | holds in every run that completes; but completion itself is gated by the S2/S4 defect, so most runs at scale never reach a checkable terminal state |
| SEG-REX | C2 cancel skipped exactly once, no double-free | correct | yes | **CORRECT (narrow)** | holds where runs complete; TSan runs consistently hung (same S2/S4 race window widened) rather than reaching a verdict |

## Decision

**Winner: the incumbent intrusive Vyukov-style MPSC mailbox, exactly as
shipped in `include/quark/core/{mailbox.hpp,descriptor.hpp,exec_state.hpp,
activation.hpp}`.**

Both challengers are disqualified by the safety/correctness gate before
their numbers are even relevant:

- **SBR-v5** is disqualified on its own `correct`-kind claims: C1v2 and
  C3v2 (the design's own stated properties — that dispatch preserves
  global ticket order across the ring/overflow seam, and that the merged
  sequence is monotonic at the boundary) were both proven **WRONG**, with
  up to 858,983 out-of-order deliveries per run once ≥2 producers
  concurrently engage the overflow path. There is no stated cheap fix —
  the root cause is structural: the overflow path re-orders by the
  incumbent mailbox's own real-time enqueue-race order, not by the ticket
  value assigned before diversion, and fixing that requires redesigning
  the ring→overflow handoff protocol itself, not a parameter tweak. On top
  of that, F3 (the central premise that N=64 suffices for declared
  steady-state load) was also proven WRONG at a strikingly low bar — the
  overflow valve engages at a small fraction of peak throughput even under
  paced traffic, and once engaged the ring is measurably *slower* than the
  incumbent alone (8.1M vs 13.5M ops/s at P=4), meaning that in the exact
  regime the design targets, it is a net regression, not an improvement.
- **SEG-REX** is disqualified on `safe`-kind claims: S2 and S4 (the
  segment-seal/reclamation protocol's own race-freedom) were both proven
  **WRONG** — a reproducible unsigned-underflow-then-deadlock, confirmed
  sanitizer-invisible (a logical/ABA bug TSan/ASan cannot see), that
  persisted even after an in-round attempted repair. The prover states
  explicitly that a real fix requires true hazard-pointer/RCU-style
  deferred reclamation, which was not implemented in this round — that is
  not a "cheap fix," it is an open research/engineering item. Its `fast`
  claim (F2, the entire performance rationale for taking on this
  complexity) was also proven WRONG: SEG-REX loses to the incumbent by
  3–4× at every producer count on both compilers, the opposite of its
  central bet.

With both challengers gated out, the incumbent wins by rule 2
(proven-beats-claimed) trivially — it is the only design with **zero**
WRONG claims among F1, F2, F3, F3b, F4-revised, F5, C1, C2, C2b, C3, all
independently reproduced by the prover on both GCC 14.2 and Clang 20.1,
under TSan and ASan+UBSan, with executed positive controls confirming
harness sensitivity wherever a control was meaningful. It also clears
rule 4 (no bent core invariant): F4-revised additionally *closed* a
previously open question in this project's own history (003-Memory.md /
ADR-031's note that ExecStateCell's acquire/release ordering was not
independently demonstrated necessary) — the isolated litmus proved it
*is* load-bearing on its own, not merely redundant with run_queue.hpp's
incidental republication.

Measured hot-path numbers for the (only) surviving design, for the
record: steady-state dequeue 8.99–10.74 ns/op (gcc/clang), enqueue 1 xchg
+ 2 stores with 0 allocation, and MPSC aggregate enqueue throughput at
P=1→4 degrading ~24–29% (28.97→21.93 Mops/s gcc, 24.81→17.62 Mops/s
clang) due to the single shared `tail_` cache line — a known, disclosed
limitation, not a regression introduced this round.

## Residual risks

1. **ARM64/weak-memory proof gap remains open.** All executed evidence
   this round (and prior rounds) is x86_64-only. The seq_cst Dekker
   fence, the acq_rel `tail_.exchange`, and ExecStateCell's ordering are
   proven load-bearing on x86-TSO; none of this has been exercised on a
   weak-memory target. 019's `store_load_barrier()` PAL seam exists but is
   still not exercised by an ARM64 CI leg.
2. **Multi-producer scaling ceiling is real and unaddressed.** The
   incumbent's own numbers this round (P=1→4: ~24–29% aggregate-throughput
   degradation) confirm ADR-031's finding that the single contended
   `tail_` line caps scaling under high fan-in. Neither challenger fixed
   this cleanly: SBR-v5's ring wins on paper only in a regime (N=64,
   modest burst) that this round's own F3 result shows is narrower than
   assumed; SEG-REX's segmented approach measurably made per-push
   throughput *worse*, not better, once the seal/pool-mutex and
   inflight/size RMW overhead is counted.
3. **`F3b`'s new coverage is real but still narrow.** The cancel-racing-
   concurrent-recycle test uses a 16-descriptor pool and one canceller
   thread; it has not been run at the full P≤4 concurrent-producer scale
   combined with concurrent recycling, nor beyond 1M iterations.
4. **The generation-horizon argument (48-bit, ~50M msg/s reference) is
   still asserted, not re-derived, for arbitrary deployment configs** —
   C2b only closes the *wrap-boundary correctness* gap (does wraparound
   itself behave safely), not the *horizon-sizing* gap (is 2^48 actually
   far enough away at some given deployment's message rate and pool size).
5. **SEG-REX's failure mode is instructive, not just disqualifying**: its
   core insight (bound oldest-message-discovery cost via segmentation)
   remains the one structurally-motivated idea in this whole four-plus-
   round lineage that isn't immediately latency-fatal (F3's narrow result
   held up). A future round attacking *only* the reclamation problem (real
   hazard pointers / RCU / epoch-based reclamation done properly, not the
   ad hoc announce/revalidate/quiesce hybrid tried here) is the more
   promising direction than re-trying DWCAS tricks.
6. **SBR-v5's F2 (cache-locality claim) is still genuinely untested** —
   the sandbox lacked perf counters; this is an environment limitation, not
   evidence against the claim, and should be re-run with hardware counters
   before it is used as an argument in any future round.
7. **Both challengers' failures were sanitizer-invisible.** SBR-v5's
   ordering violations and SEG-REX's underflow/deadlock were *not* caught
   by TSan/ASan/UBSan — they required behavioral/sequence assertions in
   the test harness itself. This is a durable methodological lesson: for
   this class of lock-free-queue bug, "0 sanitizer reports" is necessary
   but never sufficient evidence of correctness.

## Spec-update recommendations

**`003-Memory.md`**
- Close the previously "open question" note about ExecStateCell's own
  acquire/release ordering (the one flagged after ADR-031 S2): replace it
  with F4-revised's result — an isolated two-thread litmus with no
  `run_queue.hpp` hop shows the ordering *is* independently load-bearing
  (TSan clean at acquire/release, TSan fires on a relaxed mutant). Cite
  `tests/exec_state_cell_isolated_litmus_test.cpp`.
- Add a documented methodological caveat to the Mailbox section: TSan
  cannot flag a race between two accesses to the *same* atomic object
  regardless of memory_order (confirmed this round when the standard
  "downgrade release→relaxed and expect TSan to fire" control did not
  fire, on both `link_push`'s next-store and the producer's
  `tail_.exchange`). Any future ordering-strength claim for this mailbox
  must be verified via a dedicated isolated litmus (as F4-revised now is),
  not via a downgrade-and-rerun-TSan control on the shipped multi-producer
  test.
- Record the newly-closed `F3b` (cancel racing concurrent
  release/recycle) and `C2b` (48-bit generation wrap boundary) results as
  permanent regression coverage, referencing the new test files below.
- Add a "Rejected Designs" appendix entry for SBR-v5 and SEG-REX
  (round 9), each with the one-line disqualifying defect (ticket-order
  inversion across the overflow seam; segment-reclamation underflow/
  deadlock) so a future round does not re-attempt either mechanism without
  first addressing that specific defect.

**`002-Scheduler.md`**
- No change to the scheduling contract itself. Add a note that the
  Busy/Empty/mid-publish tri-state result contract (already specified) is
  the exact mechanism that let SBR-v5's C2 claim (paused-producer
  Busy-not-Empty) pass cleanly — cite it as the reference correctness
  property any future segmented/ring design must reproduce.

**`001-Actor-Execution-Model.md`**
- No change to the single-executor invariant text. Add a citation to this
  round's C3 (incumbent, max_concurrent_drainers=1 over 300k cycles/3
  workers) and SEG-REX's S3 (same invariant, untouched by that design,
  clean over 4M cycles) as the two independent executed confirmations that
  the invariant is orthogonal to mailbox internals — any future mailbox
  redesign inherits this proof for free only if it does not touch
  ExecStateCell's transitions, exactly as both challengers this round
  correctly did not.

**`015-Reentrancy-and-Quiescence.md`**
- Update the standing gap statement ("a genuinely new O(1) oldest-
  message-discovery mechanism is missing from this lineage") to record
  that SEG-REX's bounded-segment approach *did* achieve a flat, bounded
  p999 reversal-walk latency independent of backlog depth (1.3–17.9µs
  across B∈{4096..262144} at SEG_CAP=256) — the first design in five
  rounds (REX/REX-BIR/REX-CAS/B/REX-CAS/C/SEG-REX) to clear that specific
  bar. Immediately qualify it: this result is necessary but not
  sufficient, since the same design's reclamation protocol (S2/S4) is
  fatally broken; the spec should record the latency result as a reusable
  finding for a *future* attempt that pairs bounded-segment reversal with
  a correct (real hazard-pointer/RCU) reclamation scheme, not as a reason
  to re-attempt SEG-REX's specific announce/revalidate/quiesce mechanism.
- Add the sanitizer-invisibility lesson (residual risk 7 above) as a
  standing methodological requirement for any future mailbox-adjacent
  proof: sequence/order assertions in the harness are mandatory alongside
  sanitizers, not optional.

## Tie-breaking experiment (if either challenger is retried)

Not needed to decide this round — the evidence is sufficient and the gate
is unambiguous. For a *future* round that wants to retry the
bounded-segment idea: the single experiment that would matter most is
swapping SEG-REX's S4 announce/revalidate/quiesce protocol for a real
epoch-based reclamation scheme (global epoch counter, retire-list drained
only after all worker epochs have advanced past the retiring epoch) and
re-running the exact S2/S4 underflow/deadlock repro
(`segrex/s4_underflow_repro*.cpp`, tiny 6-segment pool, plain scheduling,
no delay injection needed) — if that specific repro goes clean, SEG-REX's
F3 latency result becomes reusable evidence; if it does not, the
bounded-segment-reversal angle should be considered closed for this
codebase, matching REX's four prior losses.
