# 015 — Reentrancy and Quiescence

**Cross-cutting resolution.** Reentrancy (001) was the single hazard threading
through five specs — drain-budget accounting (002), payload reclamation (003),
`Restart` (007), and persistence commit/snapshot ordering (012). Instead of each
spec inventing its own "wait for the other handlers" logic, this spec defines the
model once. The other specs reference it and drop their local hand-waving.

## The execution model, made precise

Everything below depends on stating exactly what reentrancy permits.

- **`Sequential`** — at most one handler in flight per actor.
- **`Reentrant` / `MaxConcurrency<N>`** — multiple handlers may be **suspended**
  concurrently, but **the synchronous regions between `co_await` points run
  mutually exclusively on the actor's lane.** Handlers interleave *only at
  suspension points*.

The consequence is the whole reason this is tractable:

> There are **no data races** on actor state. A reentrant actor is cooperatively
> scheduled on a single lane; only one handler executes synchronous code at a
> time. What reentrancy introduces is *logical* interleaving (a handler reads
> state, awaits, a sibling mutates state, the first resumes with stale
> assumptions) and *lifetime* overlap (several payloads and several live coroutine
> frames referencing state at once).

Quiescence therefore is a **cooperative admission barrier**, not a lock. It never
holds a mutex across a suspension point — the classic coroutine deadlock — because
it excludes *new* handlers and waits for existing ones to leave, rather than
serializing them.

### Definitions

- **in-flight set** — handlers that have started but not completed.
- **admission** — starting a new handler (dequeue from mailbox + begin executing).
- **quiescent point** — an instant when the in-flight set is empty *and* admission
  is sealed, so it stays empty until released.

## Admission control — the shared mechanism

Every "stop starting handlers" need in the engine is the **same gate** in one of
four seal states:

| Seal state | New admissions | In-flight handlers | Used by |
|---|---|---|---|
| **Open** | allowed | run normally | steady state |
| **Paused** | deferred; activation rescheduled | run to completion | drain budget (002) |
| **Draining** | sealed | **awaited** to completion | graceful quiesce: snapshot (012), deactivate (005), planned migration (010) |
| **Cancelling** | sealed | `stop_token` fired, then unwound | failure quiesce: `Restart` (007), forced shutdown |

This unifies two things that looked separate: the **drain budget** is just a
*temporary* `Paused` seal that reschedules the activation; **quiescence** is a
`Draining`/`Cancelling` seal that additionally waits for the in-flight set to
empty. Same gate, different exit condition. FIFO mailbox order (core invariant 2)
is preserved through every seal — queued messages stay queued in order and resume
on release.

### The drain invariant holds across async completion

The single-executor drain invariant (001) is preserved not only *within* a drain
but *across the suspension of an async handler*. When a suspended `quark::task<>`
completes, it **re-schedules the activation through the admission gate** — it does
**not** re-enter `drain()` on the completing thread. So the completing thread never
becomes a second executor of the actor: it hands the actor back to the scheduler
(002), which acquires ownership afresh via the exec-state CAS. This is what makes
the mailbox's consumer-private `head_` safe to leave un-atomic across the
suspension (001, [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md)).

The `Paused` seal also **ties into the mailbox `Busy`/bounded-spin park path**
(002): when a drain step reports `Busy` (a producer mid-publish) or the budget
seals `Paused`, the worker parks and re-schedules rather than spinning unbounded,
so consumer progress cannot collapse into a busy-wait under same-core
oversubscription. Admission control and the scheduler's park path are the same
"stop making progress *right now*, resume cleanly later" mechanism.

