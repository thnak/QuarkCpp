# ADR-041: Mailbox MPSC Hot Path — Round 11 Judgment

## Status

Accepted (incumbent reaffirmed).

## Question

For the actor Mailbox — the MPSC queue that owns FIFO message ordering, stores fixed-size
MessageHandles (never payloads), is written by many producer threads and drained by exactly one
worker at a time (single-executor invariant, 001/002) — which of three competing designs is
(fast) allocation-free and scalable under producer contention, (safe) free of data races/UB/ABA
under TSan+ASan+UBSan, and (correct) strictly FIFO with no lost/duplicated handles and tombstones
skipped exactly once?

This is round 11 of this exact debate. The incumbent (intrusive Vyukov MPSC, pooled-descriptor
node, consumer-private head walk) has now survived ADR-001..004, 020, 027, 029, 031, 032, 033,
and this round, against eleven independently designed, compiled, sanitizer-tested, benchmarked
challengers.

## Designs

1. **Intrusive Vyukov MPSC Mailbox** (incumbent) — the mailbox node *is* the pooled Descriptor
   (pointer-interconvertible, `offsetof(Descriptor, link) == 0`). Producers publish with a single
   unconditional `tail_.exchange(d, acq_rel)` + a release link-store into the displaced node —
   never a CAS retry loop, ABA-free by construction because no enqueue path ever compares an
   address. The consumer walks a private `head_` with plain loads; the only cross-core RMW on the
   drain path is the stub-sentinel boundary re-arm. `try_dequeue()` returns a tri-state
   `{Message, Empty, Busy}` to make Vyukov's non-linearizable emptiness observable without lying.
   Cancellation is a generation-gated single-CAS state flip on the descriptor's own packed word,
   never a queue mutation. Cross-worker ownership handoff and wakeup ride the actor's exec-state
   word plus a seq_cst Dekker fence pair, not a queue-owned atomic.

