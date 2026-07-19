# ADR-019 — Best-effort broadcast / publish primitive (`Topic<M>`, at-most-once fan-out)

Status: **Accepted (x86-64) for LOCAL fan-out** · **Draft for CROSS-NODE fan-out** (gated on GATE 7) · ARM/weak-memory **INCONCLUSIVE** (deferred, no hardware)
Date: 2026-07-19
Deciders: design-debate-prove (judge close-out)
Related: ADR-002 (mailbox MPSC), ADR-007 (handler dispatch), ADR-005/014/018 (streams), ADR-006/011 (cluster topology / relay), ADR-015 (execution vehicle), ADR-016 (wire encode)
Specs touched: 006, 003, 022, 017, 007, 010, 026, 002, 004, 025, 023

---

## Question

Quark has point-to-point `tell` (fire-and-forget to ONE actor), `ask`, typed routers that route one pre-stamped descriptor to ONE group member (ADR-007), and `Stateless<N>` load-fan-out to ONE pool worker (025) — but **no subscriber-agnostic one-to-many primitive**. Design `publish`/`broadcast`: a publisher fires a message to MANY subscribers without caring who or how many are listening. The load-bearing semantic is **BEST-EFFORT AT-MOST-ONCE**: the publisher **never blocks and never stalls** on any subscriber; a slow / full / dead subscriber is DROPPED (per-subscriber, counted), it does **not** back-pressure the publisher. This is the deliberate opposite of ADR-018 (which has credit backpressure).

Can a 006 broadcast section be stamped **Accepted (x86-64)**, or must it stay Draft on a named gate?

## Designs (one-line summaries)

- **D-A — `Topic<M>` best-effort broadcast (AtMostOnce fan-out).** Membership = `std::atomic<std::shared_ptr<const SubVec>>` immutable copy-on-write snapshot + per-entry `active` flag; unsubscribe uses **bounded quiescence** (`active=false` then wait `in_flight==0` — never awaits, never delays the *publisher*). ONE immutable pool-allocated refcounted `SharedPayload<M>` fanned as N thin descriptors onto each subscriber's **unchanged ADR-002 mailbox**; per-subscriber DROP-on-full via a broadcast-owned outstanding counter; cross-node coalesced one frame per node. *(Submitted as a schema "probe", F1/S1 withdrawn as placeholders, replaced in rebuttal with the full B1–B8 design and proven.)*
- **D-B — Immutable Refcounted Shared-Payload Fan-Out.** Same shape, but membership reclamation was a **hand-rolled `SubscriberTable` refcount reached through a mutable `head_` pointer** (EBR-via-015 boast withdrawn under attack, replaced with a plain table-refcount).
- **D-C — TopicBus (RCU-snapshot registry).** Same shape; membership = `atomic<Snapshot*>` + **QSBR beacons reused from 026** + `synchronize_rcu()` grace on unsubscribe. Claimed the shared payload makes per-subscriber publish `< 0.5×` a discrete tell.

All three converge on the **same safe architecture**: one immutable refcounted payload + N thin descriptors onto the verbatim ADR-002 mailbox + per-subscriber drop-on-full. The debate turned on (1) *how membership reclamation is made race-free* and (2) *which fast claims survived execution*.

## Evidence table (claim → survived red-team? → proven by executed C++? → number)

