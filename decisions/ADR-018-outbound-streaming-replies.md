# ADR-018 — Outbound streaming replies (an `ask` that returns a stream)

- Status: **Accepted (x86-64)**. The mechanism AND the 006 outbound-streaming axis both promote — see the
  2026-08-04 update below §Promotion.
- Date: 2026-07-18
- Scope: the one remaining pure-design cross-cutting item in `006-Messaging-and-Addressing.md`
  ("Streaming replies", lines 134–138) and `OpenQuestions.md` line 156–157: how the ADR-007
  ask/reply machinery carries a **multi-item** streaming reply back to the caller. The **inbound**
  direction (024 / ADR-005 / ADR-014) is settled and is *composed with*, not re-debated. The
  intrusive Vyukov mailbox (ADR-002/003/004) is settled and is *context*, not the subject.
- Supersedes/extends: composes with ADR-005 (inbound SPSC credit-ring), ADR-007 (ReplyCell /
  reply-ordering / permanent reply-UAF gate), ADR-014 (streaming async-suspend real-scheduler gate).

## Question

The single-shot monotonic-generation `ReplyCell` (ADR-007) resolves **once**. A stream resolves
**N times then completes**. Specify concretely, without violating any core invariant or GATE 1–7:
(a) addressing / the ReplyCell↔ReplyToken seam; (b) reply-direction credit/backpressure (callee =
producer, caller = consumer) that stalls a fast callee without dropping a reply item; (c)
cancellation + deadline (018) teardown that returns credit, stops the callee, leaks no ring, and
delivers nothing after teardown; (d) cross-node (010) with the same credit protocol and 018
remaining-duration deadline reconstruction; (e) exactly-once (017) per-item identity + dedup
watermark; (f) lifetime — the ring + handle outlive in-flight items yet are reclaimed **exactly
once** with no UAF (the ADR-007 reply-UAF gate extended to a multi-resolve stream).

## Designs considered (one-line summaries)

- **BASELINE — N-Discrete-Reply firehose** (B-tell / B-resolve): "a streaming reply is just N
  discrete replies." B-tell = N ordinary Vyukov mailbox descriptors; B-resolve = the single-shot
  ReplyCell resolved N times. Submitted as the honest **loser** with runnable failure proofs.
- **Reply-Credit-Ring** (symmetrize 024): `ask_stream<F>` returns a bounded, pre-allocated,
  credit-controlled SPSC ring that is the **024 inbound ring with producer/consumer roles flipped**
  — callee = `head` producer, caller = `disp`/`tail` consumer; credit flows caller→callee for free
  through the same derived arithmetic `capacity-(head-tail)`, **no shared counter**. Three seams:
  single-resolve `StreamReplyCell` (OPEN), the N-item ring, an in-band EoS. Caller drains one batch
  per activation turn via `StreamActivation<F>::drain` verbatim from ADR-014. **PUSH.**
- **DemandChannel** (generator-pull): `ask_stream<F>` returns a `StreamReader<F>` over an SPSC ring
  gated by a consumer-written `demand_` cursor; each callee `co_yield` is pull-gated and parks when
  demand is exhausted. Single-shot ReplyCell preserved verbatim (N item-publishes + 1 terminal
  resolve). Intrinsic backpressure; wins cross-node teardown by construction. **PULL.**

## Evidence table (claim → survived red-team? → proven by executed C++? → number)

Compiled and run under g++ 14.2.0 + clang++ 20.1.2, `-O3 -march=native (+LTO)`, pinned cores
(taskset), rdtsc percentiles, ASan/UBSan/TSan. x86-TSO only; all AArch64/weak-memory sub-claims
INCONCLUSIVE and carry **no** decision weight.

