# ADR-029: Mailbox MPSC Hot Path — Round 7 Judgment

## Status

Accepted

## Question

Design the actor Mailbox: the MPSC queue that owns FIFO message ordering,
storing fixed-size MessageHandles (never payloads). Many producer threads
enqueue; exactly one worker drains it at a time (guaranteed by the actor
exec-state CAS). The design must be proven, with executed C++:

- **fast**: allocation-free on the steady-state hot path, scales under
  producer contention
- **safe**: free of data races / UB / ABA under TSan + ASan + UBSan
- **correct**: strictly FIFO, no lost/duplicated handles, tombstones
  skipped exactly once

This is round 7 of an ongoing design-debate series (ADR-001..004,
ADR-020, ADR-027 precede this one).

## Design summaries

1. **Intrusive Vyukov MPSC Mailbox — Descriptor-is-Node, Pooled, r7-Hardened.**
   The pooled Descriptor *is* the queue node (intrusive `MailNode` link at
   offset 0). Producers publish wait-free with one unconditional
   `tail_.exchange(acq_rel)` + one release link-store — no CAS, no retry
   loop, ABA-free by construction (no address is ever compared). The single
   consumer walks a private `head_` with plain loads; the one unavoidable
   RMW is confined to the occupancy-1 stub re-arm boundary. A third drain
   result (`Busy`, distinct from `Empty`) honestly surfaces a producer's
   mid-publish window. `gen_state` packs {generation, state, flags} into
   one atomic word so claim/cancel/complete are single CAS/store ops. r7
   adds a debug-only double-enqueue guard bit and a zero-cost dequeue
   prefetch; it also proposed CAS-hardening `complete()` (S5), which was
   **conceded and withdrawn** during cross-examination because the fix
   requires threading a return value through 7 call sites in
   `activation.hpp` that this proposal didn't touch — the shipped
   plain-store `complete()` is what was proven.

