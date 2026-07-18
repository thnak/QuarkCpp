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
hierarchy**, `Supervision<Node | PerType | Tree<…>>`:

- **`Node`** — a single node supervisor (a Node-scoped resource, `004`); `Tree` of depth 1.
  The default.
- **`PerType`** — a supervisor per actor type.
- **`Tree<…>`** — a typed supervisor tree.

The node supervisor's policy may `Stop` the actor, `Stop` a whole shard, or trigger a
controlled node shutdown. Escalation is the only path by which one actor's failure can
affect another, and it is **bounded three ways** so it can never storm:

- **static depth** — the tree depth is consteval-bounded (`static_assert ≤ 8`, acyclic by
  construction);
- **runtime/reconfig cycles + fan-in** — a per-message `escalation_ttl` and a per-supervisor
  `MaxRestarts` / `Within` window bound what the consteval check cannot see;
- **aggregate storms** — capped by 022 **per-shard-local** token buckets (no global atomic,
  consistent with the 0-cross-core-RMW drain gate).

`escalate()` **tells** the supervisor's lane — a message hop, never a synchronous cross-lane
touch — so single-executor is preserved (ADR-009 C1/C3, proven: ttl-bounded cycle,
per-shard rate limiter caps 10k actors, hop depth ≤ chain).

## `ask` reply on `Restart` (ADR-009)

Knob `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`:

- **`Fail`** (default) — the in-flight `ask` that triggered the restart resolves to
  `unexpected(error::restarted)` (a value, single dispatch, never re-run). Proven clean.
- **`Retry<N, IdempotencyKey>`** — opt-in; **`static_assert`-requires an idempotency fence**
  (or EventSourced command-dedup). It **reserves the pooled `ReplyCell`** (Armed → Retained
  CAS) **before** the restart window so a racing deadline/cancel completer cannot recycle the
  cell, re-stamps a fresh descriptor before the poison is freed, and resets `next_ = nullptr`
  on re-enqueue (Vyukov contract). Proven UAF/ABA-clean over 2M asks; a fenced retry commits
  its durable effect **exactly once** (vs twice unfenced).

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

## Observability

Every failure emits: `trace_id`, `ActorId`, message type, failure source, and
supervision decision, to the metrics/trace sinks (`009`, TBD). Dead-letters are
themselves observable and optionally replayable.

## Resolved (ADR-009)

- **State rollback on `Resume`** → default **assert-intact** (zero-cost); opt-in
  Sequential-only `Transactional<Off|Snapshot|Journal>`. See *`Resume` state rollback*.
- **Escalation granularity** → configurable `Supervision<Node|PerType|Tree<…>>`, bounded by
  static depth + `escalation_ttl` + per-supervisor `MaxRestarts` + 022 rate limiter. See
  *Escalation*.
- **`ask` reply on `Restart`** → `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`, default
  `Fail`. See *`ask` reply on `Restart`*.
- **Does `Restart` reload persisted state** → **yes iff `Persistent<…>`** (012): reload via
  `StateStore::load` + fencing-token bump + EventSourced tail replay; non-persistent actors
  reconstruct fresh. Reload returns `std::expected`; a failure escalates. The poison message
  is dead-lettered exactly once either way.
- **`PerMessage<T>` factory failure (004)** → **fails the message** via this boundary,
  resolved+checked **before** the handler body (handler never runs degraded); "degrade" stays
  expressible as the explicit `OnResourceFailure<FailMessage|Degrade>` knob. See `004`.

## Open questions

- *(All 007 lifecycle open questions are resolved by ADR-009 above. The remaining
  cross-cutting item — lifting `Transactional<>` from Sequential-only to Reentrant via a
  large-state COW snapshot — is tracked in `015`.)*