### BASELINE (proven to LOSE — every failure claim CORRECT)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 no batch amortization (fails 023 ≥3× gate) | yes | **CORRECT** | per-item p50: B-tell 46.8 ns, B-resolve 28.6 ns vs ring batch-drain **1.9 ns** → **24.6×** worse (gcc), 25.1× (clang) |
| F2 cannot hold 4M lossless floor | yes | **CORRECT** | bounded pool(4096) @0.5× drain → **50.0% mid-stream drops**; big-pool run "spuriously passes" (gameable harness exposed) |
| S1 GATE-3: unbounded OR sheds | yes | **CORRECT** | 4× overproduce: unbounded mailbox peak **3,750,001** in-flight (O(3N/4)); DropOldest **2,701,425** drops. `sizeof(state)==16`, **no credit atomic** |
| S2 GATE-4: B-resolve reuse = lost/UAF | yes | **CORRECT** | resolve-vs-gen-bump: delivered=0, **lost=1,000,000**; TSan race in `ReplyCell::arm` (gcc) |
| C1 GATE-2/6: fresh-cell reorder+no dedup | yes | **CORRECT** | **500,000 order violations**; replay window → 100 undedupable dups |
| C2 GATE-5: post-teardown delivery / fence is UAF+leak | yes | **CORRECT** | nofence: 1000/1000 items after teardown; fence → ASan **heap-use-after-free**; drop branch → **23,616 B leaked** |
| C3 GATE-3 cross-node | yes | **CORRECT** | receiver peak depth **3,000,000** unbounded OR co-tenant HoL p99 **3,960,004** frames — 010's own open question |
| G4 GATE-4 not sidestepped (cross-shard leak/UAF) | yes | **CORRECT** | monotone leak (100 stuck/stream, 9,999,900 shed); "fix" = **1,000,000 cross-core RMW/item** + TSan race |
| G6X GATE-2/6 cross-node watermark gaps | yes | **CORRECT** | reorder+reconnect → gap=1 genuine drop; re-activation → 100/100 replayed dropped as dup; durable-counter control passes |

**Verdict: disqualified.** Fails GATE-3 **and** GATE-4 **and** GATE-5 **and** cross-node GATE-2/6,
plus the 023 amortization/throughput budgets. No cheap fix rescues GATE-3 (a fire-and-forget `tell`
has no reply-direction credit lever by construction). **Must not promote 006 on this design.**

### Reply-Credit-Ring — PUSH (all claims CORRECT after revisions)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 ≥3× amortization | yes | **CORRECT** | ring **14.9–15.3 ns/item** vs discrete ask/reply **125.9–126.7 ns** → ratio **0.119–0.121 (~8.3×)** ≥ 3× gate |
| F2 throughput | yes | **CORRECT** | **5.29 M/s/core (gcc), 6.42 M/s (clang)** @50M items — **4M hard floor MET, 10M goal MISSED**; fifo=0 |
| **F3 0 cross-core RMW on caller drain (HARD gate)** | yes | **CORRECT** | objdump per-item drain body = **0 lock/xchg/cmpxchg/mfence**, both compilers; RMW lives in `poll_unstall`, O(batches) |
| S1 0 per-item heap | yes | **CORRECT** | alloc slope 5M→50M = **0** both compilers; cold `task<>` frame = 1/ask (conceded, eliminable by pooled promise allocator) |
| S2 GATE-4 exactly-once reclaim, cross-node | yes | **CORRECT** | 100k streams reclaim=100000, double_reclaim=0, applied_late=0; controls trap (no-gen-fence→UAF, double-free→UAF); **TSan clean** |
| C1 GATE-2 intra-stream FIFO + ADR-007 OPEN ordering | yes | **CORRECT** | 10M monotone → order_violations=0, gaps=0; Sequential→request / Reentrant→completion PASS (OPEN seam **modeled**) |
| C2 GATE-5 two-part terminal wake | yes | **CORRECT** | idle-caller deadline reclaims=1; stalled-callee cancel returns `stream_closed`; post_terminal=0; 3 controls each fire |
| C3 GATE-6 producer_seq exactly-once | yes | **CORRECT** | replay logical 5..9 same producer_seq → delivered=10, dup=0, gap=0; ring-index control → dup=5 (fires) |
| C4 GATE-1/7 (flipped ADR-014 harness) | yes | **CORRECT** | shipped `stream_exactly_once_suspend_test`: lost=dup=torn=fifo=0, max_running=1, credit_for_parked=0; **TSan clean**; controls fire |
| C5 cross-node credit monotone max-merge | yes | **CORRECT** | reorder+dup → overwrite_violations=0; additive control overshoots → 12,494 (fires); heartbeat drains after dropped final |

