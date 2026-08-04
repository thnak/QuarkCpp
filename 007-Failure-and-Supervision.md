# 007 — Failure and Supervision

What happens when a handler throws, a deadline fires, or an actor cannot make
progress. The current draft has no error model at all — this defines one.

## Failure sources

1. **Handler throws** — a sync handler throws, or an async handler's `task<>`
   completes with an exception.
2. **Deadline exceeded** — the message's `deadline` passes before completion.
3. **Cancellation** — the message's `std::stop_token` fires (caller `ask`
   abandoned, shutdown).
4. **Resource failure** — a `PerMessage<T>` factory fails to produce a resource
   (`004`).
5. **Poison loop** — the same message repeatedly fails after restart.

Failures are **contained to one message and one actor** by default; they do not
crash the worker lane or the shard.

## Per-message outcome

When a message fails, the worker catches it (the hot path is exception-guarded at
the handler boundary only) and records the outcome on the descriptor
(`Running → Completed` with an error state). For an `ask`, the caller receives
`std::unexpected(quark::error{…})` (`006`). For a `tell`, the message is routed to
the **dead-letter** sink with its error and trace id.

The message never silently disappears: every failed message is either reported to
its `ask` caller or dead-lettered.

The **message outcome is recorded before the actor's fate** (`complete_reply` /
`dead_letter` runs first), so the reply cell (ADR-007) is always resolved before any
restart/reconstruct touches state. Both `complete_reply` and the dead-letter `enqueue`
are **statically `noexcept`** (a throwing sink fails to compile) and the dead-letter sink
is a **bounded shed-don't-buffer** sink (022): a full or failing sink sheds-with-metric,
never terminates the lane (ADR-009 S4, proven).

## Transient vs. actor failure — classification first (ADR-009)

Before the supervision decision runs, the failure **source** is classified, because not
every failure should touch the actor's lifecycle
([ADR-009](decisions/ADR-009-failure-supervision-and-recovery-policy-model.md)):

