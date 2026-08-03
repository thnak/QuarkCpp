# ADR-039 — Ordered, reliable multi-subscriber fan-out primitive

Status: **Accepted** — `FanOut<M, Policy>` (N independent per-subscriber SPSC lanes), post S1/S2r
fixes.
Date: 2026-08-03
Deciders: design-debate-prove pass (quark-architect / quark-redteam / quark-prover / quark-judge)
Related: [ADR-018](ADR-018-outbound-streaming-replies.md) (`ReplyStream<F>`, single-consumer credit
backpressure — `StreamChannel<F>` reused verbatim here), [ADR-019](ADR-019-best-effort-broadcast-publish-primitive.md)
(`Topic<M>`, N-subscriber best-effort at-most-once — COW membership snapshot + bounded-quiescence
unsubscribe pattern, `SharedPayload<M>` pool, both reused verbatim here)
Specs touched: 006 (Messaging and Addressing), 017 (Delivery Guarantees), 022 (Resource Governance
and Overload Control), 003 (Memory)
Source: [github.com/thnak/QuarkCpp#10](https://github.com/thnak/QuarkCpp/issues/10) — downstream
request from AgentEngine

## Question

Quark has one ordered-but-single-consumer stream primitive (`ReplyStream<F>`, ADR-018) and one
N-subscriber-but-best-effort broadcast primitive (`Topic<M>`, ADR-019, which explicitly rules
layering reliability onto itself out of scope — GATE 1, publisher never stalls). Neither covers
**ordered + reliable delivery to N dynamically attaching/detaching subscribers**. Design a
CRTP-policy-parameterized primitive supporting both `OnSlowSubscriber::EvictAfter<N>` (bounded
per-subscriber buffer, then evict-with-gap-signal, producer never stalls) and
`OnSlowSubscriber::Block` (whole broadcast blocks on the slowest live subscriber) as zero-cost-unused
compile-time variants, and settle it with executed C++ proof, not argument.

## Designs debated

1. **`FanOut<M, Policy>` — N independent per-subscriber SPSC lanes.** One shared refcounted
   `SharedPayload<M>` per publish (ADR-019/003 pool, reused verbatim); one `StreamChannel<F>`-shaped
   ring per subscriber holding thin 16 B `FanOutEnvelope<M>{payload*, id}` descriptors (ADR-018's
   ring, reused verbatim, genuinely SPSC because this problem is single-producer); membership is
   ADR-019's `atomic<shared_ptr<const SubVec>>` COW snapshot + bounded-quiescence unsubscribe (GATE
   6, reused verbatim). `EvictAfter<N>`: `try_push` failing IS the eviction signal (no per-publish
   scan); `Block`: a new `push_blocking_while(F, const atomic<bool>&)` StreamChannel method parks the
   producer until the lane drains or departs.
2. **`EventLog<M, Policy, Cap>` — one shared append-only ring, independent subscriber cursors.** The
   ordered stream is stored exactly once (not once per subscriber); each subscriber holds only a
   monotonic read cursor (commit-log offset style). Per-slot readiness is a seqlock
   (`atomic<uint64_t> seq` gate + `atomic_ref`-chunked word copies) so publish touches no subscriber
   state directly under `EvictAfter` (pure lazy self-/reaper-detected eviction); `Block` caps
   `write_head_` at a lazily-rescanned min-cursor watermark.

## Decision: `FanOut<M, Policy>` wins

**`EventLog` is disqualified by the safety gate.** After the cross-examination round conceded and
supposedly fixed its original unwired-`in_flight` bug (`S1` → `S1-rev`), the prover re-ran the
S1-rev design *exactly as specified* under ASan and TSan and it still failed:

> `race_test_s1.cpp`, real-design mode: **heap-use-after-free on every one of 5/5 ASan runs** and
> **10/10 TSan data-race reports**, both pinned to `eventlog.hpp:149` (`in_flight.fetch_add` inside
> `publish()`'s wake loop) — a publisher thread that has loaded a stale COW snapshot but has not yet
> reached a doomed slot in its per-subscriber loop has not yet bumped that slot's `in_flight`, so
> `unsubscribe()`'s `in_flight==0` quiescence check can pass and free the slot while the publisher is
> still about to touch it.

The prover's root-cause note is explicit that this is not a one-line patch: the design needs
**per-snapshot epoch or hazard-pointer reclamation**, not per-element touch-time refcounting — a
different SMR mechanism, not a "cheap fix" as the judging rubric requires to waive disqualification.
This is the same failure *class* (a claimed-but-unwired SMR mechanism) that the design's own risk
list cites as having disqualified ADR-019's D-B for a proven UAF — it recurred here on the second
iteration of the exact primitive built to avoid it. Per ranking rule (1), a WRONG safety claim
without a stated cheap fix disqualifies the design regardless of its other proven claims (9 of
`EventLog`'s other 10 claims were independently proven CORRECT — see the evidence table — but GATE 1
is absolute).

**`FanOut<M, Policy>` cleared the safety gate and proved every surviving claim.** The original design
had one fatal, correctly-identified defect (`evict()`'s backlog-reclaim loop ran consumer-only
`StreamChannel` cursor operations on the producer thread, a second-writer violation that could
over-release a `SharedPayload<M>` shared with other subscribers — a cross-subscriber UAF). This was
conceded honestly in cross-examination and fixed by moving reclaim into `~LaneEntry()`, firing
exactly once and single-threaded only once every `shared_ptr<LaneEntry>` reference (membership
snapshot + the subscriber's own drain handle) is dropped — mirroring the already-proven
`BoundedInbox<M>::~BoundedInbox` idiom in `topic.hpp:199-204`, not a new mechanism. A second
compile-time defect (an ambiguous `push_blocking` overload) was independently reproduced with
clang++ and fixed by renaming to `push_blocking_while`. Every one of the seven claims that survived
red-teaming after these two fixes was then proven CORRECT by executed, sanitizer-clean C++ across
gcc and clang — see the evidence table.

## Evidence table

| Claim | Design | Kind | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|---|
| F1 — publish() latency flat under one pinned/dead lane | FanOut | fast | yes | **CORRECT** | N=64: p50 Δ −2.7%/+5.6%, p99 Δ +10.4%/−10.1% (gcc/clang); N=1024: p50 Δ +1.1%/+4.5%, p99 Δ +1.9%/−3.5%. Exactly 1 eviction at sample #256 in all 6 runs, no post-eviction growth. |
| F2 — zero cross-policy symbols, compile-time-only dispatch | FanOut | fast | yes | **CORRECT** | `nm -C`: `evict(` only in EvictAfter TUs, `push_blocking_while` only in Block TUs, 0 leakage either direction; no runtime policy discriminator field exists in the type at all. |
| S1 — subscribe/unsubscribe race-free under Block (renamed `push_blocking_while`) | FanOut | safe | yes (fixed compile ambiguity + reclaim bug) | **CORRECT** | TSan + ASan/UBSan, gcc+clang (4/4 builds) clean, 0 races/UAF. Firing control (remove departure-wake) reproducibly hangs in all 4 builds — load-bearing confirmed. |
| S2r — evict() backlog reclaim, exactly-once, no cross-subscriber UAF (revised: reclaim moved to `~LaneEntry()`) | FanOut | safe | yes (S2 original conceded as fatal; S2r replaces it) | **CORRECT** | TSan + ASan/UBSan, gcc+clang (4/4 builds) clean across ~3000 real eviction cycles under a genuinely *lagging* (not dead) consumer; pool returns exactly to warm size. Firing control (a) (reinstate old producer-side reclaim) reproducibly livelocks/hangs in all builds. Firing control (b) (skip destructor reclaim) **inconclusive** — harness never left a genuine backlog to reclaim. |
| C1 — per-(pub,sub) FIFO, 0 inversion/duplication while attached | FanOut | correct | yes | **CORRECT** | 0 inversions across 8 keep-up lanes + 1,069–1,417 real eviction cycles on a genuinely lagging lane. Sensitivity control (deliberate lane-swap) reliably produces 3,092 inversions — checker not vacuous. |
| C2 — EvictAfter gap signal exactly-once, never silent at the FanOut boundary | FanOut | correct | yes | **CORRECT** | `evicted_total_` == `gap_tell_attempts` == 5,000 exactly. Firing control (`-DQUARK_FANOUT_NO_GAP_TELL`) keeps `evicted_total_`==5,000 while tells drop to 0 — boundary-observability vs. mailbox-delivery cleanly distinguished. |
| C3 — Block delivers gap-free identical history; producer stall bounded by slowest live lane | FanOut | correct | yes | **CORRECT** | Slow-lane (100µs throttle) p50 ≈ 213–215µs vs. fast-baseline p50 ≈ 150–311ns (~700–1400×, tracking the throttle order of magnitude). Post-return assertion (`ring.head()` advanced past just-published id) held in all 3,000×4 checks. |
| S1-rev — subscribe/unsubscribe race-free (per-element `in_flight` SMR) | EventLog | safe | yes (conceded original, "fixed") | **WRONG** | ASan: heap-use-after-free 5/5 runs. TSan: 10/10 data-race reports, `eventlog.hpp:149`. Root cause needs epoch/hazard-pointer reclamation, not a cheap fix. **GATE — disqualifies EventLog.** |
| S2-rev — seqlock tear-detection is TSan-clean (narrowed, not abstract-machine-proven) | EventLog | safe | yes | CORRECT | 1M forced-torn-read attempts, 0 TSan reports, 0 torn values observed; memcpy firing control reliably reproduces TSan races. |
| S3 — exactly-once gap signal under self-detect/reaper race | EventLog | safe | yes | CORRECT | 100,000 rounds, 0/0 (silent/duplicate) both compilers; non-atomic-CAS firing control reproduces 5/100,000 duplicates. |
| C1-rev — FIFO incl. tail-start attach after wrap | EventLog | correct | yes | CORRECT | 10,000,000-item stress: 0 inversion/dup/gap across gcc/clang/ASan/TSan. |
| C2-rev — no double-accounting between drain() and reaper eviction | EventLog | correct | yes | CORRECT | 50,000 episodes, delivered-set and gap-range disjoint and complete in every episode. |
| C3-rev — Block bounded stall, no permanent deadlock on late attach | EventLog | correct | yes | CORRECT | Publish after late-attach returns within 200ms watchdog; resumes within 14–17ms of a paused lane draining, 0ms of it unsubscribing. |
| F1–F4-rev | EventLog | fast | yes | CORRECT | Dead/lagging p99 measurably *lower* than all-healthy p99 in every N_subs config; 64 B `[[no_unique_address]]` footprint delta confirmed. |

**Totals used for this decision:** FanOut — 7/7 survived claims proven CORRECT, 0 WRONG. EventLog —
9/10 survived claims proven CORRECT, **1/10 WRONG on a GATE-class safety claim with no cheap fix**.

## Rationale

Ranking rule (1) is dispositive: EventLog's S1-rev is a safety claim (subscribe/unsubscribe
race-freedom, no UAF), the prover marked it WRONG with a 100%-reproducible ASan crash and 10/10 TSan
races, and the prover's own analysis says the fix is a different SMR mechanism entirely — not a
cheap fix. That disqualifies EventLog outright regardless of its otherwise-strong 9/10 record
(including a materially better all-healthy-subscriber cache-line story and a smaller per-instance
memory footprint for large `sizeof(M)` workloads — real advantages, but moot once GATE 1 fires).
FanOut cleared the safety gate on both of its genuinely new mechanisms (the Block-policy
departure-wake and the EvictAfter reclaim path) after one honestly-conceded fatal defect each in
cross-examination, and then had every surviving claim proven CORRECT with sanitizer-clean, multi-run,
firing-control-validated executed evidence. No core invariant is bent: FanOut's single-producer
precondition matches the problem statement verbatim ("one producer's ordered event stream"), and
per-(producer,subscriber) FIFO falls out for free from each lane's genuine SPSC structure rather than
being separately engineered.

## Spec update recommendations

- **006-Messaging-and-Addressing.md** — Add `FanOut<M, Policy>` to the primitive catalogue alongside
  `ReplyStream<F>` (single-consumer, ADR-018) and `Topic<M>` (best-effort broadcast, ADR-019).
  Document its precondition explicitly as a messaging/addressing contract: `publish()` is
  single-producer only (exactly one thread/actor calls it per `FanOut` instance); multi-producer
  fan-in requires an upstream serializing actor, not something `FanOut` provides. Document
  `OnSlowSubscriber::EvictAfter<N>` / `OnSlowSubscriber::Block` as CRTP policy parameters resolved at
  compile time, in the same family as `Sequential`/`Priority<P>`/`DrainBudget<N>`.
- **017-Delivery-Guarantees.md** — Add `FanOut<M,Policy>` to the delivery-guarantee taxonomy: ordered
  + reliable per-(producer,subscriber) FIFO with zero gap/zero duplication while attached, distinct
  from `Topic<M>`'s at-most-once/silent-drop. `EvictAfter<N>` = bounded-lag reliable-until-evicted
  with an explicit, exactly-once, non-silent gap signal (`evicted_total_` always increments at the
  FanOut boundary — document that this is *not* an end-to-end delivery guarantee to the subscriber's
  own mailbox, whose own backpressure policy is a separate, still-open 006 concern). `Block` = fully
  reliable/gap-free at the cost of a producer stall bounded by the slowest *live* subscriber, not a
  bounded-*time* guarantee — a subscriber that stays attached but never drains stalls the producer
  indefinitely by design; document this as requiring an external liveness backstop (007 supervision /
  deadline-based forced unsubscribe), not an internal guarantee.
- **022-Resource-Governance-and-Overload-Control.md** — Document the per-lane memory footprint model:
  N subscribers × ring capacity × `sizeof(FanOutEnvelope<M>)` (16 B descriptor, not the payload)
  resident per `FanOut` instance, pre-allocated cold at `subscribe()`. Document subscribe/unsubscribe/
  evict as O(N_subs) cold-path COW-vector rebuilds (mutex-guarded, never touching the publish hot
  path) — the same acknowledged high-churn/large-N limitation class as ADR-019. Document `Block`'s
  producer-stall-bounded-by-slowest-live-subscriber as a liveness property requiring an external
  ceiling (per 017 above), consistent with 022's posture that no primitive should assume an unbounded
  internal wait is externally safe without a stated backstop.
- **003-Memory.md** — Document the reclaim-ownership rule this ADR's fix establishes as normative for
  any future `StreamChannel<F>`-composing design: only the ring's single designated drainer thread, or
  its owning `shared_ptr`'s destructor once uniquely referenced, may call `StreamChannel`'s
  consumer-only cursor API (`peek`/`advance_dispatch`/`advance_tail`) — a second thread (e.g. a
  producer reaching in to reclaim a departed subscriber's backlog) reaching into those cursors is a
  confirmed second-writer violation. Document the `~LaneEntry()`-gated-by-refcount reclaim idiom
  (mirroring `BoundedInbox<M>::~BoundedInbox`, `topic.hpp:199-204`) as the correct pattern. Add
  `StreamChannel<F>::push_blocking_while(const F&, const std::atomic<bool>& keep_going)` as a new,
  distinctly-named method (not an overload of `push_blocking`, which is a confirmed ambiguity) for
  departure-aware blocking pushes.

## Residual risks

- Single-producer precondition is load-bearing, not incidental — a caller that fans multiple producer
  threads into one `FanOut` instance is unsupported and would need an external serializing layer;
  this is not tested or bounded here.
- `OnSlowSubscriber::Block`'s stall bound is conditioned on subscriber liveness, not time — a live but
  permanently non-draining subscriber stalls the producer forever by design. No supervision/deadline
  integration (007-style forced unsubscribe of a catatonic subscriber) has been built or proven yet;
  this is a required companion, not an assumed-away edge case.
- All ordering arguments (release/acquire retain/release, the reverse-Dekker departure-wake fence) are
  proven on x86-TSO only, in a WSL2-virtualized environment with materially higher noise (~5–10% p99)
  than the ADR-019 bare-metal precedent (~2%); ARM64/weak-memory needs its own herd7/GenMC litmus pass,
  and F1's numbers should be re-validated on a bare-metal runner before treating this pass's noise
  band as the operating bar.
- S2r's firing control (b) (skip the `~LaneEntry()` reclaim walk) was **inconclusive** — the test
  harness's own safety-net drain sweep always emptied the ring before the handle was dropped, so no
  genuine leftover backlog ever existed for the destructor to fail to reclaim. A harness that stops
  draining abruptly at the instant of eviction is needed to fully certify exactly-once reclaim across
  all backlog shapes before this claim is treated as maximally load-bearing.
- GCC's `-fsanitize=thread` emitted a "standalone fence not supported" warning for the reverse-Dekker
  `seq_cst` fence used in `poll_unstall`/`push_blocking_while`; all TSan runs were clean despite the
  warning, but this is a documented TSan blind spot on this specific synchronization edge and should
  be tracked, not silently trusted.
- Cold-path O(N_subs) COW rebuild cost on subscribe/unsubscribe/evict is inherited from ADR-019 and
  unmeasured here for high-churn/large-N deployments.
- Per-lane ring memory footprint (capacity × `sizeof(envelope)` × N_subs) needs explicit
  capacity-planning guidance in 022 before production use; not tuned or measured against real
  workloads in this pass.
- `EventLog`'s shared-single-ring architecture is not without merit (better all-healthy cache-line
  behavior, smaller per-instance footprint for large `sizeof(M)`) and remains a candidate worth
  revisiting as its own future ADR *if* its SMR is rebuilt on proper epoch/hazard-pointer reclamation
  — it should not be read as a dead end, only as disqualified in this pass as specified.

## Implementation note (post-decision, 2026-08-03)

`FanOut<M, Policy>` shipped directly from this decision (no re-run through
design-debate-prove): `include/quark/core/fanout.hpp`, plus
`push_blocking_while`/`notify_departure` added to `include/quark/core/stream_channel.hpp`.
Two divergences from this ADR's prose, both narrowing rather than changing the design:

- **Policy spelling.** The shipped tag is `OnSlowSubscriber<EvictAfter<N>>` /
  `OnSlowSubscriber<Block>` (the flat-tag-wrapper idiom matching `OnRestartAsk<Mode>`/
  `OnResourceFailure<Mode>`, 007/ADR-009), not the `OnSlowSubscriber::EvictAfter<N>`
  scoped-name spelling used in this ADR's prose. Same compile-time-only dispatch,
  same zero-cost-unused property (F2) — naming only.
- **A real bug the design-debate-prove harness had not exercised.** `LaneEntry::try_pop()`
  must call `channel_.poll_unstall()` on every pop. Without it, a `Block`-policy producer
  parked against a lane that is merely *slow* (never departs) is woken **only** by
  `unsubscribe()`'s departure-wake — ordinary draining making room never wakes it, so the
  producer hangs forever the first time a live, still-attached subscriber actually causes a
  stall (exactly the scenario C3 claims to cover: "producer stall bounded by slowest LIVE
  subscriber"). Caught by `tests/fanout_block_test.cpp` §(A) (a slow-but-never-departing
  drainer) hanging past a bounded timeout before the fix; clean after. This means the
  harness's S1/C3 evidence exercised the departure-wake path but not the plain
  credit-return-wake path under a genuinely slow (not dead) subscriber — a gap in coverage,
  not a wrong verdict on what it did test.

Evidence for the shipped code: `tests/fanout_evict_after_test.cpp` (C1/C2),
`tests/fanout_block_test.cpp` (C3, S1 departure-wake), `tests/fanout_subscribe_race_test.cpp`
(S1/S2r under concurrency — single-producer + churn thread, per this ADR's own single-producer
precondition), `tests/fanout_payload_reclaim_test.cpp` (GATE-4-style exactly-once reclaim
across evict/drop/`~LaneEntry()`-backlog paths). Clean under the normal clang build and under
ASan+UBSan (RelWithDebInfo, this session's Windows recipe — see ADR-009's implementation note),
alongside the pre-existing `topic_*`/`stream_*`/`reply_stream_*` tests (no regression from the
`push_blocking_while`/`notify_departure` addition to `StreamChannel<F>`). **TSan did not run**:
clang on this box rejects `-fsanitize=thread` outright for the `x86_64-pc-windows-msvc` target
(`error: unsupported option '-fsanitize=thread' for target …`) — a toolchain/target limitation
of this Windows dev environment, not a skipped choice. The TSan pass this ADR's own evidence
table relies on for S1/S2r-class claims is still owed on a Linux runner before those claims are
treated as re-confirmed for the shipped code, not just for the design-debate-prove harness.

## Tie-breaking experiment (if EventLog is revisited)

Not needed for this decision — the evidence gate resolved it outright. If `EventLog` is proposed
again, the single experiment that would need to re-run before any other claim is trusted is S1 itself
with a genuine snapshot-scoped SMR (epoch counter incremented once per `publish()` call covering the
whole per-subscriber loop, or hazard pointers per touched slot) substituted for the disproven
per-element `in_flight` scheme, under the identical ASan+TSan churn harness that found the bug.