> **Rejected alternative (ADR-003/004).** A challenger mailbox (REX, LIFO-push +
> reverse) was proven to have a **head-of-line freeze**: a single producer preempted
> in its ~1 ns publish gap froze the *entire* actor's dispatch — including the
> drainable prefix of already-linked messages — for a scheduler quantum (~708 µs
> measured, over the 023 5 µs p99 ceiling).
>
> **Honest correction (ADR-004):** the incumbent intrusive-Vyukov mailbox is *better
> but not perfect* here. An earlier claim that its mid-publish window "stalls only the
> newest node" is **false in general** — a producer preempted between its
> `tail_.exchange` and its link store strands the **entire linked suffix at and after
> that node**; strictly-older already-linked work still drains from `head_`
> unobstructed. So Vyukov stalls a *suffix*, REX froze the *whole actor* (prefix
> included) — Vyukov is strictly better but the guarantee is "the drainable prefix
> always makes progress," not "only one node stalls." This is a tail exposure under
> `P ≫ cores` oversubscription, not a correctness bug; the mitigation is the mailbox
> `Busy` result + 002 bounded-spin-then-reschedule (and 022 admission). Preserve the
> `Busy`/bounded-spin path in any future mailbox change.

## The quiescence primitive

```cpp
enum class QuiesceMode { Drain, Cancel };

quark::task<QuiescenceGuard> quiesce(Activation&, QuiesceMode);
```

Protocol:

1. **Seal** the actor (`Draining` or `Cancelling`). No new handler is admitted.
2. **(Cancel only)** fire the `std::stop_token` on each in-flight handler's
   `MessageContext` (004), requesting cooperative cancellation.
3. **Await** the in-flight count to reach zero.
4. Resolve to a **`QuiescenceGuard`** — while it is held, the caller has exclusive
   access to actor state, guaranteed no handler is running.
5. **Release** (guard destruction) clears the seal; the worker resumes admitting
   queued messages in FIFO order.

### Zero cost for the common case

A `Sequential` actor is **always at a quiescent point between messages** — the
in-flight set is empty by construction whenever the worker is between drains. So
`quiesce()` on a sequential actor completes synchronously and the guard is a
no-op. The counter-and-waiter machinery is only instantiated for `Reentrant` /
`MaxConcurrency<N>` actors. Sequential actors pay nothing.

### Implementation sketch

On the activation: an `in_flight` counter (incremented at admission, decremented
at handler completion), a seal state, and a single waiter continuation. When a
handler decrements the counter to zero *and* a seal is set, it resumes the waiter.
No allocation on the sequential path; one small waiter frame on the reentrant path.

## How the dependent specs build on this

- **001 (execution):** reentrancy interleaves only at `co_await`; the precise
  model lives here.
- **002 (budget):** the drain budget is the `Paused` seal. Budget counts
  **admitted** handlers per scheduling turn, not suspended ones — a suspended
  handler frees the lane and consumes no further budget. Resolves *"count messages
  started vs. handler time."*
- **001/002 (blocking/fiber adapter, ADR-015):** a `BlockingHandler`/`FiberHandler`
  in-flight call is a **sealed admission** — the actor sits in the non-`Idle` `Parked`
  exec-state (002) and is **registered in the in-flight set** for the whole offloaded call.
  So quiescence and the bounded-quiescence watchdog **see it**: a dropped carrier wakeup
  surfaces as a *stuck in-flight handler* (caught by the watchdog escalation below), never a
  silent "Running-but-nobody-home" wedge. The stackful form takes **origin-worker-pinned
  resume** — a foreign frame may hold thread-affine state (pthread mutex owner, `errno`/TLS),
  so cross-worker resume is UB. The park/resume rendezvous reuses the **ADR-007 ReplyCell
  win-arbitration** pattern (a `FiberParkCell`): exactly one of {carrier completes, actor
  parks} wins the cell, so a completion that races the park is resolved cleanly (a broken
  arbitration was proven to wedge, `rc124`).
- **003 (memory):** payloads have overlapping lifetimes, so they are freed
  **per message** (pool semantics, on the descriptor's transition to
  `Completed`/`Cancelled`), never by bulk drain-step reset. A quiescent point
  (in-flight = 0) additionally *permits* an optional arena bulk-reset as an
  optimization. Resolves *"payload arena reclamation under reentrancy."*