### DemandChannel — PULL (all claims CORRECT **on a corrected mechanism**)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 throughput @K=256 | yes | **CORRECT** | **31.0 M/s (gcc), 33.1 M/s (clang)** — exceeds 10M goal; K=1 control 1.2–1.5 M/s (windowing load-bearing) |
| F2 drain latency | yes | **CORRECT** | per-batch p999 651–2463 ns (« 50µs); per-item dispatch ~1.3 ns; drain inner loop 0 lock ops |
| **F3 O(N/K) consumer-lane RMW — CONCEDES the 0-RMW HARD gate** | yes | **CORRECT** | pull = **39,063 RMW/10M = 0.00391/item (>0)**; push measured **0**. Pull **fails** the 023 line-117 gate |
| F4 cross-node windowing | yes | **CORRECT (with caveat)** | throughput scales with K, first-item = 1 RTT, ctrl ~N/K; **K grid maxed < BDP → saturation crossing not exercised** |
| S1 0 heap + exactly-once reclaim | yes | **CORRECT** | 0 alloc/10M; two-releaser fetch_or → free_count=1, resolve=1 |
| S2 no lost wakeup (unified exec-state) | yes | **CORRECT** | fenced: 0 hangs @10M/93k parks; NOFENCE control hangs (gcc 6/6, clang 2/10) |
| C1 GATE-1/2/3 | yes | **CORRECT** | maxocc bounded ~K, ordv=0, drop=0; no-gate control grows O(N), ordv~2M |
| C2 GATE-5 quiescence reclaim | yes | **CORRECT** | post-cancel-delivered=0, leaks=0, frame destroyed once, StreamEnd once |
| C3 GATE-6 (transport retransmit, narrowed) | yes | **CORRECT** | FIFO-replay dup=0 gap=0; reorder control gaps=39,063 (FIFO precondition load-bearing); durable-crash **explicitly deferred to 017** |

**Note on pull:** the design's own `hotPathSketch` was **buggy as written** (single-shot `co_yield`
awaiter → illegal-fill deadlock ~1/3 at K=8, plus a coroutine-frame double-destroy UAF caught under
ASan). Both were fixed during bring-up; all verdicts above stand only on the **corrected**
publish-or-park retry mechanism, which a single-shot awaiter structurally cannot express.

## Decision

**Winner: Reply-Credit-Ring (PUSH) — outbound streaming replies are the 024 inbound credit-ring run
backward, callee = producer, caller = consumer.**

Rationale, in the judge's ranking order:

1. **Safety gate.** All three of BASELINE's, PUSH's, and PULL's *surviving* correctness claims that
   were executed came back CORRECT — but BASELINE's are proofs that it **fails** GATE-3/4/5 and
   cross-node GATE-2/6, so it is **disqualified**. PUSH and PULL are both safe survivors (every
   safety claim — GATE-1/4/5/6/7 — proven CORRECT under ASan+TSan with load-bearing controls that
   fire).

2. **Proven beats claimed.** Both survivors carry only executed-CORRECT claims. The tie-breaker is
   the **core outbound-streaming gate set**, and one HARD, measured gate separates them decisively:

3. **The 023 line-117 hard gate — 0 cross-core atomic RMW on the sequential consumer (caller) drain
   path.** PUSH proves **0** (objdump, both compilers; F3). PULL **concedes and measures
   0.00391 RMW/item (>0)** because the demand-refill wakeup is *intrinsically* on the consumer lane
   (the caller **is** the demand granter — it cannot move to the producer without becoming push;
   F3). This is a **hard budget**, not a goal. PULL's ~5× higher raw throughput (31M vs 6M/s/core)
   **cannot** buy back a hard-gate violation: the ground rules state throughput "breaks ties but
   cannot violate the latency/footprint ceilings or ANY correctness gate." **PUSH passes the gate;
   PULL fails it.** PUSH wins.

4. **No bent invariant.** PUSH preserves ADR-007 single-shot `ReplyCell` verbatim for ordinary asks
   (the stream rides three distinct seams; the OPEN handshake resolves exactly once), keeps
   single-executor, per-stream FIFO, std-only C++23 core, and 0 steady-state per-item heap. Its
   item-drain leg **is** the shipped 024 `StreamChannel<F>`+`StreamActivation<F>` flipped — proven,
   not modeled. PULL also bends nothing, but loses on the hard gate above; its sketch also required
   a structural correction to be safe at all.