2. **SegRing-LCRQ** — segmented bounded ring (LCRQ lineage), `fetch_add` slot claim per producer,
   consumer-private batch-sweep drain, Michael–Scott-style single-CAS segment link on overflow,
   hazard-pointer-lite pin array for segment reclamation. Targeted, in order, at the three prior
   disqualifying defects in this design family (SBR-v5's ordering inversion, SEG-REX's reclamation
   underflow, SEG-HP's premature-Empty at rotation). Went through one full debate-cycle revision
   (gate segment retirement on `tail_segment_` mismatch *before* the pin scan, plus a seq_cst
   fence sandwich around the hazard protocol, plus per-lane cache-line padding, plus
   consumer-side link-helping) to close two attacks (a split-CAS UAF and a Dekker-shaped ordering
   gap) found in cross-examination.

3. **ReversingTreiber/BR** — Treiber-LIFO producer push, budget-charged *incremental* consumer-side
   reversal into FIFO order (charging one pointer-flip per drain-budget tick against the existing
   `DrainBudget<N>`, instead of one uninterrupted reversal pass). Explicitly the same lineage this
   repo calls REX/REX-BIR/REX-CAS/B/REX-CAS/C/SEG-REX/SEG-HP, already 6x defeated before this
   round.

## Evidence table

Legend: Survived red-team = claim (possibly revised) still stood after cross-examination.
Proven = Prove-phase executed-C++ verdict. — = not reached (design conceded before Prove, or
claim out of scope for the available toolchain).

| Design | Claim | Kind | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|---|---|
| Incumbent | F1 no-alloc/wait-free enqueue | fast | yes | **CORRECT** | 5-instruction fixed enqueue (mov/mov/xchg/mov/ret), 0 allocator calls over 10M×4P messages |
| Incumbent | F2 zero-RMW steady drain | fast | yes | **CORRECT** | 1 boundary-rearm RMW draining a 100,000-node backlog (not O(N)) |
| Incumbent | F3 occupancy-1 p50≤100ns | fast | yes (revised: verdict-not-number claim) | **INCONCLUSIVE** | p50=12.30ns / p999=29.70ns on this idle host; ADR-029 recorded p50=374–434ns on a loaded host — completeness requirement (report both) not fully closeable this session |
| Incumbent | F4 sub-linear P1→P4 scaling | fast | yes (revised: lead with direct ratio) | **CORRECT** | T(P4)/T(P1)=0.586 (157.12→92.06 Mmsg/s), net regression as predicted; isolated control ratio 3.476 |
| Incumbent | S1 ABA-free tail_ exchange | safe | yes (revised: scope narrowed to Mailbox, not pool) | **CORRECT** | Exactly 1 exchange (tail_, no comparison) + 2 CAS sites (gen_state, generation-tagged, never a raw pointer) |
| Incumbent | S2 Dekker fence load-bearing (x86) | safe | yes (revised: x86-scoped, ARM64 open) | **CORRECT** | Fenced: 0/80M lost. Unfenced mutant: 2/20M lost per run, 3/3 runs |
| Incumbent | S3 gen-gated cancel prevents UAF | safe | yes | **CORRECT** | Shipped: 0/1M ASan aborts. Naive bare-pointer control: ASan UAF on first race |
| Incumbent | S4 single-executor invariant | safe | yes | **CORRECT** | max_observed_active_drainers=1 over 1.6M drains, 4 workers |
| Incumbent | C1 per-producer FIFO | correct | yes | **CORRECT** | 10M messages, in-band sequence tag, 0 inversions |
| Incumbent | C2 no lost/duplicated handle | correct | yes (revised: caller-contract precondition made explicit) | **CORRECT** | enqueued=handled=10M at P∈{1,2,4}, 0 double-fires |
| Incumbent | C3 Busy never misreported Empty | correct | yes | **CORRECT** | 20M forced-window polls, 0 Empty, 0 fabricated Message |
| Incumbent | C4 tombstone reclaimed exactly once | correct | yes | **CORRECT** | 1M claim-vs-cancel races, exactly_once=1M, 0 leaks, 0 double-frees |
| SegRing-LCRQ | F1 no-alloc/wait-free enqueue | fast | yes | **CORRECT** | 0 allocator calls / 50M enqueues, 62.87 Mmsg/s, retries=0.008% |
| SegRing-LCRQ | F2 zero-RMW interior scan | fast | yes | **CORRECT** | disasm: 0 lock-prefixed instructions in the interior already-ready-slot loop |
| SegRing-LCRQ | F3 throughput parity w/ incumbent | fast | no (withdrew a priori parity bound) | **INCONCLUSIVE** | P1=86.34, P2=54.54, P4=68.77, P8=77.44 Mmsg/s — below incumbent at every comparable point |
| SegRing-LCRQ | S1 TSan/ASan/UBSan-clean | safe | yes (revised protocol) | **WRONG** (as-submitted) | Deterministic ASan heap-UAF in `try_help_link` on first run at P=4; patch (reorder pin-clear) required |
| SegRing-LCRQ | S2 no producer dereferences a freed segment | safe | yes (revised protocol, 2 prior attacks closed) | **WRONG** (as-submitted) | **New, third** UAF window found in Prove — pin cleared before `try_help_link` still uses the segment. Cheap fix exists (reorder one store) and verified clean post-patch |
| SegRing-LCRQ | S3 bounded retirement, no leak | safe | yes | CORRECT *(post-patch only)* | max_retire_pending=3 (bound 9), allocated=freed=726,129 |
| SegRing-LCRQ | C1 total cross-segment FIFO | correct | yes | CORRECT *(post-patch only)* | 24M messages, up to 1.88M segment links, 0 inversions |
| SegRing-LCRQ | C2 Busy not misreported at rotation boundary | correct | yes | CORRECT *(post-patch only)* | 10,000/10,000 trials resolve in ≤1 drain_step call |
| SegRing-LCRQ | C3 no lost/duplicated handle | correct | yes | CORRECT *(post-patch only)* | As-submitted ordering **livelocked** (1.2–2.0M/8M drained, hung) on 3 separate runs; patched: 8M/8M, 0 dup, 0 missing |
| SegRing-LCRQ | C4 tombstone skip cost | correct | yes | CORRECT *(post-patch only)* | 3.006us/1024 skips (vs. incumbent's cited ~21us, different hardware, not head-to-head) |
| ReversingTreiber/BR | C1 per-producer FIFO | correct | **no — conceded fatal** | **WRONG** | Compiled hotPathSketch verbatim: enqueue(A,B,C) → dispatch order C,B,A (100% reversed, deterministic) |
| ReversingTreiber/BR | F3 O(1)/bounded reversal cost | fast | **no — conceded fatal** | **WRONG** | Full-reversal-to-completion: 161.9us at B=65536, 1702.1us at B=262144 — breaches the 50us hard ceiling |
| ReversingTreiber/BR | (new) DrainBudget enforcement | safety-adjacent | conceded independently | **WRONG** | 10,000 ordinary messages dispatched in one scheduler turn against `DrainBudget<3>`; budget counter underflowed uint32_t |
| ReversingTreiber/BR | F1/F2/S1–S4/C2/C3 | — | conceded (full concession pre-Prove) | not run | Governed by 015's own bar: lineage barred from re-entry without a genuinely new O(1) oldest-message-discovery mechanism, which this design concedes it does not bring |

## Decision

**Winner: Design 1 — Intrusive Vyukov MPSC Mailbox (incumbent).** No spec or code change.

**Rationale, applying the stated ranking in order:**

1. **Safety gate.** The incumbent is the only design with a clean safety record: all four safety
   claims (S1–S4) were proven CORRECT with no WRONG verdict at any point in this round.
   SegRing-LCRQ's S1 and S2 both came back **WRONG as submitted** — a fresh, third
   distinct use-after-free window (pin cleared before `try_help_link` still dereferences the
   segment) surfaced in the Prove phase, on code that had *already* been revised once in
   cross-examination to close two earlier UAF windows in the same reclamation mechanism. A cheap
   fix does exist (reorder one store) and was verified to close it — so this design is not
   automatically disqualified by the letter of the gate rule — but the fix was never itself put
   through adversarial red-teaming, and the as-submitted design's C3 test independently
   **livelocked** (hung, not merely mismatched) on 3 separate 8M-message runs, a failure mode
   distinct from and not predicted by the attacks that were actually run. Three consecutive UAF
   discoveries (cross-exam attack 1, cross-exam attack 2, Prove-phase discovery) against the same
   hazard-pointer reclamation mechanism, in a design lineage whose three direct predecessors
   (SBR-v5, SEG-REX, SEG-HP) each already failed for exactly this class of defect, is a pattern of
   structural fragility this ranking framework is built to weigh against a winner — the gate is
   about trustworthiness of the *safety proof*, not just whether a last-mile patch exists.
   ReversingTreiber/BR fails the safety-adjacent bar outright (fabricated `DrainBudget<N>`
   enforcement, unrelated to and independent of its already-fatal C1) and additionally violates a
   hard *correctness* ground rule (FIFO), which is disqualifying on its own regardless of the
   safety gate.

2. **Proven beats claimed.** The incumbent has 11 of 12 claims proven CORRECT by executed
   evidence, 1 INCONCLUSIVE (F3 — a completeness-of-reporting gap, not a falsified number: the
   number that *was* measured this session cleared both the 100ns goal and the 250ns hard budget).
   SegRing-LCRQ's correctness claims (S3, C1–C4) are CORRECT only for a post-hoc-patched variant
   that was not the design cross-examined; measured strictly as submitted, S1/S2 are WRONG and C3
   hung. ReversingTreiber/BR contributes zero surviving proven claims — full pre-Prove concession,
   and the targeted re-verification confirmed WRONG on every claim actually tested.

3. **Measured hot-path numbers, among safe survivors.** Only the incumbent clears the safety gate
   outright, so this tier is not strictly needed to decide — but for completeness: the incumbent's
   raw throughput beats SegRing-LCRQ's at every P measured this session (P=1: 157.12 vs 86.34
   Mmsg/s; P=4: 92.06 vs 68.77 Mmsg/s), and SegRing-LCRQ's own P=1→P=2→P=4→P=8 curve
   (86.34 → 54.54 → 68.77 → 77.44) is non-monotonic and unexplained, a red flag on its own that
   was not investigated further this round. SegRing-LCRQ's F2 (zero-RMW interior scan) and F1
   (allocation-free fast path) do match the incumbent's shape, so segmentation is not "faster,"
   only "differently shaped," on the evidence gathered.

4. **Core invariants.** The incumbent bends nothing: it is the exact single-executor-invariant,
   FIFO-by-default, zero-heap-alloc, no-RTTI/virtual design already codified in 003/002/001.
   SegRing-LCRQ's pre-allocated first segment (16KB at the documented `kSegCapacity=1024`,
   16B/slot) is a ~85x regression against ADR-015's measured 192B idle-actor footprint — not a
   violation of a named ground rule, but a real, unresolved tension with this project's low-idle-
   footprint posture that the design's own risk section names and does not resolve.
   ReversingTreiber/BR directly violates the ground-rule "mailbox ordering is FIFO by default,"
   confirmed by execution, not argument.

