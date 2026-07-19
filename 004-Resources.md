# 004 — Resource Model

How an actor obtains the things it needs (loggers, connection pools, caches,
per-message sessions) **without any dynamic lookup on the hot path**. There is no
service container and no scope object handed to handlers; resources are resolved
by *storage location* keyed to a lifetime, and wired at metadata-compilation time.

## Lifetimes

A resource's lifetime determines **where it is stored** and **when it is resolved**:

| Lifetime | Stored in | Resolved | Example |
|---|---|---|---|
| **Singleton** | Engine | Once, at startup | Config, metrics registry |
| **Node** | Per node | Once per node | Machine-wide connection pool |
| **Shard** | Per shard | Once per shard | Shard-local cache, allocator handle |
| **Activation** | Per activation | Once, when the actor activates | Logger, DB connection-pool handle, session factory |
| **Message (Ambient)** | `MessageContext` | Never resolved — carried with the message | `std::stop_token`, deadline, trace id, headers |

Longer-lived scopes are strictly cheaper: a Singleton is a pointer read; an
Activation resource is resolved once and cached on the activation for the actor's
whole lifetime.

## Rules

- **Heavy dependencies resolve at activation**, not per message. A handle to a
  connection pool, a session *factory*, a logger — all resolved once when the
  actor activates and cached on the activation.
- **Per-message resources come from factories.** A resource that must not outlive
  a single message (e.g. a DB session, a scratch buffer) is produced by an
  Activation-scoped factory and destroyed at the end of the message. The factory
  is the cached Activation resource; the product is the per-message resource.
- **A factory failure fails the message** (ADR-009). Each `PerMessage<T>` product is
  **resolved and checked inside the guarded region *before* the handler body runs**, so a
  factory that **throws** and one that **returns `std::unexpected`** are handled uniformly:
  dispatch is skipped, the 007 boundary fires (`ask` → `unexpected(resource_error)`, `tell`
  → dead-letter), and the handler **never runs with a null/degraded resource** (proven,
  D1 C4). A partially-acquired external resource is released by the **product's RAII guard**
  — `Cached<>`-pool checkouts must be RAII-scoped subobjects of the product, not left to
  state rollback. Callers who genuinely want degradation opt into
  `OnResourceFailure<Degrade>` (007), which fires on both the throwing and `unexpected`
  channels (both tagged `FailureSource::Resource`).
- **Ambient values are never "resolved."** Cancellation, deadline, trace id, and
  headers travel inside the `MessageContext` (see below). Handlers read them
  directly; they are not looked up.
- **No dynamic resolution while draining.** The hot path only reads already-cached
  handles and calls factories — never walks a container.

## Declaring resources

Resources are declared as part of the actor's type, so the plan is known at
compile time and materialized during metadata compilation (see `008`, TBD):

```cpp
class Order : public Actor<Order, Sequential> {
    // Cached at activation:
    Cached<Logger>       log_;
    Cached<PgPool>       pool_;      // a connection-pool handle
    // A factory (cached) that yields a per-message product:
    PerMessage<PgSession> session_;  // fresh session per message, from pool_

    void handle(const Ship& s) {
        auto db = session_.get();    // per-message; destroyed after handle()
        log_.info("shipping {}", s.id);
        db.write(/* … */);
    }
};
```

`Cached<T>`, `PerMessage<T>`, and `Ambient<T>` are declarations, not attributes —
they encode the lifetime into the type so the engine can pre-wire construction.
**Member-field declarations are the chosen ergonomics**
([ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)); the
`using resources = ResourceSet<…>` type-list alternative was dropped because members
read better and couple the declaration to its storage — exactly what the
metadata-compilation wiring needs.

### Inbound streams are activation resources (024)

An inbound `Stream<F>` (see [024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md))
is a **`Cached<>`-lifetime activation resource**: its per-stream ring slots, payload
arena, and single reusable `StreamActivationDescriptor` are wired at
metadata-compile time and materialized at **stream-open (cold path)**, so there is
**zero dynamic resource resolution while a frame is processed**. Two slot regimes are
recorded as buffer-ownership variants: **inline** (≤ 56 B frame, lives in the slot)
and **by-reference / registered-RX** (the slot references a transport buffer the
transport may DMA into — zero-copy; its lifetime tied to the credit/read-cursor
advance). This is the resource-layer view of 024's buffer ownership (003).

### Outbound streaming replies are caller-shard resources (ADR-018)

An outbound streaming reply — an `ask_stream<F>` that returns `ReplyStream<F>` — is
the 024 inbound credit-ring **run backward** (callee = producer, caller = consumer),
so its reply ring + slots + payload arena are **caller-shard `pmr`-owned,
pre-allocated cold at `ask_stream` (not per item), pooled and reused** → **0
per-item heap** ([ADR-018](decisions/ADR-018-outbound-streaming-replies.md)).

The footprint trade is **idle density**: this costs a whole ring per *in-flight*
streaming ask (`cap × slot`, e.g. ~16 KB @ cap-256) where an ordinary ask needs only
a **64 B scalar `ReplyCell`**. A fan-out of many concurrent short streaming replies
is therefore heavy, bounded by three levers — a **small default cap** (022
`credit_limit`), a **shard slab** the rings draw from, and an **admission cap on
concurrent streaming asks**. Distinct from the inbound case, the callee's cold
`task<>` frame is **1 alloc/ask** (conceded); an **optional pooled `promise_type`
operator-new** (shard frame-slab) reaches the full 0 without touching the item path.

## Message context (ambient)

Every message carries a `MessageContext`, available to the handler without lookup:

```cpp
struct MessageContext {
    std::stop_token           stop;      // cancellation
    quark::deadline           deadline;  // local monotonic instant; ships as remaining duration across nodes (018)
    quark::trace_id           trace;     // distributed trace correlation
    quark::principal          principal; // authenticated sender identity; attenuates down the causal chain (020)
    quark::header_view        headers;   // small key/value metadata
};
```

- **Sync handlers** access it via `quark::current_context()` (set on the worker
  lane for the duration of the call).
- **Async handlers** access it the same way, or `co_await quark::context()`; the
  context follows the coroutine across suspension.

Ambient values are the only resources that flow *with* a message rather than being
resolved from a scope.

### Context storage — envelope, not inline (ADR-007)

The `MessageContext` is **not** stored inline in the ≤ 64 B descriptor. It lives in the
message's **payload-arena envelope**, referenced by a descriptor `ctx_` pointer. An
*inherited* context resolves to `current_context()` with **zero deref**; an *override*
costs **one pooled-pointer deref**. The `ctx_` pointer rides the same
`next_.store(release)` publication edge as the rest of the descriptor (003), so it needs
no extra atomic.

The running-handler `stop_token` is backed by an **activation-pooled `std::stop_source`**
(sized to the Reentrant/`MaxConcurrency<N>` limit), **never a per-message
`std::stop_source`** — a per-message source allocates one control block, which a positive
control proved is 1 alloc/msg. Queued-message cancellation uses the descriptor's
generation-gated `gen_state` CAS (001/003), not a `stop_source` at all.

## Open questions

- *(Factory error handling: resolved — a `PerMessage<T>` factory failure **fails the
  message** via the 007 boundary, checked before the handler body; degrade is the explicit
  `OnResourceFailure<Degrade>` opt-in. See the factory rule above, ADR-009.)*
- Node/Shard resource resolution ordering vs. actor activation on a cold shard.