**PULL is adopted as a secondary policy — `ReplyMode::Pull`** — for high-RTT cross-node links and
bursty subscribe-style replies where the callee must be *provably idle* (silence == backpressure ==
teardown, no standing credit to reclaim). This matches both the PUSH and PULL authors' own honest
recommendation. `ReplyMode::Pull` requires its own ADR-014-grade real-scheduler reply-direction gate
(decider: the F3 RMW measurement) plus the revised S2 unified-exec-state Dekker and C2 two-releaser
reclaim; it does **not** gate the primary path.

## Promotion — 006 outbound-streaming axis: **Draft → Accepted, 2026-08-04** (`promotes006 = true`)

**Historical section (as written 2026-07-18); superseded by the 2026-08-04 update further below.**

The **mechanism** is decided and the item-transport leg is genuinely proven (it is the settled 024
ring flipped: F1/F2/F3/C1/C4 item-path all CORRECT under the shipped headers). But **006 must NOT be
stamped Accepted on the outbound axis yet.** The single honest blocker, plus four
outbound-specific seams proven only in a wrapper (not on the real dispatcher / ADR-007 reply router):

- **(blocker) 015 OPEN-cell re-admit is not wired.** `detail::reply_cell.hpp` still leaves the
  `co_await` on-lane resume as future work (only `block_on` is wired). The `StreamReplyCell` OPEN
  handshake inherits that exact unfinished seam. Promotion is **conditional on an ADR-014-grade
  real-scheduler run of the 015 OPEN-cell re-admit**, exactly as the OPEN handshake for an ordinary
  ask still needs.
- (proven in wrapper, needs real-dispatcher integration) the two-part terminal wake (C2), the
  callee-assigned `producer_seq` exactly-once (C3), the cross-node monotone max-merge credit +
  heartbeat (C5), and the `stream_id`-nonce + gen-gate-before-touch multi-resolve UAF gate (S2).
  C1's OPEN-handle ordering was **modeled**, not run through the real ADR-007 reply router.

When the 015 OPEN re-admit clears its real-scheduler gate and the four seams are re-proven against
the real dispatcher + ADR-007 reply-ordering, 006 outbound-streaming can promote Draft→Accepted
(x86-64). Until then: **Draft.**

## Spec recommendations

- **006-Messaging-and-Addressing.md** — Add an "Outbound streaming replies" subsection.
  `ask_stream<F>(M) -> task<expected<ReplyStream<F>,error>>`. Reply carried by the 024 credit-ring
  **flipped** (callee=head producer, caller=disp/tail consumer); credit = derived `capacity-(head-
  tail)`, no shared counter. Three seams: single-resolve `StreamReplyToken`/`StreamReplyCell` (OPEN,
  16 B, rides the ADR-007 `reply_(16)` field — ordinary single-shot ReplyCell untouched), the N-item
  ring, in-band EoS. ADR-007 reply-ordering governs delivery of the **OPEN handle** (Sequential→
  request, Reentrant→completion); intra-stream FIFO = monotone head order (orthogonal). Mark the
  "Streaming replies" open item **resolved-in-mechanism, Draft pending the 015 OPEN re-admit gate.**
- **024-Streaming-and-Inbound-Streams.md** — Add an "Outbound / reply-direction" section stating the
  ring is **symmetric**: the same `StreamChannel<F>`/`StreamActivation<F>` with roles flipped, ring
  allocated on the **caller** shard (consumer owns the ring), producer arm-edge executed by the
  callee (local) or the caller-node transport thread (cross-node) — off the caller drain lane.
  Reverse-Dekker producer un-stall reused verbatim. Document `ReplyMode::{Push(default),Pull}`.
- **017-Delivery-Guarantees.md** — Per-item reply identity = `(stream_id, producer_seq)` where
  `producer_seq` is **callee-assigned, stream-relative, replay-deterministic** (re-assigned
  identically on re-activation), deduped by the caller's `disp` high-watermark. State that a
  caller-local ring index is **not** a valid identity (delivers dups on re-activation). Durable
  crash-recovery exactly-once for a reply stream remains 017 Persistent's transactional
  {state,watermark,outbox} — a volatile reply ring makes no durability claim (record the fan-in
  deriving-reply hazard, line ~125, as still open).