- **Transient** — a **deadline** (#2) or **cancellation** (#3). The message is reported
  (`ask` → `unexpected(deadline_exceeded | cancelled)`, `tell` → dead-letter) and the actor
  **`Resume`s without `quiesce(Cancel)`, without reconstruct, and without charging
  `MaxRestarts`**. This stops transient overload from restart-storming a healthy actor into
  `Stop` (proven: 20% deadline load → 0 restarts, actor not stopped). The carve-out
  disposition is itself a policy slot.
- **Actor failure** — a **handler throw** (#1), **resource failure** (#4), or **poison
  loop** (#5). Only these run the configured `Decision` below.

## Supervision decision

After an **actor failure**, the actor's **failure policy** decides the actor's fate:

| Decision | Effect |
|---|---|
| **`Resume`** | Keep actor state; drop the failed message; continue draining. |
| `Restart` | Reconstruct the actor (fresh state via the construction factory); **keep the mailbox**; re-activate. |
| `Stop` | Deactivate the actor; drain remaining mailbox to dead-letter. |
| `Escalate` | Hand the decision to the node supervisor. |

Default: **`Restart`** with a bounded restart budget (below). `Resume` is the
right default only for actors whose state cannot be corrupted by a partial
handler; that is opt-in.

Declared as a policy (`005`):

```cpp
class Order : public quark::Actor<Order,
                  Sequential,
                  OnFailure<Restart, MaxRestarts<3, Within<seconds<10>>>> {};
```

### `Resume` state rollback (ADR-009)

`Resume` is **assert-intact by default** — no snapshot, no rollback, **zero cost on the
success path** (proven: objdump shows no added alloc/state-branch; guarded local-tell p99
54.4 ns vs 53.5 ns no-guard control, ratio ≤ 1.019). Rollback is **opt-in** via
`Transactional<Off | Snapshot | Journal>`, the **only** policy that copies state — its cost
is ∝ `sizeof(State)` and pays **nothing** unless selected. `Transactional<>` is
**Sequential-only**: `static_assert(!(is_transactional && is_reentrant))` fires at compile
time (Reentrant rollback awaits the 015 COW-snapshot open question).

### Descriptor reclamation — one join point

The `Running → Completed` generation-bump + shard-pool return happens **exactly once**, at a
**single site shared by the success path and all four failure branches** (`Resume` /
`Restart` / `Stop` / `Escalate`) — `do_restart` never reclaims separately. Proven
exactly-once over 4×2M descriptors, TSan+ASan clean; a relaxed-store control trips ASan
double-free.

## Restart budget (poison protection)

To prevent an actor from restart-looping on a poison message:

- Each restart within the window counts against `MaxRestarts`.
- On exhaustion, the actor **escalates** (or `Stop`s, per policy), and the
  triggering message is dead-lettered so it cannot re-poison the fresh actor.

This is why `Restart` **keeps the mailbox** but the *poison message specifically*
is removed — otherwise restart could never converge.

## Escalation — configurable hierarchy (ADR-009)

`Escalate` (or budget exhaustion) raises the failure up a **configurable supervision
hierarchy**, `Supervision<Node | PerType | Tree<…>>` (wired, `Engine::wire_escalation<A>`,
`engine.hpp` — **eager `spawn<A>()` only**; a `declare_lazy<A>()`'d actor does not yet get this
wiring, a documented residual):

- **`Node`** — a single ENGINE-WIDE default supervisor, registered via
  `Engine::set_node_supervisor<Supervisor>(id)`; `Tree` of depth 1. The default.
- **`PerType`** — a supervisor registered per ESCALATING actor type, via
  `Engine::set_type_supervisor<A, Supervisor>(id)` — falls back to the Node default if `A` has no
  dedicated entry.
- **`Tree<S0, S1, …>`** — routes to `S0` (registered via `Engine::set_supervisor<S0>(id)`); **this
  pass wires only the single `S0` hop** — chain-forwarding through `S1..` is NOT implemented. A
  supervisor that wants to escalate further re-`tell`s the next hop itself, from its own
  `handle(const Escalated&)`.

A supervisor is a plain actor — addressed by `ActorId`, reached via an ordinary `tell` — that
`handle(const Escalated&)`s a `{source: ActorId, cause: error}` message (not a Node-scoped `004`
resource, as originally sketched). `escalate()` posts this through the engine's own internal
descriptor pool (mirrors the `Wake` control-descriptor mechanism, never a user-facing
`LocalRouter`, which the fault path has no access to) — a message hop, never a synchronous
cross-lane touch, so single-executor is preserved. The supervisor's OWN policy for what to do with
an `Escalated` (`Stop` the actor, `Stop` a whole shard, trigger a controlled node shutdown,
re-escalate further) is ordinary handler code — the engine imposes none of it. Escalation is the
only path by which one actor's failure can affect another; the **static depth bound**
(`Tree<…>`'s `static_assert(depth ≤ 8)`) is enforced at compile time, and the runtime storm guards
this section originally called for are now **WIRED (2026-08-04)** as `EscalationGuard` — a runtime
parameter to `Engine::set_node_supervisor`/`set_supervisor`/`set_type_supervisor` (all-zero default
= unbounded, unchanged from before this feature): an aggregate 022 token-bucket cap over ALL
escalations reaching one supervisor (a systemic-fault storm — many actors faulting at once), a
per-source sliding-window cap keyed by the escalating actor's own `ActorId` (a respawn→refault
loop — the SAME source escalating repeatedly), and a TTL that stamps the posted `Escalated`
descriptor's deadline so a supervisor configured with 022 `enable_governance(...,
deadline_shed=true)` sheds a since-gone-stale escalation via the EXISTING 018/022 deadline-shed
path rather than new machinery. Every shed is counted exactly via `Engine::escalations_shed()` —
never a silent drop. Not yet closed: the per-source tracking table is bounded (256 distinct
tracked sources per supervisor, oldest evicted on overflow — not a full LRU) and
`declare_lazy<A>()` escalation wiring remains a separate, documented residual. See
`tests/supervision_escalation_storm_test.cpp`.

## `ask` reply on `Restart` (ADR-009)

Knob `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>` (wired, `Activation::handle_restart_retry`,
`activation.hpp`):

- **`Fail`** (default) — the faulting message is dead-lettered before `Restart` runs (`ask` →
  `unexpected(supervised_stop)`, a value, single dispatch, never re-run).
- **`Retry<N, IdempotencyKey>`** — opt-in, **Sequential-only** (`validate_supervision_policies<A>()`
  rejects it on Reentrant/`MaxConcurrency<N>` at registration — mirrors `Transactional<>`/
  `IdleTimeout<Ms>`'s existing Sequential-only restrictions). On a fault, the message is **held, not
  dead-lettered**: the actor is restarted (charging the SAME `MaxRestarts` budget `Restart` uses —
  a retry cannot out-run the actor's own restart budget) and the **same descriptor and payload** —
  including any embedded `ask` `Responder` — is re-dispatched against the fresh instance, up to `N`
  times. A retry that completes resolves the message normally: the same `Responder` replies exactly
  as a first-try success would, so no separate reply-cell reservation/re-stamp machinery is needed
  (the descriptor is simply never reclaimed until the retry loop concludes). Exhausting the budget
  falls back to `Fail` semantics. Scoped to **sync handlers only** in this pass — a retry attempt
  that genuinely suspends (async) abandons the frame (a defined, RAII-safe `.destroy()`, the same
  operation a dropped-while-suspended `task<>` already performs) and counts as a failed attempt;
  async retry support is a documented residual.

## Interaction with execution policies

- **`Reentrant` / `MaxConcurrency<N>`**: multiple handlers may be in flight when
  one fails. `Restart` runs `quiesce(Cancel)` (see
  `015-Reentrancy-and-Quiescence.md`) — the in-flight siblings are cancelled (their
  messages dead-lettered, since they operated on suspect state), then state is
  reconstructed under the quiescence guard, then the seal is released. `Resume`
  and `Stop` use `quiesce(Drain)`/`quiesce(Cancel)` respectively. The single-executor
  safety here comes from the **`Cancelling` seal + the exec-state CAS**, **not** from
  "holding Running" — a reentrant actor **releases Running at `co_await`** (ADR-009 S3
  correction); admission during a restart is blocked by the seal, not by lane ownership.
- **Reconstruct / reload failure**: reconstruction (fresh state) or persistent reload (012)
  returns `std::expected`; a failure **escalates** (Stop + dead-letter survivors **under the
  held seal**) rather than releasing the seal onto empty state, and never crosses the
  `noexcept` cold path (ADR-009 C6, proven: lane survives, survivor never dispatched against
  empty state).
- **Async handlers**: the exception surfaces when the `task<>` completes;
  suspension points inside the handler are unwound by normal C++ coroutine
  exception propagation.

## Broadcast — dead-subscriber pruning (ADR-019)

A `Topic<M>` best-effort broadcast
([ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)) delivers to
subscribers as N ordinary tells sharing one payload — so a dead subscriber is a failure
the *publisher* must never wait on (GATE 1: the publisher never blocks or stalls):

- **Dead subscriber-node** — a copy addressed to a subscriber on a SWIM-`Suspect`/`Dead`
  node (010/021) is **dropped immediately and counted** (`PublishReceipt.dropped_*`), never
  retried and never back-pressured onto the publisher.
- **Dead local subscriber** — resolves through `ActorRef` to the **dead-letter** sink like
  any other tell; the topic does not special-case it.
- **Optional SWIM-driven snapshot prune** — a membership transition may trigger a
  copy-on-write rebuild of the subscriber table that drops entries for `Dead` nodes, keeping
  the immutable snapshot from accumulating tombstones (cold path, off the publish hot path).

The **subscriber mailbox's lifetime is not re-solved by the topic** — it stays governed by
the existing `ActorRef` / 007 discipline above (supervision, dead-letter, restart budget). A
topic is a fan-out addressing layer over `ActorRef`s, not a new lifecycle owner.

## Observability

Every failure emits: `trace_id`, `ActorId`, message type, failure source, and
supervision decision, to the metrics/trace sinks (`009`, TBD). Dead-letters are
themselves observable and optionally replayable.

## Resolved (ADR-009)

- **State rollback on `Resume`** → default **assert-intact** (zero-cost); opt-in
  Sequential-only `Transactional<Off|Snapshot|Journal>`. See *`Resume` state rollback*.
- **Escalation granularity** → configurable `Supervision<Node|PerType|Tree<…>>`, WIRED for eager
  `spawn<A>()` (static depth bound enforced; runtime storm guards — TTL, aggregate + per-source
  rate limiting — WIRED via `EscalationGuard`; `declare_lazy<A>()` not yet wired). See
  *Escalation*.
- **`ask` reply on `Restart`** → `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`, default
  `Fail`, WIRED (Sequential + sync-handler only). See *`ask` reply on `Restart`*.
- **Does `Restart` reload persisted state** → **yes iff `Persistent<…>`** (012): reload via
  `StateStore::load` + fencing-token bump + EventSourced tail replay; non-persistent actors
  reconstruct fresh. Reload returns `std::expected`; a failure escalates. The poison message
  is dead-lettered exactly once either way.
- **`PerMessage<T>` factory failure (004)** → **fails the message** via a handler-authored
  `ProductGuard.acquire_or_throw()` call at this boundary (not an automatic pre-handler pass);
  "degrade" is the explicit `OnResourceFailure<FailMessage|Degrade>` knob, WIRED via
  `resource_failure_degrades<A>()`. See `004`.

## Open questions

- *(All 007 lifecycle open questions are resolved by ADR-009 above. The remaining
  cross-cutting item — lifting `Transactional<>` from Sequential-only to Reentrant via a
  large-state COW snapshot — is tracked in `015`.)*