**Net: the incumbent wins decisively.** Round 11 changes nothing about the shipped design;
`include/quark/core/{mailbox.hpp,descriptor.hpp,exec_state.hpp}` remain the authoritative
implementation, matching 001/002/003 as written.

## Residual risks

- **F3 (incumbent) is host-conditional, not universally closed.** This session measured p50=
  12.30ns on an idle host, clearing the 100ns goal and 250ns hard budget comfortably, but could
  not reproduce ADR-029's adverse (load-average-17 host, p50=374–434ns) condition within this
  session's machine-safety thread cap. Any future report of this metric must state both numbers
  side by side; a p50 above 250ns on a realistically loaded host would need re-litigating whether
  the *budget itself* (not the design) should move, since ten rounds of evidence now show the
  incumbent's own historical worst case already touches that ceiling.
- **ARM64 memory-order sufficiency remains unproven for the incumbent's Dekker fence pair.** S2 is
  proven only on x86-TSO this round (as in all ten prior rounds). The PAL seam
  (`pal::store_load_barrier()`) retains a real barrier off x86, but no herd7/cppmem litmus proof
  or physical-ARM64-hardware counted run has closed this gap. A green TSan-on-ARM64 run would
  *not* close it either (TSan's model doesn't validate hardware ordering sufficiency).
