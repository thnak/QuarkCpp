# ADR-033: Mailbox MPSC Hot Path — Round 10 Judgment

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

This is round 10 of an ongoing design-debate series (ADR-001..004, ADR-020,
ADR-027, ADR-029, ADR-031, ADR-032 precede this one). Rounds 1-9 already
settled on the intrusive Vyukov mailbox as the shipped baseline, most
recently defending it in ADR-032 against a resident sequence-numbered ring
with overflow valve and a segmented Treiber-push/bounded-batch-reversal
design (SEG-REX) — neither dislodged it, but SEG-REX's bounded-segment
mechanism did achieve a flat p999 oldest-message-discovery latency, and
ADR-032 explicitly recommended a future round pair that mechanism with real
hazard-pointer/RCU reclamation instead of SEG-REX's ad hoc scheme. This
round ran two fresh candidates simultaneously, each through a full
architect -> red-team -> defense -> prove cycle: one (SEG-HP) directly
answers ADR-032's own recommendation; the other (DeliveryMode) abandons the
"make one strict-FIFO mailbox scale" premise entirely in favor of an
explicit, opt-in, per-caller policy tag.

## Design summaries

1. **Incumbent intrusive Vyukov MPSC Mailbox — pooled-descriptor hot path
   (Quark core, as shipped), unchanged this round.** The pooled `Descriptor`
   *is* the queue node (intrusive `MailNode` link at offset 0,
   pointer-interconvertible). Producers publish wait-free with one
   unconditional `tail_.exchange(acq_rel)` + one release link-store — no
   CAS, no retry loop, ABA-free by construction. The single consumer walks
   a consumer-private `head_` with plain loads; the one unavoidable RMW is
   confined to the empty-boundary stub re-arm. Vyukov's non-linearizable
   emptiness is surfaced as a first-class `Busy` result, never folded into
   `Empty`. This round produced no fresh executed evidence *against* the
   incumbent; its ADR-032 baseline numbers are reused for comparison only.
   This is exactly the code at `include/quark/core/{descriptor.hpp,
   mailbox.hpp,exec_state.hpp}`.

