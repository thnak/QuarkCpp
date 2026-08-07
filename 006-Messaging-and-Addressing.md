# 006 — Messaging and Addressing

Covers how a message gets *to* an actor: identity, references, and the send verbs.
The current draft describes the mailbox and message lifecycle but never the send
API — this closes that gap.

## Actor identity

An actor is identified by an `ActorId`: the pair `(type, key)`.

```cpp
struct ActorId {
    quark::type_id type;   // which actor type (compile-time id)
    quark::key     key;    // user key: integer, string, or composite
};
```

`ActorId` is what `Placement` hashes to a shard (`002`). Identity is stable: the
same `(type, key)` always denotes the same logical actor and lands on the same
shard, whether or not it is currently activated.

## Actor references

Sends go through a typed handle, `ActorRef<A>`, obtained from the system:

```cpp
quark::ActorRef<Order> order = system.get<Order>(key);
```

`get` never blocks and never creates state — it resolves identity and placement
only. The actor is **activated lazily** on first message (or eagerly if
`KeepAlive`). An `ActorRef` is cheap to copy and safe to hold; it is a location +
identity, not a pointer to actor state.

## Send verbs

### `tell` — fire-and-forget

```cpp
order.tell(Ship{ .id = 42 });
```

- Enqueues a `MessageHandle` on the actor's mailbox (FIFO, `001`).
- Returns immediately; no reply channel is allocated.
- The payload is moved into shard-owned payload storage (`003`).

### `passivate` — on-demand, soft teardown (ADR-028 Phase 8)

```cpp
bool accepted = order.passivate();
```

- Fire-and-forget, like `tell` — posts the same `Deactivate` control descriptor the automatic
  idle-timeout wheel posts (011), through the same shared interlock, so the actor drains every
  message already queued/in-flight and retires at the next genuinely-idle instant. Never a hard
  mid-handler interrupt.
- Returns `false` iff the target never resolves to a live activation (nothing to passivate);
  `true` means accepted/already-pending, **not** "has retired yet."
- **Sequential-only** — a compile error on a `Reentrant`/`MaxConcurrency<N>` actor, same
  restriction as `IdleTimeout<Ms>` (011), since it reuses the same Dekker close-out sequence,
  proven only for exactly one in-flight drain.
- The `on_deactivate()` lifecycle hook (005) and the deactivation-time persistence flush (012)
  fire identically whether retirement was triggered by the wheel or by `passivate()` — one call
  site serves both.

### `ask` — request/response

```cpp
quark::task<Confirmation> t = order.ask<Confirmation>(Query{ … });
Confirmation c = co_await t;
```

