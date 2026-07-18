# Quark Engine — Overview (Draft)

The master document of the RFC. Subsystem detail lives in `001`–`007`; this file
holds the vision, the principles, and the invariants that every subsystem must
uphold. See [README.md](README.md) for the locked decisions and glossary.

## Vision

Build a high-performance C++ actor engine where the **runtime owns optimization**
and developers express only **intent**.

Principles:

- **Convention over configuration** — sensible defaults; policy only where needed.
- **Fail fast at startup** — misconfiguration is a compile error or a startup
  error, never a runtime surprise.
- **Zero-cost hot path** — no heap allocation, no reflection, no virtual dispatch
  for policy, no dynamic resource resolution while a message is processed.
- **Sequential by default** — an actor processes one message at a time unless it
  explicitly opts out.
- **Stable public API** — the runtime may change its internal implementation
  (scheduling, pooling, codegen) without changing what developers write.

## Core invariants

These hold across the whole engine. A change that violates one is a redesign,
not a tweak.

1. An actor has **at most one executor at any instant**.
2. Mailbox ordering is **FIFO by default**.
3. The scheduler schedules **activations, not individual messages**.
4. **Placement is stable** — `ActorId` deterministically maps to a shard.
5. **Workers are execution lanes, not actor owners** — any worker may execute any
   activation, but none owns an actor.

## How the pieces fit

```
  developer code            runtime (owned by Quark)
  ──────────────            ────────────────────────
  class Order               Shard  ── activation queue ─┐
    : Actor<Order,          Shard  ── activation queue ─┤
        Sequential,           …                         │
        Placement<HashById>,                            ▼
        DrainBudget<64>>    Workers (transient lanes) borrow one
  {                         activation at a time, drain its Mailbox
    void handle(Ship&);     (MessageHandle → Descriptor → Payload),
    task<> handle(Query&);  then release. Cancellation is a state flag.
  };
```

- **Intent** is declared with CRTP **policies** (invariant 3–5 are the runtime's
  side of the contract; policies are the developer's side).
- **Execution** is sequential per actor (invariant 1) and hybrid: sync handlers
  run inline, async handlers (`quark::task<>`) suspend the activation without
  releasing the single-executor guarantee.
- **Memory** is shard-owned and allocation-free on the hot path.
- **Resources** resolve once at activation or via per-message factories — never
  through dynamic lookup while draining.

## Message lifecycle

```
Created → Queued → Running → Completed
                 ↘ Cancelled           (from Queued; a tombstone until dequeued)
```

Cancellation never scans or mutates the queue — it flips a state flag on the
descriptor, and the worker skips tombstones lazily on dequeue.

## Startup pipeline

```
Discovery → Validation → Metadata compilation → Runtime
```

- **Discovery** — registered actor types are collected.
- **Validation** — Strict (fail startup) or Relaxed (warn). Checks missing
  resources, conflicting policies, placement conflicts, scheduler config.
  Results are `std::expected<Metadata, ValidationError>`.
- **Metadata compilation** — per-actor construction factory, resource plan,
  and policy tables are materialized so runtime dispatch is table-driven.
- **Runtime** — no reflection, no policy re-derivation.

(Detailed in `008-Metadata-and-Startup.md`.)

## Division of responsibility

| Developer owns | Runtime owns |
|---|---|
| Business logic (message handlers) | Scheduling & placement |
| Execution hints via policies | Pooling & allocation |
| Resource lifetime declarations | Resource caching & factory wiring |
| Policy *only when the default is wrong* | Metadata compilation & validation |