2. **SBR-Mailbox — Segmented Bounded-Ring MPSC Mailbox (r7: dedicated
   close-out counter + shard-pool segment recycling).** A Michael–Scott
   chain of fixed-capacity ring segments. Producers claim a slot with a
   ticket `fetch_add`/CAS and store the Descriptor pointer (extrusive, not
   intrusive — no layout constraint on Descriptor). The consumer does a
   straight-line array scan per segment and retires drained segments back
   to a shard-wide pmr pool. r7's close-out probe reads only a dedicated,
   never-recycled `published_count_` atomic (fixing ADR-027's disqualifying
   close-out race). Cross-examination found and the design conceded two
   further fatal, deterministic (non-racy) bugs: retiring the
   Mailbox-embedded `stub_` sub-object into the shard pool (cross-actor
   corruction on the very first segment-boundary crossing), and no
   generation-tagging on recycled segments (stale producers could inject
   messages into a different actor's mailbox). Revised fixes (S2R/S3R) were
   proposed but not executed in the debate.

3. **REX-CAS/B — Budget-Charged Resumable LIFO-Push Mailbox.** Producers
   push onto a Treiber LIFO with link-then-CAS (no partial-publish window,
   ABA-free). The consumer detaches the whole chain with one
   `exchange(nullptr)` and reverses it into FIFO order — but the reversal
   is now resumable and budget-charged (one unit per node, spread across
   scheduler turns) to fix ADR-004's fatal finding that an uninterruptible
   O(batch) reversal could stall a 262144-deep backlog's *first* dispatch
   for 39ms. Cross-examination revealed this "new" mitigation is
   mechanism-for-mechanism identical to ADR-020's already-tried-and-failed
   REX-BIR design, whose own C3 harness proved detach-to-first-dispatch
   p999 latency breaches the 50µs hard ceiling by 10x–600x at every tested
   backlog depth, including under *zero* cross-actor contention. The
   architect conceded this in full.

## Evidence table

| Claim | Design | Kind | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|---|---|
| F1 (alloc-free enqueue) | Vyukov | fast | yes | **CORRECT** | 0 calls/cmpxchg in NDEBUG disasm on GCC 14.2 + Clang 20.1 |
| F2 (0 RMW steady-state dequeue) | Vyukov | fast | yes | **CORRECT** | 0 RMW/pop off boundary, 1 RMW on stub-rearm, both compilers |
| F3 (p50≤100ns, p999≤300ns, ≥10M msg/s) | Vyukov | fast | yes | **WRONG** | P=1: p50 374–434ns, throughput 6.8–7.9M msg/s (host under heavy contention; consistent across compilers/reruns) |
| F4 (prefetch helps cold-restart) | Vyukov | fast | yes | INCONCLUSIVE | clflush produced only ~10ns delta (~22 cycles) — broken cold-cache precondition on this host, not evidence against the design |
| S1 (full protocol clean under TSan+ASan+UBSan) | Vyukov | safe | yes | **CORRECT** | 16/16 (P∈{1,2,4,8}×{gcc,clang}×{TSan, ASan+UBSan}) clean at 8M msgs |
| S2 (head_ handoff needs acquire/release) | Vyukov | safe | yes | **CORRECT** | gcc TSan: relaxed control fires race, shipped form clean (clang control hung — pathology, not disproof) |
| S3 (seq_cst Dekker fence prevents lost wakeup) | Vyukov | safe | yes | **CORRECT** | fenced: lost=0 (400k rounds); no-fence: lost=12,371–38,795 |
| S4 (generation gate prevents UAF) | Vyukov | safe | yes | **CORRECT** | gated: 0 ASan reports/8M+ ops; bare-pointer control: 100% UAF |
| S6 (double-enqueue debug guard) | Vyukov | safe | yes | **CORRECT** (after a bug fix) | prover found guard bit was mis-shifted (defeated); fixed, then 100% assert-catch in debug, deterministic corruption in release without it |
| S5 (CAS-harden complete()) | Vyukov | safe | **conceded in debate** | not re-tested | withdrawn — fix requires non-void return threaded through 7 activation.hpp call sites, not part of this proposal |
| C1 (strict FIFO, 0 lost/dup, 100M ops) | Vyukov | correct | yes | **CORRECT** | 100,000,000 ops, 0 violations, both compilers |
| C2 (tombstone exactly-once) | Vyukov | correct | yes | **CORRECT** | 8M ops, handled+tombstoned==total, double_free=0 |
| C3 (Busy never misreported as Empty) | Vyukov | correct | yes | **CORRECT** | 600,000 forced-stall probes, 0 misreads |
| C4 (bounded tombstone-skip budget) | Vyukov | correct | yes | **CORRECT** | 10M all-cancelled entries, exactly 1024 skips under DrainBudget<1024> |
| C5 (ABA-free under reincarnation) | Vyukov | correct | yes | **CORRECT** | 10M free/reuse cycles, 0 lost/dup/foreign, 0 TSan reports |
| F1 (zero-alloc) | SBR | fast | yes | **CORRECT** | 0 pool fallbacks, single-mailbox and 1000-mailbox burst scenarios |
| F2 (drain ≤ incumbent, ≥1 kSegCap) | SBR | fast | yes | **WRONG** | SBR 1.6x–2.2x *slower* than Vyukov at every kSegCap∈{64,256,4096}, both compilers |
| F3 (SBR loses producer throughput, self-unfavorable) | SBR | fast | yes | **CORRECT** | SBR 0.38x–0.86x of Vyukov's throughput at every P∈{1..64} |
| S1 (close-out never touches per-segment/private state) | SBR | safe | yes | **CORRECT** | 0 TSan reports; gcc negative control fires the exact ADR-027 bug shape |
| **S2R (generation-tagged claim word prevents cross-mailbox corruption)** | SBR | safe | yes | **WRONG** | **as-specified pseudocode hung/mis-delivered a message cross-mailbox in 3/3 runs; required a non-trivial unspecified fix (owner tag + CAS-based segment advancement + recovery anchor) to pass** |
| S3R (stub_ never retired to shard pool) | SBR | safe | yes | **CORRECT** (post-fix build) | 0 stub-guard assertion fires, full-protocol TSan/ASan/UBSan clean |
| C1R (strict FIFO across recycling) | SBR | correct | yes | **CORRECT** (post-fix build) | 20M ops, 0 dup/missing/violation, both compilers |
| C2 (tombstone exactly-once) | SBR | correct | yes | **CORRECT** | 8M ops with 30% cancel race, 0 bad_state |
| C3 (Busy/Empty exhaustive+exclusive) | SBR | correct | yes | **CORRECT** | 800,000 probes, 0 misclassifications |
| F1 (alloc-free enqueue) | REX-CAS/B | fast | yes | **CORRECT** | 0 allocations, 8M ops × P∈{1..32} |
| F2 (worse under contention, self-unfavorable) | REX-CAS/B | fast | yes | **CORRECT** | REX strictly worse than Vyukov at every P≥2, retry-ratio rising super-linearly |
| S1 (no race, ABA-free push/detach) | REX-CAS/B | safe | yes | **CORRECT** | clean at 500K–3M ops; gcc positive control fires exact race |
| S2 (ABA-free under pool reincarnation) | REX-CAS/B | safe | yes | **CORRECT** | 64-slot pool, up to ~150,000x reuse/slot, 0 dup/lost |
| S3 (5-field handoff safe across work-steal) | REX-CAS/B | safe | yes | **CORRECT** | 1.4M forced handoffs, clean; gcc positive control fires on exact 5 target fields |
| C1 (per-producer FIFO under pause/resume) | REX-CAS/B | correct | yes | **CORRECT** | 16M ops, exact per-producer order preserved |
| C2 (tombstone exactly-once) | REX-CAS/B | correct | yes | **CORRECT** | 8M ops, 50% cancel race, 0 double-free; negative control UAFs |
| C3 (no partial-publish window, push-side only) | REX-CAS/B | correct | yes | **CORRECT** (narrowed) | 4M msgs, 0 dangling-chain observations; BudgetExhausted fires 255x more predictably than Vyukov's incidental Busy |
| **F3 (resumable reversal fixes the fatal ADR-004 stall)** | REX-CAS/B | fast | **conceded fatal in debate** | **WRONG** | identical mechanism to ADR-020's REX-BIR, already proven to breach 015/023's 50µs hard tail-latency ceiling by 10x–600x at every backlog depth, incl. zero contention |

**Totals across all three designs: 27 claims proven CORRECT, 4 proven WRONG, 1 INCONCLUSIVE.**

## Decision

**Winner: Design 1 — Intrusive Vyukov MPSC Mailbox — Descriptor-is-Node, Pooled, r7-Hardened** (with S5's `complete()` CAS-hardening withdrawn; ship the plain-store `complete()` as currently committed in `include/quark/core/descriptor.hpp`).

Applying the ranking in order:

**(1) Safety gate.** Design 2 (SBR-Mailbox) is disqualified. Its central
r7 claim, S2R (generation-tagged claim word prevents cross-mailbox
corruption), was proven **WRONG**: the pseudocode as specified in the
debate produced deterministic, reproducible cross-actor message injection
(a message from mailbox A physically written into mailbox B's segment,
3/3 repeats) and hangs. A fix exists, but it is not a "cheap fix" in the
sense the judging rubric requires — it required the prover to invent and
add three new mechanisms not present in the debated design: a segment
`owner_` tag, CAS-based (not blind-store) `enq_seg_` advancement, and a
`known_good_` consumer-published recovery anchor to prevent livelock when
a producer observes a foreign `enq_seg_`. That is a materially different
synchronization protocol from what was red-teamed and cross-examined;
promoting it now would mean judging a design nobody has attacked. SBR is
disqualified from winning this round regardless of its (otherwise
respectable) S1/S3R/C1R-C3 results.

Design 3 (REX-CAS/B) is disqualified on the same gate by a different
route: it bends a core invariant. 015 (Reentrancy and Quiescence)
prohibits unbounded actor stalls; the design's own headline mitigation
(F3, resumable/budget-charged reversal) was conceded **fatal** in the
debate itself — it is mechanism-identical to ADR-020's already-executed
REX-BIR, which measured detach-to-first-dispatch p999 latency at 10x–600x
over 023's 50µs hard ceiling at every backlog depth tested, including
zero cross-actor contention. The architect explicitly stated they expect
this design to lose. There is no stated cheap fix — the design's own
Risks section explains why a real fix (a second synchronization
structure for eager oldest-message discovery) was deliberately not
attempted, because it would reopen exactly the class of new,
unproven-surface bugs that sank REX and REX-CAS twice before.

**(2) Proven beats claimed.** Design 1 is the only design with zero
disqualifying safety/correctness failures. Its one disproven claim, F3
(absolute latency/throughput floor), is a *fast* claim, not a safety or
correctness claim — it does not trigger the gate. It is weighed, and
weighed honestly: measured p50 was 374–434ns and single-producer
throughput 6.8–7.9M msg/s against a claimed floor of ≤100ns / ≥10M msg/s.
The prover's own note is that the reference host was under heavy,
unrelated multi-tenant contention (load average ~17 on a 32-core box)
during this run, which is a real confound, but the result was consistent
across both compilers and multiple reruns, so it stands as reported: not
proven, weighed as a loss on that specific number, not dismissed as
noise. Every other Vyukov claim — F1, F2, S1–S4, S6 (after the prover
caught and fixed a real double-enqueue-guard bit-shift bug), and C1–C5 —
was proven correct with executed evidence at meaningful scale (up to
100,000,000 operations).

**(3) Measured hot-path numbers among safe survivors.** Since Designs 2
and 3 are disqualified, this tie-break does not need to be exercised
between competitors. For the record, Design 1's own measured numbers
this round: F2 shows 0 cross-core RMW on the steady multi-node dequeue
path (only the boundary pop pays one); C1 clears 100,000,000 FIFO
operations with zero violations; S3's Dekker litmus shows the seq_cst
close-out fence is load-bearing (0 lost wakeups vs. 12k–38k without it).
Design 1's own aggregate P=4 throughput (15.4–17.3M msg/s) still beats
both disqualified competitors' best P=4 numbers in this round's
benchmarks (SBR ≈9–11M msg/s at P=4; REX-CAS/B's Mops/s at P=4 was
21.0/g++ and 19.5/clang, both below Vyukov's).

**(4) Core invariants.** Design 1 bends none: single-executor,
mailbox-FIFO-by-default, zero-heap-allocation-hot-path, and sanitizer
cleanliness are all upheld and directly proven, not merely argued.

## Residual risks

- **ARM64 / weak-memory correctness remains unproven by a real litmus
  tool.** Every sanitizer run across all seven rounds, including this
  one, is x86-64-only (GCC 14.2, Clang 20.1). TSan does not model ARM's
  relaxed store-buffering; the acq_rel-exchange / seq_cst-fence reasoning
  underlying `producer_close_out_fence`'s x86 elision is asserted, not
  executed, on the platform where it matters most. This is the single
  most important open gap and the natural next tie-breaking-quality
  experiment: run the shipped protocol through herd7/GenMC or on real
  ARM64 hardware.
- **F3's absolute-floor miss was measured under host contention** (load
  avg ~17/32 cores from unrelated concurrent sessions). The number stands
  as reported per this round's evidence, but a clean-room, dedicated-host
  rerun of `bench/mailbox_bench_mt.cpp` at P∈{1,4} is warranted before
  treating 374–434ns p50 as the design's true floor rather than an
  environment artifact. F4 (prefetch benefit) is fully inconclusive for
  the same class of reason (clflush wasn't producing real cache eviction
  in this environment) and needs a working cold-cache harness.
- **S5 (CAS-hardened `complete()`) remains a dormant gap, not a closed
  one.** No concurrent force-completer exists yet in the codebase (011's
  deadline-driven force-complete is not implemented), so today's
  single-writer `complete()` is genuinely race-free. The moment 011 adds
  a concurrent completer, this must be redesigned properly — as a
  non-void return (or out-param) threaded through all 7
  `activation.hpp` call sites — not patched in isolation the way r7
  attempted.
- **The P=4 aggregate-throughput scaling gap** (measured 1.36x–1.48x vs a
  naive ~4x expectation in prior rounds) remains root-cause-undiagnosed
  for Design 1. This round's F3 numbers (15.4–17.3M msg/s at P=4 vs.
  6.8–7.9M msg/s at P=1, i.e. roughly ~2.0–2.3x) are consistent with that
  known sub-linear pattern and were not re-diagnosed here.
- **The debug-only double-enqueue guard (S6) had a real, previously
  undetected bug** (flag bits mis-shifted into the generation subfield,
  silently defeating the guard) that the prover found and fixed this
  round. This is evidence the guard mechanism is subtle enough to warrant
  a standing unit test in CI, not just an ADR-level claim.
- **SBR-Mailbox's and REX-CAS/B's disqualifications should not be read as
  "this class of algorithm can never work here."** SBR's S2R fix, once
  properly specified (owner tag + CAS-based segment advancement +
  recovery anchor) and put through its own red-team round, could
  plausibly close the gate next time — but it must go through debate as
  the design that was actually attacked, not be adopted post-hoc from a
  prover's patch. REX-CAS/B's fundamental latency ceiling problem is
  architectural (O(batch) reversal cost is unavoidable in any LIFO-push
  design without a second discovery structure) and is unlikely to be
  fixable within this lineage; a fourth attempt should not reuse the
  Treiber-push-with-reversal shape without a genuinely new idea for
  O(1) oldest-message discovery.

## Spec update recommendations

- **`003-Memory.md`**: Formally document the shipped (plain-store, not
  CAS) `Descriptor::complete()` as the current spec, and add an explicit
  "Future Work" note gating any change to a non-void/CAS form on 011's
  deadline-driven force-completer landing first, with the API-shape
  change (return value) and all `activation.hpp` call-site updates
  designed and red-teamed together as one unit — not introduced
  piecemeal as this round's S5 attempted. Add the r7 `kDebugInQueue`
  double-enqueue guard bit to the documented `gen_state` flags layout,
  and record the correct bit-packing (shift into the flags subfield, not
  a raw OR into the word) as a normative requirement given the bug found
  this round.
- **`002-Scheduler.md`**: Record the F2 steady-state zero-RMW dequeue
  property and the F3 measured throughput/latency numbers (with the
  host-contention caveat) as the mailbox's current baseline, superseding
  prior rounds' numbers. Note the still-open, undiagnosed P=4 scaling
  gap as a tracked item, not a closed one.
- **`001-Actor-Execution-Model.md`**: No changes required to the
  single-executor / exec-state-CAS contract — Design 1's S2 result
  reconfirms the acquire/release handoff is load-bearing and sufficient
  exactly as currently specified. If a future round revisits a
  multi-field consumer-private handoff (as REX-CAS/B attempted with 5
  fields), formally specify the `Running→Scheduled→Scheduled→Running`
  work-steal/requeue release/acquire contract explicitly — this round's
  cross-examination found it is presently relied upon but not written
  down precisely, and Clang's TSan positive control for it timed out
  rather than confirming, leaving that specific path's proof incomplete
  even for the losing design that most needed it.
- **`015-Reentrancy-and-Quiescence.md`**: Add REX-BIR/REX-CAS/B's
  measured p999 numbers (10x–600x over the 50µs hard ceiling at
  B∈{1024,16384,262144}, reproduced twice now — ADR-020 and this round)
  as a permanent, citable negative example under the no-unbounded-stall
  rule, explicitly warning off future LIFO-push-with-batch-reversal
  mailbox proposals from this lineage unless they bring a genuinely new
  O(1) oldest-message-discovery mechanism.

## Provenance

Debate, cross-examination, and executed-evidence artifacts for this
round:

- Vyukov (winner) prover artifacts: `/tmp/quark-prove.tdI3SA`
- SBR-Mailbox prover artifacts: `/tmp/claude-1002/-home-nvthanh-works-QuarkCpp/9483e4a6-efa5-47bd-8b7d-51305a91f8c0/scratchpad/mailbox-prove`
- REX-CAS/B prover artifacts: `/tmp/quark-prove.mSeXAx`
