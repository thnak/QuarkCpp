# ADR-020: Mailbox MPSC Hot Path — Round 5 (Vyukov intrusive design reconfirmed as winner)

## Question

Design the per-actor Mailbox: the MPSC queue that owns FIFO message ordering,
storing fixed-size MessageHandles (never payloads). Many producer threads
(tell/ask from any worker) enqueue; exactly one worker drains it at a time,
guaranteed by the actor exec-state CAS (single-executor invariant, 001).
Prove it is:

- **fast** — allocation-free on the steady-state hot path, scales under
  producer contention;
- **safe** — free of data races / UB / ABA under TSan+ASan+UBSan;
- **correct** — strictly FIFO, no lost/duplicated handles, tombstones
  skipped exactly once.

Ground-rule invariants that no design may violate: at most one executor
per actor at any instant; mailbox ordering is FIFO by default; no heap
allocation on the steady-state hot path; no data race/UB under
TSan+ASan+UBSan; C++23 std-only, zero-cost policy dispatch, no RTTI on
the hot path.

This is the fifth executed round on this exact question (ADR-001..004
covered earlier iterations of the Vyukov design and the REX-CAS/REX-BIR
Treiber-stack lineage; this round adds a new competitor, the Segmented
Linear-Chunk Ring, and re-runs REX-BIR's latest incarnation, REX-BIR
bounded-incremental-reversal, against both).

## Designs (one-line summaries)

1. **Intrusive Vyukov MPSC Mailbox** — the pooled `Descriptor` *is* the
   queue node (intrusive `MailNode` first member). Producers publish via
   one unconditional `tail_.exchange(acq_rel)` + one release link-store —
   no CAS, no retry loop. The single draining consumer walks a
   consumer-private `head_` with zero cross-core atomic RMW on the
   steady multi-node path; the one unavoidable consumer atomic (stub
   re-arm) fires only at the occupancy-1 boundary. Cancellation is a
   single packed `{generation:48,state:4,flags:12}` CAS. Non-linearizable
   emptiness is surfaced honestly as a third `Busy` drain result.

2. **Segmented Linear-Chunk MPSC Ring** — producers claim a slot via an
   unconditional `fetch_add` on a per-segment ticket counter; overflow
   (ticket ≥ capacity) CAS-links a fresh, geometrically-grown segment.
   The consumer walks a private cursor with zero cross-core RMW.
   Retired segments are *never freed while the actor is live* (tier-1,
   freed only at teardown) — trading unbounded-in-principle memory for
   zero reclamation-safety machinery.

3. **REX-BIR: Treiber-Push Mailbox with Bounded Incremental Reversal** —
   a Treiber-stack LIFO producer push (single CAS, no publish gap, no
   transient Busy state) feeding a consumer that reverses each detached
   batch into FIFO order, chunked into resumable, bounded quanta so no
   single scheduler turn is monopolized past `R` pointer-steps. Fourth
   incarnation of a lineage (REX-CAS, ADR-001..004) already rejected
   three times for head-of-line tail latency.

## Evidence table

Legend: Survived = survived red-team cross-examination without being
withdrawn outright; Proven = executed C++ result from the prove phase.

### Design 1 — Intrusive Vyukov MPSC Mailbox