| Design | Claim (gate) | Survived | Proven | Number / result |
|---|---|---|---|---|
| **D-A** | B1 fast: 1-copy amortization + O(1) heap (F1/S1 slot) | n/a¹ | **CORRECT** | copies/pub = 1 for all N∈{1,64,1024} & M∈{16B,256B,4KB}; malloc/pub = 0; **amortization ratio = N** (1024× at N=1024); ~22.5M subscribers/s |
| **D-A** | B2 **GATE 1** publisher never stalls | n/a¹ | **CORRECT** | full+dead-local+dead-remote vs all-empty: p99 delta −1.6%/−6.6% (does NOT rise); all pathological legs O(1) |
| **D-A** | B3 **GATE 2** at-most-once + counted drops | n/a¹ | **CORRECT** | recv 144,001,024 + drop 111,998,976 = 256,000,000 = N·K exact; recv+drop==K for all 256 subs; delivered ∈ {0,1} |
| **D-A** | B4 **GATE 3** per-(pub,sub) FIFO | n/a¹ | **CORRECT** | 0 inversions / 1e6 pub × 256 subs |
| **D-A** | B5 **GATE 4** shared-payload lifetime | n/a¹ | **CORRECT** | ASan/UBSan/TSan clean; 3 firing controls all fire (skip-dec → pool leak; extra-dec → heap-UAF; no-dtor path covered) |
| **D-A** | B6 **GATE 6** subscribe/unsubscribe race-free | n/a¹ | **CORRECT** | TSan/ASan clean on `atomic<shared_ptr>` swap + active flag + `in_flight` barrier; post-unsub deliveries = 0; control fires 93,292 |
| **D-A** | B7 **GATE 5** mailbox verbatim / zero-cost-unused | n/a¹ | **CORRECT** | enqueue = 1 `xchg` + link store, 0 new drain RMW; 0 `Topic` symbols when uninstantiated; `sizeof(Mailbox)=192B`. *(exec-state CAS out of harness scope — INCONCLUSIVE for that sub-mechanism; mailbox single-consumer/FIFO/no-loss proven)* |
| **D-A** | B8 **GATE 7** cross-node amplification bounded | n/a¹ | **CORRECT (sim, x86-TSO)** | wire copies/pub = D=8 (== #nodes, not R=4096); dead node dropped immediately, publisher latency unchanged |
| **D-B** | F1,F2,F3,S1,S3,C1,C2,C3,C4 | yes | **CORRECT** | O(1) heap, GATE 1 flat (block-control +89,382c p99), FIFO 0-inv, GATE-4 exactly-once w/ 3 controls, drain byte-identical |
| **D-B** | **S2 GATE 6** subscribe/unsubscribe race-free | yes | **WRONG** | **heap-use-after-free on CLEAN build, 5/5 g++ AND 5/5 clang.** COW writer frees the old table (refcount 1→0) in the window between the reader's `head_.load` and its `refcount.fetch_add`; publisher then reads a freed `SubscriberTable`. A plain refcount reached through a concurrently-reclaimed pointer is not a safe-acquire SMR. Fix needs hazard-pointers / RCU / split-refcount — **not cheap** |
| **D-C** | S1,S2,S3,C1,C2,C3 (GATES 4,6,2,3,5,7) | yes² | **CORRECT** | all six gates clean under ASan/TSan/LSan with firing controls; `atomic<shared_ptr>` + `synchronize_rcu` grace; pool 262144/262144 reclaimed |
| **D-C** | F2 **GATE 1** publisher never stalls | yes² | **CORRECT** | full+dead vs empty p99 does NOT rise; block-control ~1000× (171,176 ns) proves harness sees stalls |
| **D-C** | **F1** per-sub `< 0.5×` a discrete tell | yes² | **WRONG** | topic is **1.57–2.25× SLOWER** per subscriber (extra refcount+inflight atomics + per-sub pooled-desc lock). Alloc sub-claim (O(1), N-independent) holds; headline latency claim falsified |
| **D-C** | F3 dynamic registry "nearly free vs static" | yes² | **INCONCLUSIVE** | safety-verified build (read-guard) pays +15–25% > noise; noise-level beacon-QSBR build's safety not co-verified in the same harness |

¹ D-A's B-series was introduced in rebuttal (replacing the withdrawn probe placeholders) and proven by adversarial *execution* (both compilers, sanitizers, firing controls) but did **not** pass through a substantive cross-examination round — see Residual risks.
² D-C's cross-examination attack was itself a vacuous placeholder ("test steelman / test argument"); its claims "survived" only a non-attack. Decisive falsification of F1 came from the prover, not the red-team.

## Decision

**Winner: D-A — `Topic<M>` best-effort broadcast (AtMostOnce fan-out).**

Reasoning, in the judge's priority order:

1. **Safety is a gate.** **D-B is DISQUALIFIED**: GATE 6 is a proven **heap-use-after-free on the clean build under both compilers** (subscribe/unsubscribe raced against a concurrent publish → publisher reads a freed subscriber table), and the stated fix is explicitly *not* cheap (hazard-pointers / RCU / split-refcount — a structural change, not a parameter). A shared-membership UAF / delivery-to-a-departed-subscriber is a LOSE-condition. D-A and D-C pass all seven gates.

2. **Proven beats claimed; disproven counts against.** D-A: **8 gates proven CORRECT, 0 disproven.** D-C: 6 gates + GATE 1 proven CORRECT, but **F1 disproven** (the headline throughput win — "cheaper than a discrete tell per subscriber" — is false; it is 1.6–2.3× *slower* per subscriber) and F3 INCONCLUSIVE. D-A dominates D-C on the count and carries no disproven claim.

3. **Best measured numbers on the core gates.** D-A proves the copy amortization cleanly at **ratio = N** (payload copies/pub = 1, malloc/pub = 0, independent of N and of `sizeof(M)`) and **~22.5M subscribers/s**, with publisher latency **flat** under a full + dead-local + dead-remote set (GATE 1). D-C's own F1 shows the honest truth all three designs share: the ~40 ns per-subscriber ADR-002 enqueue floor dominates, so the win is in **allocation / copy volume (O(1), 1× copy)**, not in beating a single tell's wall-clock. D-A states this correctly; D-C over-claimed and was falsified.

4. **The decisive engineering point** is *how membership reclamation is made safe*. D-A and D-C both use an **`atomic<shared_ptr>` immutable snapshot** — a safe-acquire SMR whose control block keeps the vector alive across the publisher's load+walk — and both proved GATE 6 clean. D-B hand-rolled the refcount through a mutable `head_` and got the classic acquire race → UAF. **D-A's bounded-quiescence unsubscribe** (`active=false` then wait `in_flight==0`) is *lighter* than D-C's `synchronize_rcu()` grace (which self-discloses a potentially millisecond-scale wait on the unsubscriber) and never touches the publisher, so D-A wins the tie within the safe family.

5. **No core invariant is bent.** Broadcast lowers to **N ordinary ADR-002 tells sharing one payload** — enqueue/drain byte-identical (B7/GATE 5), at-most-one executor per subscriber unchanged, mailbox FIFO per-(pub,sub) preserved (B4/GATE 3), std-only C++23, zero heap growth on the publish hot path, zero cost when no `Topic<M>` is instantiated. ADR-002 mailbox and ADR-007 point-to-point routing are **untouched**.

D-C is recorded as the **corroborating twin**: it independently proves the same safe architecture and is a valid fallback; only its over-stated fast claim and its heavier unsubscribe grace place it behind D-A.

### promotes006

**YES for LOCAL fan-out (x86-64).** All six local gates — GATE 1 (publisher never stalls), GATE 2 (at-most-once + counted drops), GATE 3 (per-(pub,sub) FIFO), GATE 4 (shared-payload exactly-once), GATE 5 (mailbox verbatim + zero-cost-unused), GATE 6 (subscribe/unsubscribe race-free) — are proven CORRECT by executed C++ under GCC 14.2 + Clang 20.1, ASan/UBSan/TSan with firing controls, on the shipped ADR-002 mailbox + 003 pool, std-only.

**NO for CROSS-NODE fan-out — stays Draft on GATE 7.** Bounded amplification (one frame per distinct node, dead-node no-stall) is proven only on an **in-process simulated transport, x86-TSO**; the relay-tree variant's per-(pub,sub) FIFO depends on **ADR-011 path-pinning** and is INCONCLUSIVE, and a **real-transport** amplification + dead-node proof is required before the cross-node section can be stamped.

## Spec recommendations

- **006-Messaging-and-Addressing.md** — Add a **Publish/Subscribe (broadcast)** section, stamped **Accepted (x86-64) for local fan-out, Draft for cross-node**. Specify: `Topic<M>` with `subscribe(ActorRef<A>)` / `unsubscribe(...)` (idempotent, ActorId **set-semantics dedup** in the COW rebuild so a double-subscribe yields one delivery — GATE 2 at actor granularity) and `publish(M) -> PublishReceipt{delivered, dropped_full, dropped_deadline, remote}`. State the invariants: publisher never blocks (GATE 1); membership is an `atomic<shared_ptr<const SubVec>>` immutable snapshot; delivery is **N ordinary ADR-002 tells sharing one immutable refcounted payload** (composes with ADR-002 + ADR-007 verbatim, no reply/ReplyCell binding — fire-and-forget, the deliberate opposite of ADR-018). Bounded-quiescence unsubscribe guarantees no delivery after `unsubscribe()` returns.
- **017-Delivery-Guarantees.md** — Add a **best-effort broadcast** note under the existing `AtMostOnce` level: broadcast is `AtMostOnce` at a *distinct-sender granularity* (like a stream) with **per-(topic,subscriber) COUNTED (non-silent) drops**, **no retry, no ack, no dedup**, and **no cross-subscriber order**. `MessageId=(sender,seq)` is carried for per-(pub,sub) FIFO/observability only, never for duplicate suppression. Note explicitly that layering AtLeastOnce on broadcast is out of scope — any per-subscriber ack would reintroduce a publisher stall and violate GATE 1.
- **003-Memory.md** — Add the **shared-payload reclamation** rule: one immutable pool-allocated `SharedPayload<M>{atomic<uint32_t> rc; dtor-thunk; M}` per publish; init rc, `fetch_add` per admitted enqueue under a publisher BUILD ref, final `fetch_sub(acq_rel)==1` runs the **type-erased dtor** (same mechanism the tell path already carries) then returns the cell — reclaimed **exactly once** whether a subscriber consumes, drops, unsubscribes, or dies. Put `rc` on its **own cache line** (padded away from the immutable `M`) to avoid read-side/reclaim-side false sharing. Note the residual O(N) coherence traffic on the reclaim line is a consumer-lane cost, off the publisher's critical path.
- **022-Resource-Governance-and-Overload-Control.md** — Add **fan-out amplification governance**: broadcast owns its own caps (there is **no** shipped concurrent-outstanding-broadcast cap today — do not claim reliance on 022 ingress token-buckets). Specify a **per-(topic,subscriber) outstanding-broadcast counter** (relaxed inc on the broadcast leg, dec in the broadcast reclaim branch — touches no ordinary-tell code; soft/approximate bound is acceptable for best-effort) and a **per-shard live-payload cap L** (publish sheds drops-all, counted, rather than allocate past L). Cross-node amplification is governed by **#distinct-subscriber-nodes** (≤ 026 `relay_cap = ⌈log₂N⌉`).
- **007-Failure-and-Supervision.md** — Add **dead-subscriber pruning**: a SWIM-Suspect/Dead subscriber-node's copy is dropped immediately (counted), no publisher wait; a dead local subscriber resolves through `ActorRef` to dead-letter. Optional SWIM-driven snapshot prune keeps the COW subscriber table from accumulating tombstones. Subscriber-mailbox lifetime is governed by the existing ActorRef/007 discipline, **not** re-solved by the topic.
- **010-Distribution.md** / **026-Large-Scale-Cluster-Topology.md** — Add **cross-node fan-out (Draft)**: coalesce remote subscribers **by node**, one fire-and-forget frame per distinct node (amplification = #nodes, not #remote-subs), or route via the 026 bounded relay tree (≤ `⌈log₂N⌉` hops). Note the **synchronous per-node ADR-016 encode** is publisher CPU work → publisher latency rises *linearly in distinct alive nodes* (this IS the bounded amplification, not a stall). Keep Draft pending real-transport proof + ADR-011 path-pinned relay-tree FIFO re-gate.
- **002-Scheduler.md** — No change to the scheduler contract; record that broadcast **schedules activations, not messages** (each delivery is an ordinary mailbox enqueue + exec-state Idle→Scheduled wake), at-most-one executor per subscriber unchanged.
- **004-Resources.md** — Record that the shared payload + thin descriptors are pool-resolved (shard size-class pools); no dynamic resource resolution while a message is processed.
- **025-Placement-Policies-and-Stateless-Workers.md** — Note broadcast is orthogonal to `Stateless<N>` fan-*to-one*; a stateless pool worker may be a subscriber, but broadcast does not route to a single pool member.
- **023-Performance-Targets-and-Budgets.md** — Add broadcast budgets: **payload copies/publish = 1** (independent of N and `sizeof(M)`), **malloc/publish = 0** (pool-warmed), **publisher latency flat under a full+dead subscriber** (GATE 1), and the honest note that per-subscriber wall-clock is floored by the ~40 ns ADR-002 enqueue (the win is copy/alloc volume, not beating a single tell). Reference numbers: ~22.5M subscribers/s, ~44 ns/subscriber at N=1024.
- **decisions/ADR-002-mailbox-mpsc-hot-path-r2.md** — Add a back-reference: the mailbox enqueue/drain is **reused verbatim** by broadcast (objdump byte-identical WITH vs WITHOUT `Topic<M>`; 0 new drain RMW). No change to ADR-002 itself.
- **decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md** — Add a back-reference: broadcast descriptors use the existing dense-slot dispatch (`slot_of<A,M>`) with `kind=Broadcast` and **no responder/reply field** (fire-and-forget). No change to the ADR-007 dispatch contract.

## Residual risks

- **ARM64 / weak-memory RE-GATE (deferred, no hardware).** Every ordering argument — the `SharedPayload` refcount `acq_rel`, the `active`-flag acquire/release, the `atomic<shared_ptr>` snapshot swap, the `in_flight`-barrier before/after `active=false`, and per-(pub,sub) FIFO via the mailbox `tail_.exchange` modification order — is verified on **x86-TSO only**. A herd7/GenMC litmus on the refcount-to-zero, active-flag, and snapshot-publish edges is required before any ARM stamp. Marked **INCONCLUSIVE**; must not decide the x86-64 acceptance.
- **Winner's B-series was not adversarially cross-examined.** D-A entered as a schema "probe", its placeholder claims were conceded, and the real design (B1–B8) was supplied in rebuttal and proven by *execution* — but it never passed through a substantive red-team round the way D-B did. The proof is strong (both compilers, sanitizers, firing controls, and it directly avoids D-B's exact SMR bug), but a fresh red-team pass on the bounded-quiescence unsubscribe and the outstanding-counter drop path is advisable before shipping.
- **GATE 5 exec-state CAS is INCONCLUSIVE in-harness.** B7's in-process harness models no scheduler, so the specific at-most-one-executor exec-state CAS was not exercised; only the mailbox single-consumer/FIFO/no-loss invariant it protects was proven. Re-verify against the real scheduler.
- **Deferred Hard-budget: aggregate footprint under a permanently-slow subscriber / starved owner shard.** The per-publish heap is proven O(1), but steady-state boundedness relies on the **new broadcast-owned caps** (per-subscriber outstanding counter C, per-shard live-payload cap L). With caps disabled the firing control shows linear growth — so C and L are load-bearing and must be spec'd and defaulted, not optional. There is no shipped 022 concurrent-broadcast cap to fall back on.
- **Cross-node stays Draft on GATE 7.** In-process simulation proved bounded amplification + dead-node no-stall, but real-transport re-gate and the ADR-011 path-pinned relay-tree FIFO re-proof are required before a cross-node broadcast section can be stamped Accepted. The per-node coalescing also makes loss **coarse** (one dropped frame loses the publish for all subscribers on that node) — a documented trade of loss-granularity for amplification governance.
- **Subscribe/unsubscribe is O(N) copy-on-write on the cold path.** High-churn large-fan-out topics pay O(N) table rebuilds (mutex-serialized, off the publish hot path). Segmented/tombstone-compact registries are a deferred optimization; the publish path is unaffected.

## Tie-breaking experiment (if the winner's lack of a red-team round is deemed disqualifying)

Run one adversarial cross-examination + re-prove on D-A's **bounded-quiescence unsubscribe** specifically: a publisher mid-fan-out (snapshot loaded, `in_flight` incremented) racing an `unsubscribe` that observes `in_flight==0` on a *different* generation — assert under TSan/ASan that no in-flight descriptor references a subscriber whose `unsubscribe()` has returned, with a firing control that removes the `in_flight` barrier. If that passes (as B6's 93,292-delivery control suggests it will), D-A stands unambiguously; if it reports, fall back to **D-C** (the corroborating twin, all gates proven, only the fast claim over-stated), which uses the more conservative `synchronize_rcu` grace.