- **Caller-contract violations (double-tell of a live descriptor; two concurrent `try_dequeue`
  callers) remain sanitizer-silent and release-build-undetected** for the incumbent. This is a
  disclosed, accepted precondition (C2 is now scoped explicitly to contract-compliant callers),
  not a defect, but it is a live sharp edge for any future caller-side code (e.g. new admission-
  control or fan-out paths) that must not be allowed to violate single-membership or single-
  consumer.
- **SegRing-LCRQ's Prove-phase UAF discovery is evidence, not proof, that the hazard-pointer
  reclamation approach is unsalvageable** — a fourth attempt with the pin-clear-ordering fix
  applied *from the start* and put through a fresh cross-examination round could plausibly close
  it. This ADR does not rule that out; it rules that the design as evaluated this round does not
  win, given the safety gate's spirit (a proof that has already needed three successive patches to
  the same mechanism carries materially less weight than a ten-round-unblemished incumbent).
- **The `SegRing-LCRQ` non-monotonic P1→P2→P4→P8 throughput curve (86.34 → 54.54 → 68.77 → 77.44
  Mmsg/s) was not root-caused this round.** If this design lineage is ever revisited, that curve
  shape should be explained (or shown to be measurement noise) before any throughput comparison is
  trusted.
- **ReversingTreiber/BR should be treated as closed for this lineage.** This is its seventh
  consecutive defeat (ADR-002/003/004, 020, 027, 029, 031, 032, 033, and now this round). Per
  015's own governing text, no further proposal from the REX/REX-BIR/REX-CAS/SEG-REX/SEG-HP/
  ReversingTreiber lineage should be entertained without a genuinely new O(1) oldest-message-
  discovery mechanism — incremental budget-charging of an O(batch) reversal, as tried here, is not
  such a mechanism and additionally introduces an independent `DrainBudget<N>` enforcement bug on
  its ordinary-dispatch path.