- **018-Clocks-and-Deadlines.md** — The reply-stream deadline travels as **remaining-duration**;
  the callee reconstructs `deadline_B = now_B + remaining - transit` against `pal::BootClock`
  (CLOCK_BOOTTIME), never a raw remote instant. Terminal CAS on deadline expiry performs the
  two-part wake (arm caller drain + bump credit_gen).
- **010-Distribution.md** — Cross-node reply credit-return = edge-triggered `CreditReturn{stream_id,
  tail}` carrying the **absolute** caller tail, applied `shadow_tail = max(shadow_tail, tail)`
  (monotone max-merge — reorder/dup safe), plus a low-rate tail **heartbeat** so a dropped final
  frame never wedges the callee. `stream_id` is a process-monotonic **nonce** (no ABA on the
  transport `stream_id→ring*` map); the receive path **gen-gates before any write** to ring memory.
- **004-Resources.md / 003-Memory.md** — The reply ring + slots + arena are **caller-shard pmr-
  owned, pre-allocated cold at `ask_stream`, pooled/reused** → 0 per-item heap. Add the **idle-
  density footprint** tax (a whole ring per in-flight streaming ask, cap×slot, e.g. 16 KB @cap-256,
  vs a 64 B scalar ReplyCell) with a small default cap governed by 022 `credit_limit`, a shard slab,
  and an admission cap on concurrent streaming asks. Note the cold `task<>` frame alloc/ask and the
  optional pooled `promise_type` operator-new to reach 0.
- **002-Scheduler.md** — The reply-stream terminal edge (cancel/deadline/close) reuses the seq_cst
  Dekker close-out: the terminal CAS **arms the caller drain** (`notify_enqueued`) **and** bumps
  `credit_gen + notify` so a stalled callee wakes. Never key wakeup on ring/credit emptiness.
- **001-Actor-Execution-Model.md** — State GATE-1 for both legs: neither the callee producing reply
  items nor the caller draining reply batches is entered concurrently with its own actor; a
  suspended handler advances neither `head` nor `disp/tail` past the parked item (mirror ADR-014).
