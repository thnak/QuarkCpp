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

- **Backpressure**: what does `tell` do on a full bounded mailbox — block the
  sender, return `std::expected` failure, or drop with a policy? The per-actor
  `Overflow<Block|Fail|DropOldest>` policy is the leaning here, and it is the
  **foundational lever** of the whole-engine overload model — bounded mailboxes
  generalized to every exhaustible resource, plus rate limiting, deadline-aware load
  shedding, and circuit breaking — in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md).
- **Streaming replies**: `ask` returning `std::generator<>`/a stream for multi-item
  responses. *(The **inbound** direction is resolved — a `StreamRef<F>` handle +
  `handle(StreamBatch<F>&)` drain overload over the credit-ring of
  [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md). The
  **outbound** `ask`-returns-a-stream case remains open; the credit-ring is reusable
  for it but the reply-routing interaction is unspecced.)*