## Spec recommendations

No design or algorithmic change to 001/002/003/015 is warranted — the incumbent implementation is
unchanged and already matches all four specs as written. The following are documentation-only
tightenings that make the specs match what this round's evidence actually established (closing
gaps the red-team found in how prior rounds' claims were *stated*, not in the design itself):

- **`002-Scheduler.md`** — in the occupancy-1 latency section (near the existing ADR-029
  374–434ns citation), add this round's favorable-host number (p50=12.30ns / p999=29.70ns,
  ADR-041) explicitly alongside the adverse one, with a standing instruction that any future
  citation of this metric must quote both, per the "budget-compliance verdict, not a single ns
  figure, is what carries across hosts" framing established in this round's rebuttal.
- **`002-Scheduler.md`** — add the direct `T(P=4)/T(P=1)` throughput-regression ratio (0.586 this
  round, ~0.757 in ADR-032/033 on different hardware) as the headline framing for the mailbox's
  known contention-scaling limitation, demoting the isolated-cache-line-control comparison to
  secondary/explanatory context — matching the wording fix this round's rebuttal already applied
  to claim F4.
- **`003-Memory.md`** — narrow the "ABA-free by construction" language for `Mailbox::enqueue` to
  explicitly scope it to the mailbox's own `tail_` exchange and the `gen_state` CAS (neither ever
  compares a raw `Descriptor*`), and add an explicit cross-reference stating the companion
  `MessagePool` free-list's ABA closure is a separate, independently-proven boundary
  (`tests/mailbox_pool_aba_stress_test.cpp`) not covered by or borrowing evidence from this
  scoped claim — closing the over-generalization gap the red-team flagged (minor, S1).
- **`003-Memory.md`** — state the caller-contract preconditions for the mailbox's FIFO/no-lost/
  no-duplicate guarantee explicitly in the guarantee's own text (single-membership: a live
  `Descriptor*` enqueued at most once; single-consumer: exactly one concurrent `try_dequeue`
  caller per mailbox), rather than only in prose risk notes — matching the C2 claim revision this
  round, and note that violations are sanitizer-silent logical corruption (self-loop/fork in the
  intrusive chain), not a memory-safety bug, so ASan/TSan cleanliness must not be read as evidence
  these preconditions hold.
- **`019-PAL` (or wherever the PAL fence seam is specified — not in the four assigned files but
  referenced by all of them)** — record the open ARM64 litmus-proof gap for the close-out Dekker
  fence pair as a tracked open item, explicitly stating that a green TSan run on ARM64 hardware
  would not close it (TSan's C++ memory model does not validate hardware-ordering sufficiency on
  any ISA) — the correct closing evidence is a herd7/cppmem litmus proof plus a physical-hardware
  counted lost-wakeup run.
- **`015-Reentrancy-and-Quiescence.md`** — record this round's ReversingTreiber/BR result as the
  lineage's seventh consecutive defeat and tighten the existing bar's wording to explicitly name
  "incremental/budget-charged resumable reversal of an O(batch) mechanism" as *not* satisfying the
  "genuinely new O(1) oldest-message-discovery mechanism" requirement, since this round's proposal
  attempted exactly that framing and it was not sufficient — closing the ambiguity the design's own
  risk section flagged ("whether resliced-but-still-O(batch) satisfies the spirit of the bar... is
  a judgment call"). Also record the newly-found independent `DrainBudget<N>` enforcement bug
  (case-1 dispatch branch missing a `budget == 0` check, causing uint32_t underflow) as a
  reference example of the class of bug this lineage tends to introduce, for any future red-team
  round in this space to check for by default.