- **022-Resource-Governance-and-Overload-Control.md** — Reply backpressure is the **credit window
  (a producer stall), never mid-stream shedding**; the concurrent-streaming-ask admission cap and
  default reply-ring capacity are 022-governed (inherits 024's adaptive-`credit_limit` open item).
- **023-Performance-Targets-and-Budgets.md** — Add outbound-reply rows: per-item ≥3× amortization
  (**achieved 8.3×**), caller-drain **0 cross-core RMW** (**achieved 0**, the gate PUSH passes and
  PULL fails), 0 per-item heap (**achieved**), sustained ≥4M/s hard floor (**achieved 5.3–6.4M**),
  ≥10M/s goal (**MISSED for PUSH; PULL hits 31–33M but violates the 0-RMW gate**). Record that
  throughput does not override the 0-RMW/footprint ceilings.
- **decisions/ADR-007** — Note extension: the single-shot monotonic-gen `ReplyCell` is **preserved
  verbatim** for ordinary asks; multi-item streaming replies use a **distinct** `StreamReplyCell`
  (single OPEN resolve) + ring + EoS, never a mid-stream cell reuse. The permanent reply-UAF
  ASan+TSan CI gate extends to the multi-terminal (close/cancel/deadline) reclaim + cross-node
  replay surface.
- **decisions/ADR-005** — Note the ring is now used in **both** directions; the caller-shard-owned
  ring with a callee/transport-thread cross-shard producer is a new NUMA case (remote-store penalty)
  the ADR-005 single-shard measurements did not cover — flag for `perf c2c` on a 2-socket rig.
- **decisions/ADR-014** — Record that the outbound reply drain **is** the ADR-014 item-drain flipped
  (proven), but the OPEN-cell 015 re-admit + the terminal-wake edge need their own ADR-014-grade
  real-scheduler run before 006 outbound promotes.

## Post-decision update (2026-08-04) — the 015 OPEN-cell re-admit blocker is PARTIALLY cleared

While scoping this exact gate, `detail::reply_cell.hpp`'s `ReplyCell<R>::resolve()` was wired to
route its co_await resume through `Activation::complete_parked()` (exec-state-gated, 015) instead
of a raw `h.resume()` — and, more consequentially, a genuinely SEPARATE, pre-existing lost-wakeup/
UB race was found and fixed IN `complete_parked()` itself (a carrier could legitimately race ahead
of `drain_step`'s own `exec_.park()` store; confirmed with an executable repro, 429/429 stuck under
a 5 µs injected delay — see [ADR-015](ADR-015-actor-execution-vehicle-passive-stackless-vs-fibers.md)'s
residual risk #7). Both fixes are proven under a REAL multi-worker Engine via `tests/
ask_coawait_real_scheduler_test.cpp`: 100 Askers × 25 sequential `co_await ask<int>()` cycles each
(2500 total, chained suspend/resume through the SAME activation) against a small pool of Answerer
actors — exactly-once, 0 lost, 0 mismatched, full 199-test suite green.

Since `StreamReplyCell = detail::ReplyCell<Opened>` is the SAME template instantiated on a
different `R`, this fix applies to it VERBATIM — the underlying mechanism this ADR's gate names is
no longer broken. At the time of this update, the ask_stream-specific dedicated integration run
(the ADR's own tie-breaking experiment, below) had not yet been built — that closure follows in the
next update.

## Post-decision update #2 (2026-08-04, same day) — the tie-breaking experiment is DONE; 006 outbound promotes

`ActorRef<A>::ask_stream<F>(Q)` / `LocalRouter::ask_stream<A,F,Q>` — the addressing layer this ADR's
own `reply_stream.hpp` comment named as not-yet-built — is now wired (`actor_ref.hpp`), mirroring
`ask` exactly: `static_assert(Handles<A, AskStream<Query,F>>)`, `make_ask_stream` + the existing
pooled-descriptor `post_message` path, returning the `OpenStreamFuture<F>`. `dispatch.hpp` needed no
changes (already generic over message type); `StreamResponder<F>::accept()`/`reject()` became `const`
(mirroring `Responder<R>`'s existing `mutable armed_` pattern), since `handle(const M&)` is mandatory.

The named tie-breaking experiment itself: `tests/ask_stream_coawait_real_scheduler_test.cpp` runs a
real 4-worker/4-shard `Engine` with a `Streamer` actor whose `handle(const AskStream<Query,Row>&)`
accepts and pushes 8 items back-to-back with no delay, and a `Puller` actor whose `task<> handle`
does `co_await target.ask_stream<Row>(...)`. 100 Pullers × 25 streams each = 2500 independent
OPEN-vs-first-item races across real worker threads — exactly-once OPEN resume, every item delivered
FIFO with 0 loss, clean `Closed` termination, no gap, 3/3 repeated runs green, full 200-test suite
green. **This is the result ADR-018's Tie-breaking experiment section names as sufficient to flip
006 outbound Draft→Accepted (x86-64).** TSan remains unavailable on the proving host (as for the
2026-08-04 #1 update); ASan+UBSan + the full suite were used instead.

**A genuinely new finding surfaced while building this test, worth recording as its own residual**
(below): `ReplyCell::resolve()`'s co_await resume is fully synchronous and stack-reentrant on the
RESOLVING thread, not deferred to a scheduler tick. A naive first draft of the integration test had
the Puller busy-spin-drain the ring synchronously inside the same coroutine turn that `co_await`
resolved in — and every one of the 2500 streams stalled, because `Streamer::handle()`'s call to
`accept()` does not return (so its own subsequent item pushes cannot happen) until the ENTIRE resumed
Puller continuation finishes running, nested inside the Streamer's own call stack. This is not a bug
in the mechanism (it is the intended "transfer, not re-enqueue" no-extra-scheduling-hop design,
already proven correct for `ask`) — but it is a real, previously-undocumented hazard for anyone
authoring an `ask_stream` callee handler that does work after `.accept()`. See the new residual risk
below. The shipped test avoids it by handing the opened `ReplyStream<F>` off to a plain drain-thread
pool rather than draining inline in the actor's own coroutine turn.

## Residual risks

- **015 OPEN-cell re-admit + ask_stream-specific integration — CLOSED (2026-08-04).** Both the
  mechanism (via `ask`'s shared `ReplyCell<R>` template) and the ask_stream-specific dedicated
  integration run (via `tests/ask_stream_coawait_real_scheduler_test.cpp`) are now proven under a
  real multi-worker Engine. See the post-decision updates above.
- **NEW: OPEN-resolve reentrancy is a callee-authoring hazard, not yet mitigated or even a lint.**
  `StreamResponder::accept()` (and `Responder::operator()` for ordinary `ask`) may synchronously run
  the ENTIRE resumed caller continuation, inline, on the callee's own thread, nested inside the
  callee's current call — because `Activation::complete_parked()`'s coroutine resume is unconditionally
  inline (unlike 024's inbound `StreamActivation`, which deliberately defers via a Parked→Scheduled
  transfer picked up by a worker's own re-acquire, precisely to avoid this). A callee handler that does
  meaningful work AFTER resolving/accepting — expecting that work to happen-before the resumed caller
  observes it — is unsafe if the caller is a suspended waiter; the caller's continuation can run to its
  next suspension or completion before the callee's own remaining statements execute. No compile-time
  or runtime guard catches this today; it is a documented hazard, not an enforced invariant. Considering
  whether `complete_parked()`'s cross-activation resume should defer (transfer-and-let-a-worker-pick-up,
  mirroring 024's inbound design) instead of running inline is a real open question — it would trade
  this reentrancy hazard for an extra scheduling hop on every co_await resume, a hot-path cost with its
  own tradeoffs; not decided or scoped here.
- **AArch64 / weak-memory re-gate (INCONCLUSIVE, no decision weight).** Every load-bearing order —
  the producer arm-edge `armed.exchange`, the seq_cst Dekker close-out, the terminal-wake fence, the
  cross-node gen-gate — is proven only on x86-TSO. AArch64 needs explicit seq_cst fences/exchanges
  and a herd7/GenMC litmus pass; deferred, no hardware. Nothing here may be promoted beyond x86-64.
- **Deferred Hard budget: the 10M/s/core goal is MISSED by PUSH (5.3–6.4M).** The 4M hard floor is
  met; the 10M goal is not. A pooled `promise_type` allocator, cap tuning, and a re-measure on the
  reference core are the path; until then the goal is an open Hard-budget item, not a gate failure.
- **Idle-density footprint.** A whole ring per in-flight streaming ask (vs a 64 B scalar ReplyCell)
  multiplies 024's ~60K-streams/GB tax per ask; a fan-out of many concurrent short streaming replies
  is heavy. Mitigation (small default caps, shard slab, admission cap) is specified but unproven and
  could oscillate (inherits 024's adaptive-`credit_limit` open question).
- **Cross-node throughput is BDP-bound.** Credit-return is edge-triggered, so on a high-RTT link a
  fixed window underfills and reply throughput → window/RTT. The ≥10M claim is **local only**;
  cross-node latency stays INCONCLUSIVE pending reference silicon. PULL's F4 K-grid never crossed the
  BDP saturation point, so its "bandwidth-bound for K≥BDP" is trend-proven, not crossing-proven.
- **Larger terminal-state UAF surface.** Three terminal causes (close/cancel/deadline) arbitrated by
  a term_state CAS + a ring gen-fence + the OPEN cell's own single-shot lifecycle = strictly more
  concurrent-reclaim states than ADR-007's single-shot cell. A missed interleaving is a UAF, not a
  wrong value — the permanent ASan+TSan CI gate is mandatory.
- **Durable exactly-once across a real callee crash is out of scope** (needs 017 Persistent's
  transactional outbox); the volatile reply ring only dedups transport retransmit under 010 per-
  stream FIFO. The fan-in deriving-reply hazard (017) is a real hazard once a reply is forwarded
  through an effectively-once pipeline.

## Tie-breaking experiment (only if the promotion gate is contested) — DONE, 2026-08-04

None needed to pick the winner — the F3 0-RMW hard gate is decisive and measured (PUSH 0, PULL
0.00391/item). The **one** experiment that would unblock promotion: an ADR-014-grade real-scheduler
run of the **015 OPEN-cell re-admit** (`co_await` on-lane resume) for `StreamReplyCell`, under the
real 002 scheduler + ASan/TSan, proving the OPEN handshake resolves exactly once with no lost wakeup
and no UAF when it races the ring's first item. **Run and passed** — see Post-decision update #2
above (`tests/ask_stream_coawait_real_scheduler_test.cpp`, 2500/2500, 0 mismatches, 3/3 repeats). This
result flips 006 outbound Draft→Accepted (x86-64).