- **007 (Restart):** `Restart` runs `quiesce(Cancel)` — in-flight siblings are
  cancelled (their messages dead-lettered, since they operated on suspect state),
  then state is reconstructed under the guard, then the seal is released. Resolves
  *"Restart must quiesce in-flight handlers before reconstructing state."*
  - **Sibling reply cells complete before teardown (ADR-009 S2):** each cancelled sibling
    `ask` **unwinds through its own handler-boundary guard** and completes its reply cell
    with `unexpected(cancelled)`. The existing *"await `in_flight == 0` **before**
    reconstruct"* step is exactly what guarantees every sibling cell is resolved before state
    teardown — **no caller hangs** (proven: resolved == K over K=64 siblings).
  - **Restart-episode marker (ADR-009 S3):** a sibling that **faults during the
    seal→quiescence window** (before it observes its cooperative `stop_token`) is
    dead-lettered as its cancellation outcome and returns **without** re-incrementing
    `restart_count` or launching a nested restart — this protects the `MaxRestarts` bound
    (a control without the marker double-restarts 2946/2000).
- **012 (persistence):** commit steps execute in the synchronous region at handler
  completion, on the single lane, so they are **naturally serialized**; each commit
  takes a monotonically increasing commit sequence number. Per-event appends need
  no quiescence. A consistent point-in-time **snapshot** uses `quiesce(Drain)`.
  Resolves *"reentrant persistence commit ordering."*

## Self-debate

### Drain vs. Cancel — which is the default?

- **Draining** lets in-flight handlers finish. Graceful, but a caller that needs a
  barrier *because state is broken* (Restart) would be letting handlers run
  against suspect state, possibly committing bad effects.
- **Cancelling** aborts in-flight work. Correct when state is suspect, hostile when
  it isn't (you'd kill healthy user work just to checkpoint).
- **Decision:** `Drain` is the graceful default (snapshot, deactivation, planned
  migration). `Cancel` is reserved for **failure-driven** quiescence (`Restart`)
  and forced shutdown, where in-flight handlers are presumed to hold references to
  suspect state and must be unwound.

### Why not a mutex / per-spec solution?

A mutex around handlers would serialize them, destroying the throughput that
reentrancy exists to provide, and holding it across a `co_await` is a textbook
coroutine deadlock. Quiescence is deliberately *not* mutual exclusion: it is an
**admission barrier** that blocks only new starts and drains the existing set,
holding no lock across suspension. One primitive, built once, beats five
subsystems each re-deriving a subtly different version.

### Bounded quiescence (no unbounded stall)

A stuck or slow in-flight handler could stall `Drain` forever. **Decision:**
`quiesce(Drain)` carries a deadline (from the caller, e.g. a shutdown budget). On
expiry it **escalates to `Cancel`** — firing the in-flight `stop_token`s. If a
handler ignores cooperative cancellation past a second deadline, the failure
escalates to the node supervisor (007). Quiescence therefore always terminates.

### Guard-reentry hazard

While an actor is `Draining`/`Cancelling`, its own critical section must not
`ask` itself — the actor is sealed, so a self-directed round-trip would deadlock
against the barrier. **Rule:** quiescent critical sections perform *state*
operations only (reconstruct, snapshot, serialize), never message round-trips to
the sealed actor. Enforced by construction where possible (the guard exposes state
access, not an `ActorRef` to self).

## Open questions (narrowed)

- **Copy-on-write snapshots** for large-state actors, to take a consistent
  snapshot without a `Drain` stall — snapshot the state version, let handlers
  continue against a new version. Worth it only above a state-size threshold. **This is
  also the precondition for lifting 007's `Transactional<>` rollback from Sequential-only
  to Reentrant actors** (ADR-009): Reentrant rollback needs a per-version COW snapshot, not
  a single per-message copy.
- **Admission fairness** under `MaxConcurrency<N>`: strictly FIFO admission vs.
  `Priority<P>`-aware admission (002) when a slot frees.
- **Drain→Cancel escalation deadline**: a per-actor policy, or a single engine
  default fed from shutdown/migration budgets.
