# 005 — Developer Model

## Philosophy

The developer declares **intent**; the engine chooses the implementation. Intent
is expressed with **CRTP policy types** on the `Actor` base — checked at compile
time, compiled to metadata at startup, zero-cost at runtime. There are no
attributes and no reflection.

## Defining an actor

```cpp
class Order : public quark::Actor<Order,
                  Sequential,               // execution policy
                  Placement<HashById>,      // placement policy
                  DrainBudget<64>>          // scheduling policy
{
public:
    using protocol = quark::Protocol<Ship, Query>;  // the dispatched message set

    void          handle(const Ship&  s);   // sync handler
    quark::task<> handle(const Query& q);   // async handler (opt-in)
};
```

- The first template parameter is the derived type (CRTP).
- The rest are **policies**. Order is irrelevant; each policy slot has a default,
  so an actor with no policies (`Actor<Order>`) is fully valid and sequential.
- Handlers are ordinary member functions; the set of `handle` overloads is the
  actor's **protocol**, enumerated in `using protocol = quark::Protocol<…>` (see
  `006` and the dispatch section below).

## Policy catalog

Every policy is a type. Defaults in **bold**.

### Execution

| Policy | Meaning |
|---|---|
| **`Sequential`** | One message at a time (default). |
| `Reentrant` | Begin the next message while an async handler is suspended. |
| `MaxConcurrency<N>` | Cap in-flight handlers at `N`. |
| `Stateless<N>` | **Pool** of up to `N` local activations, load-routed, no identity, no per-key FIFO, non-durable — opts out of single-activation for stateless workers (see [025](025-Placement-Policies-and-Stateless-Workers.md)). |

### Placement

A placement policy is a **strategy** plus optional **modifiers** that narrow/bias the
candidate nodes — the developer's levers to optimize for capability, capacity,
locality, or affinity. Modifiers resolve against the gossiped membership+capability
set, so they stay **deterministic** (010, 025). Full model in
[025-Placement-Policies-and-Stateless-Workers.md](025-Placement-Policies-and-Stateless-Workers.md).

| Strategy | Meaning |
|---|---|
| **`Placement<HashById>`** | Stable HRW hash of `ActorId` → node → shard (default). |
| `Placement<Explicit>` | Caller-specified shard/node. |
| `Placement<Custom<F>>` | User function `F(ActorId, capability-annotated membership) → node`. |

| Modifier | Effect |
|---|---|
| `Require<Cap…>` | Hard constraint — only nodes with all caps are eligible. |
| `Prefer<Cap…>` | Soft preference — rank preferred, fall back if none. |
| `Weighted` | Capacity-weighted HRW by each node's `weight`. |
| `LocalFirst` | Prefer the calling node if eligible (latency). |
| `Affinity<A>` / `AntiAffinity<A>` | Co-locate with / place away from actor `A`. |

### Scheduling

| Policy | Meaning |
|---|---|
| `Priority<P>` | Names a compile-time priority **class** consumed by the engine-level `PriorityBands<K, Anti>` scheduling policy (default `UniformFIFO`, [ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md)). Priority orders activations **across** actors and **never** reorders an actor's own mailbox. Band is constant per actor type (startup-resolved, no per-message recompute). |
| `DrainBudget<N>` | Max messages drained before yielding the lane. |

### Lifecycle

| Policy | Meaning |
|---|---|
| `KeepAlive` | Never deactivate on idle. |
| `IdleTimeout<Ms>` | Deactivate after `Ms` idle. |

`KeepAlive` / `IdleTimeout<Ms>` compile into per-type `LifecyclePolicy` fields that are
**Live-reconfigurable operational knobs** ([ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md)):
the idle timeout is encoded as **coarse wheel ticks** in the packed operational word (013),
with `idle_ticks == 0` the **`KeepAlive` sentinel**. A live `IdleTimeout` change is applied
to the **existing idle population** by a shard-local **reconcile sweep** on the 011 wheel
(cancel armed timers on a `KeepAlive` transition; arm timers for currently-idle activations
on a timeout transition) — "takes effect at next arm" alone is insufficient. Per-type /
per-instance overrides of `drain_budget` / `idle_timeout` are honored on the hot path via
**tiered resolution** (one load from the owning tier), never re-derived per message.

### Resources