| Claim | Kind | Survived red-team? | Proven | Number / detail |
|---|---|---|---|---|
| F1 (0-alloc enqueue) | fast | yes (rescoped to `Mailbox::enqueue` only, excl. MessagePool) | **CORRECT** | 0 allocations, P=1..128, up to 128M ops; objdump: store+`xchg`+store+ret, 0 calls |
| F2 (0-RMW steady dequeue) | fast | yes | **CORRECT** | objdump: `xchg` confined to occupancy-1 stub re-arm branch only; multi-node path 0 RMW |
| F3 (≥3x throughput at P=4) | fast | yes (new bench authored) | **WRONG** | measured ratios 1.36x, 1.48x, 0.66x (clang regression!), 1.46x — falls well short of 3x |
| S1 (TSan/ASan clean) | safe | yes (scale bumped to 8M) | **CORRECT** | 0 reports, 8,000,000 ops, both compilers, TSan+ASan+UBSan |
| S2 (relaxed exec-state reopens race) | safe | yes | **CORRECT** | isolated harness: deterministic head_ race on first run when relaxed; Engine-level harness masked it (noted, not a counter-finding) |
| S3 (gen-gated cancel safe / bare-ptr UAF) | safe | yes (scale bumped to 8M) | **CORRECT** | 0 ASan reports real design; bare-pointer control reproduces UAF deterministically |
| S4 (packed CAS vs split-atomic TOCTOU) | safe | yes | **CORRECT** | packed: 0 stale writes; split: 2622–5412/8M stale writes |
| S5 (read-only close-out vs mutating variant) | safe | yes | **CORRECT** | shipped: 0/400,000 lost; mutating: 400,000/400,000 (100%) lost |
| S6 (Dekker seq_cst fence vs plain rel/acq) | safe | yes (quantitative % inconclusive) | **CORRECT** (qualitative) | shipped: 0/300,000 lost; weak: 32.8–39.2%/300,000 lost |
| C1 (100M-msg FIFO, no lost/dup) | correct | yes (scale bumped to 100M) | **CORRECT** | dup=0 missing=0 torn=0 fifo_violation=0 |
| C2 (tombstone exactly-once reclaim) | correct | yes (scale bumped to 8M) | **CORRECT** | handled+tombstoned==8,000,000, double_free=0 |
| C3 (ABA-safe under pool reincarnation) | correct | yes (new test authored) | **CORRECT** | 10M reincarnation cycles, 0 cycles/lost/dup-membership |
| C4 (Busy never misread as Empty) | correct | yes (new test authored) | **CORRECT** | 800,000/800,000 probes correctly Busy, 0 misreads |
| C5 (bounded tombstone-skip budget) | correct | yes (wall-clock criterion withdrawn) | **CORRECT** | exactly 1024 skips on a 10M-entry all-cancelled mailbox |

**13/14 proven correct, 1 proven wrong (F3, a fast/performance claim, not
safety or correctness). No safety or correctness claim failed. No core
invariant violated.**

### Design 2 — Segmented Linear-Chunk MPSC Ring (post red-team revision)

| Claim | Kind | Survived red-team? | Proven | Number / detail |
|---|---|---|---|---|
| F1r (p50≤250ns/p99≤600ns, P≤4) | fast | yes (rescoped from original 40ns, which was fatally wrong by ~5x) | **CORRECT** | P=4: p50=223.5–228.8ns, p99=434–511ns, 0 allocations |
| F2r (bounded allocation ladder / 0 further alloc once stabilized) | fast | partially (ladder sub-claim yes; "0 further alloc" sub-claim no) | **WRONG** | ladder (1→2→3→4→5 allocs at capacity boundaries) confirmed; but under **sustained throughput** with active draining, 19,531 *additional* allocations over 10M further enqueues (≈1 per 512 msgs) — **violates the "no heap allocation on steady-state hot path" ground-rule invariant**, because retired segments are never reused (linear, non-wraparound) |
| S1 (TSan/ASan clean incl. lazy-init/overflow sentinel race) | safe | yes | **CORRECT** | 0 reports, stall-injected across rollover, 400K×3 repeats |
| S2 (gen-gated cancel safe in ring slots) | safe | yes | **CORRECT** | 0 reports gated mode (8M); UAF reproduced in bare-pointer control |
| C1 (per-sender FIFO, distinct + oversubscribed) | correct | yes | **CORRECT** | 0 inversions, both regimes |
| C2 (no lost/dup handles) | correct | yes | **CORRECT** | 0 missing, 0 double_seen |
| C3 (Busy never misread at segment boundary) | correct | yes | **CORRECT** | 2000/2000 gated-window probes correctly Busy |
| C4 (bounded tombstone-skip budget) | correct | yes | **CORRECT** | exactly 1024 skips / 10M cancelled |