- Routes a one-shot reply through a **shard-pooled, monotonic-generation `ReplyCell`**
  (not the caller's frame; ADR-007); completes when the handler produces the reply or
  the deadline/cancellation fires. **0 pool-upstream allocation** (measured); the
  round-trip is p50 ≈ 83 ns / p99 ≈ 130 ns locally, ~40× under the 023 ask budget.
- A deadline set on an `ask` is monotonic and, if the target is on another node,
  travels as remaining duration and is reconstructed against the receiver's clock
  (see `018-Clocks-and-Deadlines.md`).
- **`ask` is async-only** — it returns `quark::task<std::expected<R, quark::error>>`
  and is only callable from an async context; the reply type must match the handler's
  declared return for that message. There is **no `ask_sync`** (ADR-007): a blocking
  variant was rejected because its runtime "am I on a worker lane?" guard produced
  genuine TSan races in the competing designs. Off-lane bootstrap/edge code uses
  `quark::block_on(task)`, which **asserts it is not running on a worker lane**
  (returns `unexpected(on_worker)` if it is).
- Reply delivery does **not** re-enter the target (asking) actor's own exec-state — it
  re-admits through the 015 Parked→Scheduled/Running gate, so the asker's activation is
  never concurrently executed twice. That gate does NOT mean the resume happens on a
  DIFFERENT thread, though: `ReplyCell::resolve()`'s co_await path runs
  `Activation::complete_parked()` — the asker's ENTIRE resumed continuation — synchronously,
  inline, on whichever thread calls `resolve()` (ordinarily the replying actor's own worker).
  A callee handler that keeps doing work AFTER resolving/accepting a reply must not assume
  that work happens-before the resumed asker observes it (see `ask_stream<F>`'s reentrancy
  hazard note below and [ADR-018](decisions/ADR-018-outbound-streaming-replies.md)'s residual
  risks — found while proving `ask_stream`, applies equally to plain `ask`).

### Reply type and errors

An `ask` handler's result is delivered as `std::expected<Reply, quark::error>` so
that failure (handler threw, actor stopped, deadline exceeded — see `007`) is a
value, not an exception across the boundary:

```cpp
std::expected<Confirmation, quark::error> r = co_await order.ask<Confirmation>(q);
```

`AskFuture<R>::await_resume()` — the `ask` awaitable above — **returns a `result` in the
failure state**; it never throws. `quark::task<T>` (`001` §Hybrid handler execution,
ADR-047), the *separate* nested-coroutine primitive a handler uses to call an ordinary
async function, instead **rethrows at `co_await`** — the same idiom `task<void>`'s own
`fault_`/`faulted()`/`fault_ptr()` already uses. This is a deliberate, documented split
between the two async return shapes in the codebase, not an oversight — don't try to
unify them without revisiting ADR-047's reasoning. Where `T` is itself `quark::result<U>`
(e.g. `task<result<ChatResponse>>`), `task<T>::await_resume()` yields exactly `result<U>`
— unconditional pass-through, never `result<result<U>>`; only a *thrown* exception is
captured into the fault channel.

### `ask_stream` — a reply that is a *stream* (ADR-018)

When a reply is **multi-item** (a query answered by many frames, a subscription, a
paged scan) the single-shot `ReplyCell` cannot carry it — it resolves **once**, a
stream resolves **N times then completes**. `ask_stream<F>` is the outbound
counterpart of 024's inbound ingestion: **the 024 credit-ring run backward** — the
**callee is the producer, the caller is the consumer** (ADR-018, winner
*Reply-Credit-Ring / PUSH*).

```cpp
// caller: ask for a stream of F; drain it batch-per-turn like an inbound stream.
quark::ReplyStream<Row> rs = (co_await scan.ask_stream<Row>(Query{ … })).value();
while (auto row = rs.next()) { use(*row); }        // 0-RMW batch drain (024 §drain)
// rs terminal state: Closed (EoS) / Cancelled / DeadlineExceeded / Failed(error)
```

- **Mechanism.** `ask_stream<F>(M) -> task<expected<ReplyStream<F>, error>>`. The
  reply rides a bounded, pre-allocated `StreamChannel<F>` **flipped**: callee = `head`
  producer, caller = `disp`/`tail` consumer. Credit is the **derived**
  `capacity-(head-tail)` — **no shared counter** — so a slow caller stalls a fast
  callee (a producer stall, **never** a mid-stream drop; 022). The item-drain leg **is**
  the shipped 024 `StreamChannel`/`StreamActivation`, so intra-stream FIFO, 0 per-item
  heap, and 0 cross-core RMW on the caller drain come for free (proven — ADR-018 F1–F3).
- **Three seams.** A **single-resolve `StreamReplyCell`** (the OPEN handshake — 16 B,
  rides the ADR-007 `reply_` field; **the ordinary single-shot `ReplyCell` is
  untouched**), the **N-item ring**, and an **in-band EoS**. ADR-007 reply-ordering
  governs delivery of the **OPEN handle** (Sequential → request order, Reentrant →
  completion order); intra-stream FIFO is the orthogonal monotone `head` order.
- **Identity & exactly-once.** Each item carries a **callee-assigned, replay-deterministic
  `producer_seq`**; the caller dedups by its `disp` high-watermark (a caller-local ring
  index is **not** a valid identity — see `017`). Cross-node, the reply stream rides the
  010 transport with a monotone-max-merge credit-return; the deadline travels as
  remaining-duration and is reconstructed against the callee clock (`018`).
- **Cancel / deadline** tear the stream down, return credit, stop the callee, and deliver
  nothing after teardown — a two-part terminal wake (arm the caller drain **and** wake a
  stalled callee; `002`). The ring + handle are reclaimed **exactly once** with no UAF
  (the ADR-007 reply-UAF gate extended to the multi-terminal reclaim surface).
- **`ReplyMode::{Push(default), Pull}`.** Push (above) is the default and the only path
  that meets the 023 **0-RMW** caller-drain gate. A secondary `Pull` (demand-driven,
  `DemandChannel`) is adopted for high-RTT cross-node links and bursty subscribe-style
  replies where the callee must be *provably idle*; it trades the 0-RMW gate for intrinsic
  backpressure and higher raw throughput (ADR-018 §Decision).

## Protocol

An actor's **protocol** is the set of message types it enumerates in
`using protocol = quark::Protocol<…>` and declares a `handle` for. `tell`/`ask` are
constrained by concept so that sending an unhandled message type is a **compile
error**, not a runtime dead-letter. The concept checks **protocol membership**, not
merely that a `handle` overload is callable ([ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)):

```cpp
template<class A, class M>
concept InProtocol = /* M ∈ A::protocol */;
template<class A, class M>
concept has_handle = requires(A a, const M& m) { a.handle(m); };

template<class A, class M>
concept Handles = InProtocol<A, M> && has_handle<A, M>;
```

`ActorRef<A>::tell(M)` and `::ask<R>(M)` require `Handles<A, M>`. Membership (not mere
call-validity) is required so an implicit conversion cannot smuggle an unlisted type to
a runtime slot — and so `slot_of<A,M>` can never index past the dense jump-table (a
handled-but-unlisted overload is itself a compile error, per 005 Validation).

## Delivery semantics

| Property | Guarantee |
|---|---|
| Ordering | FIFO **per (sender, receiver)** pair, local node. |
| Streams (024) | Inbound stream frames preserve **FIFO per-stream** end-to-end. A stream is a **distinct sender** from the control-plane `tell`s to the same actor — no mutual global order. Frames drain in **batches** off a per-stream ring, not one mailbox descriptor per frame; the mailbox hot path is unchanged. |
| Duplication | At-most-once locally. Across nodes it is set by the chosen `Delivery` level (`017`): at-least-once may duplicate; **effectively-once** dedups via message identity + fencing (see below). |
| Backpressure | Bounded mailbox → `tell` may block, fail, or shed per policy (open question). |
| Cancellation | An `ask` observing a fired deadline/stop cancels the queued message (`001`). |

**Principal propagation on a local forward (ADR-044 C3).** The ambient `Principal`
(`020` §3) a handler is running under propagates to a `tell`/`ask` it issues the
same way trace id and deadline do — unconditionally, **not** gated on crossing a
node boundary. A handler that received a message under a non-anonymous principal
and forwards work to another LOCAL actor carries that principal to the next hop
automatically, unless it explicitly attenuates. See `020-Security.md` §3 for the
attenuation rule and `010-Distribution.md` for the cross-node case.

## Publish/Subscribe (broadcast)

**Status: Accepted (x86-64) for local fan-out · Draft for cross-node**
([ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)).

`tell`/`ask` address ONE actor; routers and `Stateless<N>` fan to ONE group/pool
member. `Topic<M>` is the **subscriber-agnostic one-to-many** verb: a publisher
fires a message to MANY subscribers without knowing who or how many are listening.
The semantic is **best-effort, at-most-once** — a slow/full/dead subscriber is
**dropped (counted), never blocks the publisher**. It is the deliberate opposite of
024's streaming reply, which HAS credit backpressure; broadcast has none.

```cpp
quark::Topic<Tick> ticks;
ticks.subscribe(clockRef);              // ActorRef<A>, idempotent
quark::PublishReceipt r = ticks.publish(Tick{ .seq = 42 });
// r = { delivered, dropped_full, dropped_deadline, remote }
ticks.unsubscribe(clockRef);           // no delivery after this returns
```

- `subscribe(ActorRef<A>)` / `unsubscribe(ActorRef<A>)` are **idempotent** with
  `ActorId` **set-semantics dedup** — a double-subscribe of the same actor yields
  **one** delivery (at-most-once at actor granularity).
- `publish(M)` returns a `PublishReceipt{delivered, dropped_full, dropped_deadline,
  remote}` — per-subscriber drops are **counted, not silent**. There is **no reply
  channel and no `ReplyCell` binding**: broadcast is fire-and-forget.
- **Publisher never blocks** (GATE 1): every leg is O(1); a full + dead-local +
  dead-remote subscriber set does not raise publisher latency.
- **Membership** is an `atomic<shared_ptr<const SubVec>>` immutable copy-on-write
  snapshot; the publisher loads one snapshot and walks it. `unsubscribe` uses
  **bounded quiescence** (mark inactive, wait `in_flight == 0`) so **no delivery
  can occur after `unsubscribe()` returns** — it never delays the publisher.
- **Delivery lowers to N ordinary `tell`s sharing ONE immutable refcounted payload**:
  each subscriber gets a thin descriptor onto its **unchanged mailbox** (001/002),
  dispatched through the existing `slot_of<A,M>` jump-table with `kind=Broadcast`
  and no responder field. It composes with the `tell` path (ADR-002) and handler
  dispatch (ADR-007) **verbatim** — enqueue/drain byte-identical, at-most-one
  executor per subscriber, per-`(publisher, subscriber)` FIFO preserved.
- Cross-node fan-out (coalesce one frame per distinct node) is **Draft** pending
  real-transport amplification + dead-node proof (ADR-019 GATE 7).

## Ordered, reliable fan-out — `FanOut<M, Policy>`

**Status: Accepted (x86-64)** ([ADR-039](decisions/ADR-039-ordered-reliable-multi-subscriber-fanout.md)).

`Topic<M>` trades reliability for a never-stalling publisher; `ReplyStream<F>`
(024) is ordered and reliable but single-consumer. `FanOut<M, Policy>` is the
third point in that space: **ordered + reliable delivery to N dynamically
attaching/detaching subscribers**, at the cost of a policy-selected reaction to
a slow subscriber instead of a silent drop.

```cpp
quark::FanOut<Tick, quark::OnSlowSubscriber<quark::EvictAfter<256>>> ticks;
auto lane = ticks.subscribe(clockRef.id());      // this actor's own drain handle
quark::FanOutReceipt r = ticks.publish(Tick{ .seq = 42 });
// r = { delivered, evicted, departed }
Tick t;
while (lane->try_pop(t)) { /* per-(publisher,subscriber) FIFO */ }
ticks.unsubscribe(clockRef.id());                // no delivery after this returns
```

- **Precondition (load-bearing): single-producer.** Exactly one thread/actor
  calls `publish()` per `FanOut` instance — unlike `Topic<M>`, which is safe
  under concurrent publishers. Multi-producer fan-in needs an upstream
  serializing actor; `FanOut` does not arbitrate producers.
- `OnSlowSubscriber<EvictAfter<N>>` / `OnSlowSubscriber<Block>` are **CRTP
  policy parameters resolved at compile time** — the same family as
  `Sequential`/`Priority<P>`/`DrainBudget<N>` — with zero cross-policy symbols
  in the compiled type (ADR-039 F2).
  - `EvictAfter<N>`: a full per-subscriber lane (capacity `N`) drops the
    incoming message for that lane and bumps an exactly-once, non-silent
    counter (`LaneEntry::evicted()`) — the subscriber stays attached
    (bounded-lag, not ejection). This is **not** an end-to-end delivery
    guarantee into the subscriber's own mailbox; see 017.
  - `Block`: the whole `publish()` call stalls on the slowest **live**
    subscriber's lane until it drains or departs — fully reliable/gap-free,
    at the cost of an unbounded-by-time producer stall. A subscriber that
    stays attached but never drains stalls the producer forever by design;
    an external liveness backstop (007 supervision / deadline-based forced
    unsubscribe) is required, not provided.
- **Mechanism**: one shared refcounted `SharedPayload<M>` per publish (reused
  verbatim from `Topic<M>`/ADR-019/003); one genuinely-SPSC `StreamChannel<F>`
  lane per subscriber (reused verbatim from `ReplyStream<F>`/ADR-018), holding
  thin 16 B `FanOutEnvelope<M>{payload*, id}` descriptors, never `M` itself.
  Membership is `Topic<M>`'s exact `atomic<shared_ptr<const SubVec>>` COW
  snapshot + bounded-quiescence `unsubscribe` (ADR-019 GATE 6).
- `subscribe()` returns the subscriber's own drain handle (`shared_ptr<LaneEntry>`).
  A departed/evicted lane's still-queued envelopes are reclaimed in that
  handle's destructor only, once every reference (membership snapshot's +
  the subscriber's own) has dropped — never on the producer thread.

## Resolved (ADR-007)

- **`ask` from sync code** → forbidden. `ask` is async-only; off-lane bootstrap uses
  `quark::block_on` (asserts not-on-a-worker-lane). No `ask_sync`.
- **Typed vs. dynamic refs** → `ActorRef<A>` is **always typed**. Routers are
  homogeneous typed groups that forward **pre-stamped** descriptors; dead-letter is a
  **non-dispatching descriptor sink**. No untyped ref is exposed on the send path, so
  "unhandled send = compile error" holds universally. (The 008 `type_index → thunk`
  scan survives only where a message arrives without a static type — wire arrival — never
  on the local typed send path.)

## Open questions

- **One-to-many / broadcast** → **Resolved** ([ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)):
  the missing subscriber-agnostic fan-out primitive is now `Topic<M>` best-effort
  broadcast (see *Publish/Subscribe* above) — Accepted (x86-64) local, Draft cross-node.
- **Backpressure**: what does `tell` do on a full bounded mailbox — block the
  sender, return `std::expected` failure, or drop with a policy? The per-actor
  `Overflow<Block|Fail|DropOldest>` policy is the leaning here, and it is the
  **foundational lever** of the whole-engine overload model — bounded mailboxes
  generalized to every exhaustible resource, plus rate limiting, deadline-aware load
  shedding, and circuit breaking — in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md).
  This local, per-actor bound is the sole correctness guarantee, unconditionally
  enforced regardless of anything below. A REMOTE sender's own admission decision
  ([ADR-046](decisions/ADR-046-cross-node-mailbox-backpressure-signalling.md)'s
  `PeerCongestionGate`, resolving 010's cross-node variant of this same open
  question) is strictly a soft, latency-hiding front-end to it — proven (C3) to
  compose with, never substitute for, this section's bound whether or not the
  cross-node congestion gossip is wired, partitioned, or dropped. The wire
  `ControlKind::Congested` payload's `from_incarnation`/`congestion_remaining_ns`
  fields reuse this same deadline vocabulary (018-style deadline-travel
  reconstruction — a relative duration each side reconstructs against its own
  clock) rather than inventing a new one.
- **Streaming replies** — **Accepted (x86-64), 2026-08-04**
  ([ADR-018](decisions/ADR-018-outbound-streaming-replies.md)).
  `ask` returning a stream for multi-item responses is the **024 credit-ring flipped**
  (callee = producer, caller = consumer) — see `ask_stream<F>` above. The **inbound**
  direction was already Accepted (a `StreamRef<F>` handle + `handle(StreamBatch<F>&)` drain
  over the credit-ring of
  [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md)); the
  **outbound** reply-routing interaction is now fully proven (three seams: single-resolve
  `StreamReplyCell`, the N-item ring, in-band EoS; `producer_seq` identity; monotone
  credit-return). The item-transport leg was already proven (the shipped 024 ring); the
  remaining blocker — the **015 OPEN-cell re-admit** (`co_await` on-lane resume) clearing an
  ADR-014-grade real-scheduler gate — is now closed on both legs: the mechanism
  (`detail/reply_cell.hpp` routing through `Activation::complete_parked()`, proven exactly-once
  via an ordinary `ask`) AND the ask_stream-specific dedicated run (the OPEN handshake racing
  the ring's first item through a real actor handling `AskStream<Q,F>` via `ActorRef::
  ask_stream<F>`/`.accept()`, 2500/2500 exactly-once, 0 loss — see
  [ADR-018](decisions/ADR-018-outbound-streaming-replies.md)'s second post-decision update).
  `ActorRef<A>::ask_stream<F>(Q)` / `LocalRouter::ask_stream` (the addressing layer this
  section's `ask_stream<F>` sketch describes) are now implemented in `actor_ref.hpp`. **New,
  documented hazard** (see ADR-018's residual risks): a callee `handle(const AskStream<Q,F>&)`
  that does work AFTER `.accept()` must not assume that work happens-before the resumed
  caller's continuation — `ReplyCell::resolve()`'s co_await resume is synchronous and
  stack-reentrant on the resolving (callee's) thread. *Residual open design item: multi-source
  fan-in deriving a reply (017).*