2. **SEG-HP — strict-FIFO, hazard-pointer/RCU-paired SEG-REX descendant.**
   A direct answer to ADR-032's own residual-risk recommendation:
   per-producer-shard (`ProducerShards<K>`) segmented mailbox, `SEG_CAP=256`
   slots/segment. The original architect draft used a shared `published_idx`
   counter and hazard pointers announced only at segment-creation; red-team
   *built and ran* a probe demonstrating two fatal bugs (a torn/stranded
   read under skewed producer completion order, sanitizer-invisible; a UAF
   window where the hazard pointer never protected the currently-active
   segment). The architect conceded both fully and shipped a REVISED
   design — per-slot atomic `Descriptor*` (replacing the shared counter) and
   a generation-gated packed `claim_state` word (`{generation:40, cap:12,
   claimed_idx:12}`, mirroring `Descriptor::gen_state`'s own pattern), plus
   a non-cached, always-re-derived-live force-seal CAS. The prover
   implemented and tested this REVISED design, not the dead original draft.

3. **`DeliveryMode<OrderFirst|ThroughputFirst<K>|LatencyFirst>` — a tiered,
   explicit-opt-in policy tag.** Structurally different from every prior
   challenger in this lineage: instead of trying to make one strict-FIFO
   mailbox scale past 2-4 producers, callers choose explicitly.
   `OrderFirst` is the byte-identical incumbent, unchanged, and the default.
   `ThroughputFirst<K>` is K independent, unmodified Vyukov mailbox shards —
   one per producer-thread-hash-lane — merged by a round-robin consumer
   scan, explicitly *relaxing* cross-producer-thread ordering (same-thread
   FIFO only is guaranteed). `LatencyFirst` is an unmodified single mailbox
   with smaller `DrainBudget`/higher `Priority` scheduling defaults,
   honestly disclosed as orthogonal to `tail_` contention and
   less-developed this round. Red-team found real, all-fixable gaps (a
   process-wide lane-assignment counter creating cross-actor coupling,
   missing K-power-of-two enforcement, missing cross-tag mutual-exclusion
   enforcement, an under-specified Busy-scan-short-circuit ambiguity); the
   architect conceded and fixed all of them with a per-actor-instance-scoped
   hash, registration-time `static_assert`s, and an explicit
   non-short-circuiting-scan invariant.

## Evidence table

Only claims that survived cross-examination and were then run as real,
compiled, sanitized C++ are listed. "Survived" = red-team round left it
standing (possibly revised). "Proven" = prover's executed verdict.

| Design | Claim | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|---|
| Incumbent (reused, ADR-032) | steady-state dequeue cost | fast | — | CORRECT (prior round) | 8.99-10.74 ns/op both compilers, not re-run this round |
| Incumbent (reused, ADR-032) | P=1->4 aggregate throughput under tail_ contention | fast | — | CORRECT (prior round, disclosed limitation) | 28.97->21.93 Mops/s gcc, 24.81->17.62 Mops/s clang; ~24-29% degradation |
| SEG-HP | F1 1-in-256 contended-RMW ratio | fast | yes | **CORRECT** | exact ratio confirmed via instrumented counters + objdump asm gate, both compilers |
| SEG-HP | F1b barrier-synchronized rotation-burst p999 (self-flagged "not assumed to pass") | fast | yes | **WRONG** | SEG-HP p999 91.9µs(gcc)/112.8µs(clang) vs incumbent 250ns(gcc)/286ns(clang) — ~370-400x worse |
| SEG-HP | F3 flat p999 oldest-message-discovery, array-indexed only | fast | yes | **CORRECT (narrow)** | reproduces SEG-REX's one real strength via direct array-indexed reads, no reversal walk at all |
| SEG-HP | F4/F4b bounded head-of-line stall + K-consecutive-idle-segment bound | correct | yes | **CORRECT** | force-seal bounds stall within the 023 50µs ceiling |
| SEG-HP | S1 hazard-scan is load-bearing for the ordinary claim path | safe | yes | **INCONCLUSIVE** | 0 TSan/ASan reports, but the required positive-control mutant (skip hazard scan) could not be made to fire |
| SEG-HP | S2(a) Attack-2 UAF scenario replayed directly | safe | yes | **CORRECT** | stalled thread resumes to bumped generation, self-heals via create_and_link_segment, 0 corruption |
| SEG-HP | S2(b) force-seal bounds "exactly that one segment" | safe | yes (revised claim) | **WRONG as characterized** | a stalled lane blocks a *different*, healthy lane for the full ~0.13-0.18s (20M-spin) measurement window via the single per-mailbox head_seg_ cursor — a full-mailbox stall, not scoped to one segment; recoverable, not corruption |
| SEG-HP | S2(c) busy-system incidental rescue | safe | yes (disclosed emergent, not designed) | **CORRECT (narrow)** | only when a second lane is actively rotating; degenerates to S2(b) otherwise |
| SEG-HP | S3 single-executor invariant untouched | safe | yes | **CORRECT** | max_concurrent_drainers=1, 150k cycles/3 workers, both compilers |
| SEG-HP | S4a relaxed-fence mutant detectable | safe | yes | **INCONCLUSIVE** | required positive control could not be made to fire — consistent with this project's documented "TSan can't see fence-only downgrades" limitation |
| SEG-HP | S4b packed claim_state CAS prevents TOCTOU | safe | yes | **CORRECT** | baseline 0/500 violations; split-atomics mutant fires 442-469/500 (~89-94%), both compilers |
| SEG-HP | S4c three separate reclamation litmus tests | safe | yes (partial, disclosed reuse) | **CORRECT (partial)** | reused S1/S4a's 1.2M-message stress evidence rather than three fresh dedicated litmus tests |
| SEG-HP | C1 per-lane strict FIFO incl. force-seal rotations | correct | yes | **CORRECT** | 240,000/240,000 delivered, 0 violations, both compilers (harness retried past thousands of the C4 spurious-Empty bug per run) |
| SEG-HP | C2 cross-lane order = seg_tail_ exchange MO (narrowed) | correct | yes (bound withdrawn) | **CORRECT** | 0 inversions in the narrowed claim; the "≤256-message divergence" bound is withdrawn |
| SEG-HP | C3 cancellation/tombstone unaffected by sharding | correct | yes | **CORRECT** | both compilers |
| SEG-HP | C4 Busy/Empty/mid-publish tri-state contract preserved | correct | n/a (new bug, found by prover) | **WRONG** | premature Empty at ordinary full-segment-rotation boundary: 195/200 (97.5%) isolated K=1 repro; 13,353/240,000 in one C1 multi-lane stress run — falsifies the reference tri-state contract every prior round treated as non-negotiable |
| DeliveryMode | F1 0 extra atomic RMW per enqueue steady-state | fast | yes | **CORRECT** | asm gate: shard's own tail_.exchange only; one-time per-thread nonce fetch_add ~45.65ns uncontended / ~177ns worst-case 4-thread-contended |
| DeliveryMode | F2 >=3x throughput at P=K=8 vs incumbent | fast | yes | **WRONG** | real hash-based assignment: 1.2x-1.5x mean; idealized force-disjoint control (0 collisions): 2.1x-2.7x, never reaching 3x — root-caused structurally to the single merge-consumer, reproduces across compilers/repeats |
| DeliveryMode | F2b single-fold avalanche hash diffusion for small sequential nonces | safe/fast | n/a (new bug, found by prover) | **WRONG** | ~5.76% worse lane spread for the first threads touching the process-wide counter (the common fixed-worker-pool-at-startup case), confirmed via zero-threading computation across 500 mailbox addresses — partially undercuts the architect's Attack-1 per-actor-scoped-hash fix |
| DeliveryMode | F3 O(1)/sub-linear drain scan cost vs K | fast | yes | **CORRECT (quantified caveat)** | dense traffic flat/O(1) both compilers; sparse traffic grows sub-linearly vs naive K-proportional prediction, both compilers |
| DeliveryMode | F4 additive-only, byte-identical baseline files | correct | yes | **CORRECT** | mailbox.hpp/descriptor.hpp/activation.hpp/exec_state.hpp/run_queue.hpp byte-identical; delivery_mode.hpp new standalone file |
| DeliveryMode | S1 full race/UB-free, all sub-claims | safe | yes | **CORRECT** | C1/C1b/C2/C3/C4/C5/C6 all clean, TSan + ASan/UBSan, both compilers |
| DeliveryMode | S2 K-shard lost-wakeup, fenced | safe | yes | **CORRECT** | new from-scratch harness: 10M rounds fenced = 0 lost, both compilers; fence-removed control loses 30/2M (gcc) / 5/2M (clang); TSan (as expected, fence-only) does not see the control's bug |
| DeliveryMode | S3 single-executor invariant untouched | safe | yes | **CORRECT (argument-based)** | not wired into activation.hpp's real dispatch this round; same standalone-before-integration methodology as prior rounds |
| DeliveryMode | S4 non-power-of-two K rejected at compile time | safe | yes | **CORRECT** | DeliveryMode<ThroughputFirst,6> fails to compile at the registration-path static_assert, both compilers |
| DeliveryMode | S5 cross-tag mutual exclusion enforced | safe | yes | **CORRECT** | declaring both ThroughputFirst and LatencyFirst on one actor is a compile error, both orders, both compilers |
| DeliveryMode | C1 per-producer-thread FIFO | correct | yes | **CORRECT** | 12 threads > K=8 lanes, 1.2M messages, 0 violations, both compilers/sanitizers |
| DeliveryMode | C1b cross-producer inversions genuinely observed (positive finding) | correct | yes | **CORRECT** | 49.99%-66.7% inversions across three runs — proves the relaxed contract is actually exercised, not accidentally total-order |
| DeliveryMode | C2 no dup/missing across shards | correct | yes | **CORRECT** | 2M messages, 4 threads, K=16, both sanitizers |
| DeliveryMode | C3 tombstone exactly-once unaffected by sharding | correct | yes | **CORRECT** | both compilers/sanitizers, all K shards + merge-consumer path |
| DeliveryMode | C4 Busy genuinely observable; merge never short-circuits | correct | yes | **CORRECT** | statistical: 0 sanitizer reports after prover fixed own harness bug; algorithmic: exhaustive proof for every K/Busy-position |
| DeliveryMode | C5 fairness bound exact (max gap = K-1) | correct | yes | **CORRECT** | exact for every K tested, both sanitizers |
| DeliveryMode | C6 idle-shard latency "must not shift materially" | correct/fast | yes | **WRONG** | 1.7x-2.8x p50 degradation on an idle shard when K-1 neighbors saturate — same root cause as F2 (shared merge-consumer) |

## Decision

**Winner: the incumbent intrusive Vyukov-style MPSC mailbox, exactly as
shipped in `include/quark/core/{mailbox.hpp,descriptor.hpp,exec_state.hpp,
activation.hpp}`, retains its undefeated status as the default mailbox —
round 10 and neither fresh candidate dislodges it.**

**SEG-HP is disqualified outright by rule 1, the safety/correctness gate.**
Its C4 claim — that the Busy/Empty/mid-publish tri-state contract holds,
the one reference correctness property every round since ADR-028 has
treated as non-negotiable for any mailbox redesign — came back **WRONG**,
via a genuinely new bug neither architect nor red-team anticipated:
`try_dequeue()` returns `Empty` at an *ordinary*, non-force-sealed
full-segment-rotation boundary even though the producer has already
committed guaranteed-forthcoming work (the gap between publishing a
segment's last slot and linking the next segment). This reproduced in
195/200 (97.5%) isolated trials and fired 13,353 times in a single
240,000-message multi-lane stress run. On top of that, F1b — a claim the
architect *itself* flagged as "not assumed to pass" — also came back
**WRONG**: SEG-HP's rotating-enqueue p999 is 91.9µs(gcc)/112.8µs(clang)
versus the incumbent's every-message p999 of 250ns(gcc)/286ns(clang), a
~370-400x regression in exactly the synchronized-contention regime the
redesign targets. Neither defect has a stated cheap fix — both are
structural (a publish/link ordering gap for C4; per-rotation RMW cost for
F1b) — so per rule 1, SEG-HP cannot win regardless of F3's real result.

**DeliveryMode clears the safety/correctness gate cleanly** — every one of
its `safe`/`correct` claims (S1-S5, C1-C5) came back **CORRECT**, including
a from-scratch K-shard lost-wakeup harness with a firing positive control
(S2: 0/10M lost fenced vs. 30/2M and 5/2M lost with the fence removed) and
an exhaustive proof that the merge scan never short-circuits past a real
`Busy` (C4). It is not disqualified by rule 1. But it does not dislodge the
incumbent either, on rule 3: its own central `fast` claim — F2, >=3x
throughput at P=K=8 — was proven **WRONG**. Real hash-based lane assignment
reaches only 1.2x-1.5x; even an *idealized* force-disjoint control (hash
bypassed, zero lane collisions) tops out at 2.1x-2.7x, never reaching the
claimed 3x. This is not a benchmark artifact: an isolated single-threaded
drain-only diagnostic shows the merge consumer alone can service
~100-170M msg/s (it is not itself slow), root-causing the ceiling to a
genuine structural fact — the single-executor invariant means exactly one
consumer thread still merges all K shards, and that consumer's finite
capacity to service real concurrent cross-core coherence traffic from K
live producers is the new bottleneck. A second claim, C6 ("must not shift
materially" for an idle shard's latency), was also proven **WRONG**:
1.7x-2.8x p50 degradation on an idle shard when K-1 neighbors are
saturated — the same root cause surfacing as a real, previously-undisclosed
cost. A third, F2b, surfaced a genuinely new, mild finding: the specified
single-fold avalanche hash is measurably under-diffused (~5.76% worse
spread) for small sequential nonces — exactly the common fixed-worker-pool
startup case — partially undercutting the very Attack-1 fix (per-actor
scoped hashing) the design's own red-team round required.

**Measured numbers for the winning (unchanged) design, reused from
ADR-032, for the record:** steady-state dequeue 8.99-10.74 ns/op
(gcc/clang), enqueue 1 xchg + 2 stores with 0 allocation, MPSC aggregate
enqueue throughput at P=1->4 degrading ~24-29% (28.97->21.93 Mops/s gcc,
24.81->17.62 Mops/s clang) — the same disclosed, unfixed limitation this
round's two candidates both attacked and both failed to remove without
introducing a new defect.

**Two narrower findings are banked, not merged as replacements for the
incumbent:**

1. **SEG-HP's F3** (flat, array-indexed O(1) oldest-message-discovery, no
   reversal walk at all) is a real, strict improvement over ADR-032's
   SEG-REX finding (which still required a bounded reversal walk). It is
   recorded as reusable evidence *only if* a future round pairs it with
   fixes to (a) the C4 premature-Empty bug at ordinary rotation boundaries
   and (b) the S2(b) single per-mailbox `head_seg_` cursor that lets one
   stalled lane globally stall every other lane — not as a reason to revive
   SEG-HP as specified.
2. **DeliveryMode's structural single-consumer-ceiling finding** (the root
   cause common to F2 and C6) is the single most valuable, durable result
   of this round: it *proves*, with an idealized zero-collision control
   ruling out hash-quality confounds, that any multi-shard-plus-single-
   consumer mailbox redesign is capped at roughly 2x-2.7x aggregate
   throughput under this codebase's fixed single-executor invariant — far
   short of near-linear scaling with shard count K. This resets the
   scoping bar for round 11. Separately, `DeliveryMode<ThroughputFirst<K>>`
   itself — fully safety-clean, additive-only (F4 CORRECT), and strictly
   opt-in (`OrderFirst` stays the byte-identical default) — is a plausible
   candidate for a **future implementation-adoption decision** (distinct
   from this design-debate ADR), contingent on restating its throughput
   claim honestly (1.2x-2.7x, not >=3x) and documenting the idle-shard
   latency cost (C6) as an accepted trade-off for callers who opt in.

No core invariant is bent by this decision: the incumbent is untouched: no
"safe"/"correct" claim of its own was contested this round, no ground
rule is violated by declining to adopt either challenger as the default,
and `DeliveryMode`'s design, on its own evidence, does not touch the
default hot path at all (F4).

## Residual risks

1. **ARM64/weak-memory proof gap remains open**, unchanged from ADR-031/032
   — all executed evidence this round, like the two before it, is
   x86_64-only.
2. **The `tail_`-contention gap is still open for the strict-FIFO default
   path, but its ceiling is now empirically bounded, not just
   theoretically assumed.** Round 10's most durable finding is negative:
   given the single-executor invariant, no multi-shard-plus-single-consumer
   design — however well-hashed — can be expected to exceed roughly
   2x-2.7x aggregate throughput (DeliveryMode's idealized, collision-free
   control). A future round targeting this gap for the *default* path must
   either accept this ceiling or change (or formally relax) the
   single-consumer merge itself, which is a materially larger architectural
   conversation than a mailbox redesign alone.
3. **SEG-HP's C4 bug is a new class of tri-state-contract violation**: a
   premature-Empty race at an *ordinary* (non-force-sealed) segment
   rotation boundary, distinct from every prior round's failure modes. Any
   future segmented-mailbox design must explicitly test the publish-then-
   link-next-segment gap, not just the force-seal path.
4. **SEG-HP's S2(b) mischaracterization is a real, uncorrected liveness
   gap** (not corruption): the design's single per-mailbox `head_seg_`
   cursor across all lanes means one stalled lane can stall every other
   lane for the full measurement window. A per-lane head cursor is an
   architecturally plausible fix but was neither implemented nor proven
   this round.
5. **SEG-HP's F1b and F3 pull in opposite directions** and are not yet
   reconciled: the same segmentation that makes oldest-message-discovery
   flat (F3) is where the catastrophic rotation-burst p999 regression
   (F1b) and the C4 premature-Empty bug both live. F3 is real but not
   currently detachable from the broken mechanics around it.
6. **DeliveryMode's F2b finding is real but unquantified in impact**: the
   ~5.76% worse hash spread for small sequential nonces has not been shown
   to translate to a measurable throughput or fairness effect at realistic
   K/worker-pool sizes — flagged for a follow-up, not yet a disqualifying
   defect on its own.
7. **DeliveryMode's S3 is argument-based only**, not wired into
   `activation.hpp`'s real per-actor dispatch path this round. A future
   round that wants to actually adopt `ThroughputFirst<K>` needs a fresh
   integration-level S3 test against the real engine, not the standalone
   harness used here.
8. **Two harness-methodology traps were caught by DeliveryMode's own
   prover in its own first-draft test code, before they could produce
   false verdicts** — worth recording as a durable, reusable methodological
   lesson (same spirit as this project's existing "TSan same-atomic
   blind spot" / "sanitizer-invisible" lessons): (a) a single-threaded
   strict-alternation S2 harness that could never exhibit a lost wakeup by
   construction, regardless of whether the design was correct; (b) a C4
   harness with two threads illegally calling `try_dequeue()` on the same
   shard concurrently — itself a violation of `Mailbox`'s own
   single-consumer precondition, caught by TSan as a real race. A harness
   that structurally cannot fail, or that itself violates the invariant
   under test, must be caught before its "clean" or "dirty" result is
   trusted.

## Spec-update recommendations

**`002-Scheduler.md`**
- Extend the "Mailbox hot-path baseline" section with round 10: two fresh
  challengers (SEG-HP, a hazard-pointer/RCU-paired segmented-Treiber-push
  descendant of ADR-032's SEG-REX; `DeliveryMode<OrderFirst|
  ThroughputFirst<K>|LatencyFirst>`, an explicit opt-in per-caller policy
  tag) both failed to dislodge the incumbent. SEG-HP was disqualified by a
  new tri-state-contract violation (a premature-`Empty` race at an
  ordinary, non-force-sealed segment-rotation boundary) plus a ~370-400x
  p999 regression under synchronized rotation-burst contention.
  DeliveryMode passed every safety/correctness gate but its central
  throughput claim (>=3x at P=K=8) fell to 1.2x-1.5x real / 2.1x-2.7x
  idealized-ceiling, root-caused to the single-executor invariant's
  single merge-consumer becoming the new bottleneck once the shared
  `tail_` line is removed.
- Record the new empirical ceiling as a standing scoping note: any future
  multi-shard-plus-single-consumer mailbox proposal attacking the P-scaling
  gap should be evaluated against a ~2x-2.7x realistic ceiling (proven via
  an idealized, collision-free control), not an assumed near-linear
  scaling with shard count K — cite ADR-033.
- Add a rejected-design line for SEG-HP (round 10) alongside the existing
  SBR-v5/SEG-REX (round 9) entries: disqualifying defect is the
  premature-`Empty` ordinary-rotation-boundary race plus the catastrophic
  rotation-burst tail-latency regression.
- Note `DeliveryMode<ThroughputFirst<K>>` as a safety-clean, additive-only,
  strictly opt-in candidate for a *future implementation-adoption*
  decision (not this design-debate's default), contingent on restating its
  throughput claim honestly and documenting the idle-shard latency cost.

**`003-Memory.md`**
- Add a round-10 entry to the "Rejected designs" appendix (alongside
  SBR-v5/SEG-REX): **SEG-HP** (per-slot-Descriptor*/generation-gated
  segmented Treiber-push, hazard-pointer/RCU-paired) — disqualified by a
  reproducible premature-`Empty` tri-state-contract violation at an
  ordinary segment-rotation boundary (195/200 isolated repro; 13,353/
  240,000 in stress) plus a ~370-400x rotation-burst p999 regression versus
  the incumbent. Do not re-attempt this specific rotation/reclamation
  mechanism without first fixing both defects.
- Add a durable methodological-lessons paragraph (alongside the existing
  TSan-same-atomic-blind-spot note): two harness-construction traps caught
  by this round's own prover in its own first-draft test code — a
  single-threaded strict-alternation lost-wakeup harness that can never
  fire regardless of correctness, and a concurrency harness that itself
  violated the mailbox's single-consumer precondition (two threads calling
  `try_dequeue()` on one shard concurrently) and was only caught because
  TSan flagged the harness's *own* bug. Any future mailbox-adjacent proof
  must verify the harness itself cannot structurally pass/fail
  independently of the design under test.
- Record `DeliveryMode`'s F2b finding (single-fold avalanche hash
  under-diffusion for small sequential nonces, ~5.76% worse lane spread)
  as a known caveat for any future thread-id/nonce-keyed sharding hash,
  particularly relevant to fixed-size worker pools spawned once at
  startup (the common case that touches the counter's low values first).

**`015-Reentrancy-and-Quiescence.md`**
- Add an "Update (ADR-033, r10 judgment)" block alongside the existing
  ADR-031/032 updates: SEG-HP's C4 finding is a new, permanent negative
  example for the standing "Busy must never be misread as Empty" rule —
  a design can violate the tri-state contract not only via its
  force-seal/reclamation path (as prior rounds probed) but at an
  *ordinary* segment-rotation boundary, where a producer has published its
  segment's last slot but not yet linked the next one. Cite the 195/200
  isolated repro and the 13,353/240,000 stress-test firing rate as the
  reference numbers.
- Record SEG-HP's F3 (array-indexed, no-reversal-walk flat p999
  oldest-message-discovery) as a strictly stronger version of ADR-032's
  SEG-REX finding, reusable only if a future round also fixes (a) the C4
  premature-Empty bug and (b) the single per-mailbox `head_seg_`
  cross-lane stall cursor (S2(b)) — not a reason to re-adopt SEG-HP as
  specified.
- Add `DeliveryMode`'s single-consumer-ceiling finding as a new standing
  scoping constraint for future mailbox-adjacent proposals: given the
  single-executor invariant this spec itself defines, a multi-shard
  redesign's aggregate throughput is structurally capped near 2x-2.7x
  (proven via an idealized, collision-free control, not merely observed
  under one hash function) — any future proposal claiming near-linear
  scaling with shard count K must explain what it changes about the single
  consumer, not just the producer side.

**`TAIL-CONTENTION.md`**
- Add an ADR-033 (round 10) entry to the "Status" section: two fresh
  candidates were tried — SEG-HP (a producer-side segmented redesign of
  the single mailbox, disqualified on tri-state-contract-violation and
  tail-latency grounds) and `DeliveryMode<ThroughputFirst<K>>` (an
  opt-in K-shard-plus-single-consumer design, safety-clean but
  structurally capped at 2.1x-2.7x by the single-executor invariant,
  confirmed via an idealized zero-collision control that rules out
  hash-quality as the limiting factor).
- Update the "Open" section: the question "how much can a producer-side
  redesign help at all, given the single-executor invariant" is now
  partially closed — round 10 supplies a proven ceiling (~2x-2.7x) for any
  design in the multi-shard-plus-single-consumer family, rather than
  leaving the achievable ceiling unbounded/unknown. The narrower question —
  whether a per-producer-cache-line design can be merged fairly by the
  *one* consumer at close to that ceiling, for the *default*, ordering-
  preserving FIFO path (not an opt-in relaxed-ordering tag) — remains open
  and unattempted as a fresh round.

## Tie-breaking / round 11 scoping experiment

Not needed to decide this round — the evidence is sufficient and the gate
is unambiguous for both challengers. For a future round that wants to
retry either idea:

- **SEG-HP retry**: fix (a) the C4 premature-`Empty` bug (make the
  sealed-segment branch of `try_dequeue()` re-check for an in-flight
  next-segment link before returning `Empty`, or publish the rotation
  atomically with the segment's last-slot publish) and (b) replace the
  single per-mailbox `head_seg_` cursor with a per-lane cursor so a
  stalled lane cannot stall its siblings (S2(b)). If both fixes land
  clean under the existing C1/C4 harnesses (240,000/240,000 delivered, 0
  premature-`Empty` events; S2(b)'s stall scoped to exactly the stalled
  lane, not the whole mailbox) *and* F1b's rotation-burst p999 comes back
  within budget, SEG-HP's F3 result becomes real, usable evidence toward
  closing the tail-contention gap on the strict-FIFO default path. Short
  of all three, the bounded-segment-reversal angle should be considered
  exhausted for the *default* path.
- **DeliveryMode follow-up**: this is now an implementation-adoption
  question, not a fresh design-debate round. Before landing
  `ThroughputFirst<K>` as an opt-in production feature: restate the
  throughput claim as 1.2x-2.7x (not >=3x), document the idle-shard
  latency cost (C6, 1.7x-2.8x p50) as an accepted trade-off, wire S3 into
  the real `activation.hpp` dispatch path for an integration-level test,
  and quantify whether F2b's hash-diffusion gap matters in practice at
  realistic worker-pool sizes.