Resource *lifetimes* (`Cached<T>`, `PerMessage<T>`, `Ambient<T>`) are declared as
**member fields** on the actor (`Cached<Logger> log_;`), as shown in `004`. This is
the decided ergonomics ([ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)) —
the `using resources = ResourceSet<…>` type-list alternative was dropped; member
fields read better and couple the declaration to its storage, which is what the
metadata-compilation wiring needs anyway. They are lifetime declarations, not
scheduling policies.

## Handler dispatch

Resolved by [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md)
(D1 JumpTable-Dispatch, proven): dispatch is a **dense per-actor jump-table**, not a
runtime type-id scan and not a virtual call.

- Each actor enumerates its protocol: `using protocol = quark::Protocol<M1, M2, …>;`.
- `slot_of<A, M>` is a `consteval` **dense index** into that list. The send site — where
  the message type is statically known — stamps a 2-byte `msg_slot_` into the descriptor.
- The drain does **one** `.rodata` `static constexpr std::array<Thunk, k>` indexed
  indirect call, `thunks[msg_slot_](self, payload, ctx)`. No RTTI, no vtable, no map
  lookup (objdump-confirmed: a single `call *(%rax,%rcx,8)`).
- Measured: dispatch step p50 ≈ 20 ns / p99 ≈ 25 ns; the whole sync `tell`
  enqueue→dequeue→dispatch is p50 ≈ 47 ns / p99 ≈ 62 ns, 0 alloc — inside the 023
  100/250 ns budget. Code size is **≈ 260 B/actor, linear, independent of the
  engine-wide message count** (a plain fn-ptr table, not a vtable). It beats 008's
  sorted-flat runtime scan on the local path and ties a hand-written `switch` on both
  uniform and 95%-skewed message mixes.
- The 008 `type_index → thunk` sorted-flat scan is **retained only** for the
  non-statically-typed path (wire arrival / any future untyped forward). The dense slot
  is **process-local**, renumbers when handlers change, and is **never serialized**.

## Validation (fail-fast)

Policies and resources are validated during startup, before any message runs.
Some checks are compile-time (a malformed policy list won't compile); the rest run
in the **Validation** phase and return `std::expected<Metadata, ValidationError>`:

- conflicting execution policies (`Sequential` + `Reentrant`),
- placement conflicts,
- **`Stateless` conflicts** — `Stateless` + `Placement<Explicit>` (a pool has no
  fixed node), `Stateless` + persistence/durable state, or code that assumes per-key
  FIFO to a stateless pool (025),
- a handler needing a resource that is not declared,
- **protocol-membership drift** (compile-time, ADR-007) — a message type that has a
  `handle` overload but is **omitted from `using protocol = Protocol<…>`** is a compile
  error at the send site (`Handles<A,M> := InProtocol<A,M> && has_handle<A,M>`), not a
  silent runtime unreachable. The `QUARK_PROTOCOL` macro co-locates the list with the
  handlers and a lint flags the reverse drift (listed-but-unhandled),
- **scheduling misconfiguration** — including (ADR-010): distinct `Priority<P>` classes
  must be **≤ K** bands; "disable priority" must resolve to the `UniformFIFO` **type**, not
  `PriorityBands<1>` (which is not zero-cost); and for `WeightedDRR`, per-actor
  `DrainBudget<N>` must be **≤ the class quantum** (the deficit-carry self-correction does
  not hold otherwise).

Modes: **Strict** (fail startup) or **Relaxed** (warn and continue). See
`008-Metadata-and-Startup.md` (TBD).

## Metadata

Each actor compiles to a metadata record before runtime:

- construction factory (how to build the actor + wire cached resources),
- resource plan (what to resolve at activation, what factories to hold),
- placement policy,
- execution policy,
- scheduling policy.

Runtime dispatch reads these tables. **No reflection, no RTTI, no policy
re-derivation on the hot path.**

## Resolved

- **Resource-declaration ergonomics** → member fields (`Cached<Logger> log_;`).
  ADR-007. The `ResourceSet<…>` type-list alternative was dropped.
- **`handle` dispatch** → dense per-actor jump-table keyed by a compile-time
  `slot_of<A,M>` (see *Handler dispatch* above). ADR-007. Generated-switch was tied on
  the hot path but needs the full message set at the definition site; a CPO/`tag_invoke`
  scheme needs no protocol list but its "zero-indirect" edge was **disproven** (one
  indirect call remains on both toolchains), leaving it at parity with a larger
  per-`(A,M)` code-bloat tax.

## Open questions

- Naming: `tell`/`ask` verbs and whether replies are `std::expected` (reply-type and
  `ask`-shape are otherwise resolved in `006` — async-only, `task<expected<R,error>>`).
