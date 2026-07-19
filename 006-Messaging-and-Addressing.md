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
- Reply delivery does **not** re-enter the target actor; it re-admits through the 015
  gate, never inline on the completing lane.

### Reply type and errors

An `ask` handler's result is delivered as `std::expected<Reply, quark::error>` so
that failure (handler threw, actor stopped, deadline exceeded — see `007`) is a
value, not an exception across the boundary:

```cpp
std::expected<Confirmation, quark::error> r = co_await order.ask<Confirmation>(q);
```

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
- **Streaming replies** — **resolved in mechanism ([ADR-018](decisions/ADR-018-outbound-streaming-replies.md)), Draft pending the 015 OPEN re-admit gate.**
  `ask` returning a stream for multi-item responses is the **024 credit-ring flipped**
  (callee = producer, caller = consumer) — see `ask_stream<F>` above. The **inbound**
  direction was already Accepted (a `StreamRef<F>` handle + `handle(StreamBatch<F>&)` drain
  over the credit-ring of
  [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md)); the
  **outbound** reply-routing interaction is now specced (three seams: single-resolve
  `StreamReplyCell`, the N-item ring, in-band EoS; `producer_seq` identity; monotone
  credit-return). The **item-transport leg is proven** (it is the shipped 024 ring), but
  006 outbound stays **Draft** until the **015 OPEN-cell re-admit** (`co_await` on-lane
  resume) clears an ADR-014-grade real-scheduler gate — the same unfinished seam an
  *ordinary* `ask`'s OPEN handshake still inherits (`detail/reply_cell.hpp`). *Residual
  open design item: multi-source fan-in deriving a reply (017).*