**7/8 proven correct, 1 proven wrong. The wrong claim (F2r) is a direct,
executed violation of one of the four MUST-hold ground-rule invariants
for this target ("no heap allocation on the steady-state hot path") —
this is not a mere missed performance target, it is a structural
consequence of the design's linear (non-wraparound) segment reclamation
policy: any sustained throughput keeps allocating a fresh segment
forever. Per ranking rule 4 ("a design that bends a core invariant does
not win"), this disqualifies design 2 regardless of its otherwise-clean
safety record.**

### Design 3 — REX-BIR: Treiber-Push Mailbox with Bounded Incremental Reversal

| Claim | Kind | Survived red-team? | Proven | Number / detail |
|---|---|---|---|---|
| F1 (0-alloc CAS-retry enqueue) | fast | yes | **CORRECT** | 0 allocations P=1..64; objdump confirms 1 load+1 store+1 `lock cmpxchg` |
| F2 (p99 ≥3x worse than Vyukov at P=16) | fast | yes (conceded as untestable at scale here) | **INCONCLUSIVE** | 7 trials at P=16 ranged 0.17x–4.11x, dominated by 4-core oversubscription noise; no stable trend |
| F5 (revised: 1 load+1 store tax, not "exactly 1 store") | fast | yes | **CORRECT** | objdump confirms 1 load+1 store vs Vyukov's 1 load |
| F3 (original: R-independent throughput deficit) | fast | **conceded/withdrawn during cross-exam** | — | conceded outright before the prove phase — chunk-boundary yields were unbudgeted, reproducing the O(N)-monopoly bug 002 already fixed once |
| F4 (no transient Busy state) | correct | yes (as corollary of S1) | **CORRECT** | proven inside S1's 8M-message forced-preemption harness |
| S1 (reversal walk safe under forced cross-worker resumption) | safe | yes | **CORRECT** | 0 reports at up to 8M msgs; positive control fires on gcc (not on clang — unresolved asymmetry) |
| S2 (ABA-safe under forced pool reuse) | safe | yes | **CORRECT** | 0 lost/dup at P=32, 8M msgs, 8-slot pool |
| S3 (gen-gated cancel unaffected by LIFO linkage) | safe | yes | **CORRECT** | 0 wrongly_cancelled/toctou at 8M, both negative controls fire |
| C1 (strict FIFO despite internal LIFO+reversal) | correct | yes (extended to forced cross-worker) | **CORRECT** | 0 inversions, 8M msgs, R∈{1,8,64} |
| C2 (no lost/dup across chunk-resume cycles) | correct | yes | **CORRECT** | produced==dispatched exactly, 0 duplicates |
| **C3 (deep-backlog tail latency still breaches 023's 50µs hard ceiling)** | correct | yes (mechanism sub-claim softened) | **CORRECT** | **p999 measured at 10x–600x over the 50µs ceiling in every configuration tested (513µs at B=1024 up to 29.7ms at B=262144), including with zero cross-actor contention (741µs at B=16384)** |

**9/10 proven correct, 1 inconclusive (F2), 1 withdrawn (F3). The
headline "proven correct" claim that matters most, C3, is the design's
own author predicting — and the executed evidence confirming — that
this design catastrophically fails the hard tail-latency budget. This
is the fourth time this lineage has been measured and rejected on
essentially this exact defect (ADR-001..004).**

## Decision

**Winner: Design 1 — Intrusive Vyukov MPSC Mailbox (Descriptor-is-node,
pooled, per-actor).**

Rationale, applying the stated ranking in order:

1. **Safety gate.** No design had a safety or correctness claim proven
   WRONG. Design 1 and design 3 both pass the gate cleanly (0 safety/
   correctness failures). Design 2's failure (F2r) is technically
   classified "fast," but its substance is a direct, executed violation
   of the explicit ground-rule invariant "no heap allocation on the
   steady-state hot path" — under sustained throughput it allocates
   forever, once every ~512 messages, because retired segments are
   never reclaimed for reuse. Per ranking rule 4 ("a design that bends
   a core invariant does not win"), **design 2 is disqualified.** No
   cheap fix is stated in the executed evidence — an active-reuse
   (hazard-pointer/epoch) reclamation scheme was explicitly named as
   unproven future work by the design's own author, not a stated cheap
   fix available today.

2. **Proven beats claimed.** Design 1 has the strongest surviving-and-
   proven record: 13/14 claims proven correct at the claimed (and in
   several cases re-scaled-up) evidence bar — 100,000,000-message FIFO
   with zero violations, 8,000,000-iteration cancel/reclaim races clean
   under all three sanitizers, 10,000,000-cycle ABA/pool-reincarnation
   check clean, exact-1024-skip budget enforcement. Design 3's single
   most consequential "proven correct" result (C3) is a proof that the
   design fails the workload it would need to serve at depth — its own
   author recommends against adopting it as a default mailbox, and
   this is the fourth executed round in which this exact lineage has
   been measured and rejected for essentially the same reason.

3. **Measured hot-path numbers among safe survivors.** Design 1 sustains
   8.2–28.6M enqueues/sec (single-to-four-producer, both compilers) with
   an enqueue path proven to compile to store+`xchg`+store+ret (zero
   calls, zero cmpxchg retry loop) and a steady-state dequeue proven to
   contain zero cross-core atomic RMW. Design 3, its only surviving
   competitor after the gate, does not have a usable head-to-head
   throughput number (F2 is INCONCLUSIVE due to this host's 4-core cap)
   and — decisively — has a proven, catastrophic p999 tail-latency
   defect (10x–600x over the 50µs hard ceiling from 023) that has no
   analogue in design 1's evidence.

4. **Core invariants.** Design 1 upholds all four: single executor
   (rides the exec-state CAS, proven load-bearing by S2's positive
   control), FIFO by default (proven at 100M scale), zero heap
   allocation on the steady-state hot path (proven at P=128), and zero
   data race/UB under TSan+ASan+UBSan (proven at 8M scale across six
   distinct safety properties). Neither competitor clears all four:
   design 2 bends the allocation-free invariant under sustained load;
   design 3 does not bend a bright-line invariant from this list, but
   its Treiber-push/bounded-reversal shape is fundamentally in tension
   with strict FIFO-without-latency-cost, which is the entire reason
   this lineage keeps failing the deep-backlog tail-latency bar tied to
   023's performance budget.

**One proven weakness in the winner, honestly weighed and not
disqualifying:** F3 (aggregate throughput scaling ≥3x at P=4 producers)
is proven WRONG — measured ratios ranged 0.66x–1.48x, including one
clang trial that regressed below P=1 throughput. This is a genuine gap
between the design's claimed contention behavior and reality, but it is
a "fast" claim about degree of scaling, not a safety/correctness/
invariant failure: absolute throughput at P=4 still reached 11.7–28.6M
enq/s across trials (i.e., contention does not collapse to a serialized
bottleneck, it simply doesn't scale as cleanly as first claimed). This
should be tracked as a follow-up investigation (see spec recommendation
for 002/023 below), not treated as a design-losing defect.

## Spec-update recommendations

**002-Scheduler.md**
- Formally document the memory-order contract for the `Running →
  Scheduled → Scheduled → Running` requeue/steal pair with the same
  rigor already given to `Idle ↔ Running` (explicit orders + a positive
  TSan control), since this round's cross-examination found this pair
  is presently *implied*, not specified, and is load-bearing for any
  design (including the incumbent Vyukov mailbox) whose consumer-private
  state must survive a mid-drain requeue-and-steal.
- Record F3's measured result (1.36x–1.48x aggregate throughput at P=4,
  one 0.66x regression observed) as a known, open contention-scaling gap
  for the shared `tail_` cache line; note it as a candidate follow-up
  investigation (e.g., padding/false-sharing audit, backoff strategy) —
  not a blocking defect, but currently a claim the spec should not
  repeat unqualified ("scales near-linearly").
- Note design 2's segment-boundary convoy/thundering-herd risk and
  design 3's proven tail-latency breach (C3: 10x–600x over the 50µs
  ceiling) as two now-executed, negative reference points for any future
  mailbox redesign proposal — both should be required reading before a
  new design is proposed against this same target.

**003-Memory.md**
- Promote the Vyukov Mailbox's data structure, memory orders, and
  hotPathSketch (as reproduced/executed in this ADR) from Draft to
  Accepted for the Mailbox MPSC hot path, citing this ADR (ADR-020) and
  its predecessors (ADR-001..004) as the executed-evidence trail.
- Add the debug-only tripwire discovered as a gap during cross-
  examination: `Descriptor::complete()`'s plain load+store on
  `gen_state` is safe today only by a single-writer-once-Running
  *convention*, with zero code-level guard against a future second
  legitimate writer. Recommend wrapping it in a debug-only
  `compare_exchange_weak` + assert-succeeds (zero release-build cost)
  so a future regression is caught by a test, not discovered by an
  incident — this was proposed during rebuttal but its own
  verification was not executed in this proof round (residual risk,
  below).
- Explicitly scope the "0-alloc enqueue" invariant to `Mailbox::enqueue`
  proper; add a companion note (or companion claim in a future review)
  that `MessagePool::acquire()`/`grow_one()` is a *distinct* subsystem
  whose own allocation-free-ness under producer/consumer imbalance is
  unproven and out of scope here — do not let readers infer "sending a
  message under load is 0-alloc" from this ADR alone.
- Record the two rejected/disqualified alternative shapes (segmented
  linear-chunk ring; Treiber-push with bounded incremental reversal) as
  named, evidence-backed rejected alternatives, with their specific
  proven failure modes (unbounded steady-state allocation under
  sustained throughput; catastrophic deep-backlog tail latency)
  so a future proposer does not have to re-litigate them from scratch.

**001-Actor-Execution-Model.md**
- No change to the single-executor invariant itself (proven intact
  across all three designs). Add a cross-reference note that the
  `Idle ↔ Running` release/acquire pair was re-verified this round via
  an isolated two-thread handoff harness (not just the full-Engine
  harness, which was shown in this round to *mask* a positive control
  due to RunQueue's own independent synchronization) — recommend this
  isolated-harness technique be the standard method for any future
  exec-state ordering claim, since the Engine-level harness alone gave
  a false negative on a genuinely load-bearing ordering.

**015-Reentrancy-and-Quiescence.md**
- Record the proven Busy-vs-Empty disambiguation behavior (C4: 800,000/
  800,000 probes correctly resolved) and the proven read-only-close-out-
  probe safety (S5: mutating variant loses 100% of contested messages)
  as the accepted mechanism for the Idle-race window; flag the
  documented-but-not-compiler-enforced precondition on
  `probe_has_work()` (only valid when `head_==tail_==&stub_`) as a
  residual fragility that a future refactor could silently violate —
  recommend adding a debug-mode assertion of that precondition at the
  single call site, not just a comment.

## Residual risks

- **ARM64/weak-memory correctness is still unproven by a real litmus
  test** (herd7/GenMC) for the winning design — all executed evidence
  in this and prior rounds is x86-64 only (GCC 14.2, Clang 20.1). CI
  disclosed a live, unexplained non-termination on arm64 release builds
  for an unrelated test (`topology_fifo_under_relay_test`), which is not
  direct counter-evidence against the Mailbox but is a live signal that
  this architecture still holds surprises for this codebase generally.
- **F3's throughput-scaling gap is real and unexplained**: 1.36x–1.48x
  measured aggregate gain at P=4 instead of the claimed ≥3x, including
  one clang trial that regressed below single-producer throughput
  (0.66x). Root cause (false sharing, NUMA effects, measurement noise,
  or a genuine algorithmic contention limit on the shared `tail_` line)
  was not diagnosed in this round.
- **`Descriptor::complete()`'s single-writer convention has no
  code-level tripwire** today; the debug-assert fix proposed during
  rebuttal was not itself executed/verified in this proof round.
- **`MessagePool::acquire()`'s allocation behavior under producer/
  consumer imbalance is unproven** — F1's 0-alloc claim is correctly
  scoped to `Mailbox::enqueue` alone, but a caller experiencing pool
  exhaustion under sustained P≫1 load with a slow consumer could still
  see a real allocation one level up the stack; this needs its own
  dedicated claim and proof round.
- **The newly-authored proof artifacts for this round** (8M/100M-scale
  mailbox tests, the ABA/pool-reincarnation test, the Busy-misread
  test, the Dekker wakeup litmus test, the new `mailbox_bench_mt.cpp`)
  exist only as this session's scratch artifacts, not yet merged into
  the repository's permanent CI-gated test suite — until they are
  committed at the scale used here, the "proven at scale" claims in
  this ADR cannot be automatically re-verified by `ctest` alone.
- **`perf`-based cache-miss correlation for F2 could not be executed**
  in this sandbox (`perf_event_paranoid` denies access) — F2's
  objdump-based zero-RMW proof stands on its own, but the cache-
  coherence-cost characterization of the steady-state dequeue is
  unconfirmed by hardware counters.

## Single tie-breaking experiment, if evidence had been insufficient

Not needed — the safety gate plus the executed proof phase were
decisive: design 2 fails the allocation-free invariant under sustained
load, design 3 fails 023's hard tail-latency ceiling by 10x–600x, and
design 1 is the only design with zero safety/correctness/invariant
failures. If a tie-breaker were still wanted, it would be: commit
`bench/mailbox_bench_mt.cpp` (authored this round) to CI and re-run F3
at P up to the shard's actual physical core count on production-class
hardware (not this sandbox's 4-core cap) to root-cause the 1.36x–1.48x
scaling gap before the next spec-promotion cycle.
