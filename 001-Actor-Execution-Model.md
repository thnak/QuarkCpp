# 001 — Actor Execution Model

## Goal

Guarantee **sequential execution per actor** while maximizing throughput across
the machine.

## Invariants (local restatement of the core invariants)

- One actor has **at most one executor at a time**.
- Mailbox is **FIFO by default**.
- The scheduler dispatches **activations, never individual messages**.

> **The one declared exception:** a `Stateless<N>` actor (025) is a *pool* of N
> activations, so it has neither a single activation nor per-key FIFO. This does not
> weaken the invariant — each pool activation is still single-executor and sequential;
> the actor simply declares it holds **no cross-message state and no identity**, and
> the validator (005) enforces that (no persistence, no per-key-order assumption).
> Everything below is about the default, stateful, single-activation actor.

## Lifecycle

```
Idle → Scheduled → Running → Idle
Running → Scheduled          (when the drain budget expires)
```

- **Idle** — no pending messages, no activation queued.
- **Scheduled** — an activation exists in exactly one shard queue.
- **Running** — a worker holds this actor's execution ownership and is draining.

## Activation

An **activation** is the right to execute an actor. At most one activation for a
given actor may be scheduled at any time — this is how invariant 1 is enforced
without per-message locking. Ownership is acquired with a single atomic
compare-exchange on the actor's execution state; a worker that loses the race
does not execute, it just ensures the actor is (re)scheduled.

## Mailbox

- **Intrusive MPSC queue** — the queue node *is* the pooled `Descriptor` (many
  senders, one draining worker). Owns **ordering only** — never payload memory
  (see `003-Memory.md`). FIFO is the modification order of the producers'
  `tail_.exchange`.
- A worker drains until the mailbox reports empty **or** the drain budget is
  exhausted, then releases ownership. If messages remain, it transitions the actor
  back to `Scheduled` and re-enqueues the activation.
- **The exec-state CAS memory orders are load-bearing, not incidental — and the
  same atomic carries *two* obligations with *different* orders** (ADR-003):
  1. **`head_` / `fifo_` handoff** — carrying the consumer-private dequeue cursor
     across a worker handoff. This is a single-writer publish and needs only
     **release** on `Running → Idle` release-ownership and **acquire** on
     `Idle → Running` acquire-ownership. The dequeue side does no atomic RMW of its
     own (003, 023). Downgrading either to `relaxed` reintroduces a data race on
     `head_`: [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md) proves it
     with a positive control (relaxing the CAS makes TSan report a `head_` race
     every run).
  2. **Wakeup rendezvous** with producers — the release-ownership `store(exec_state)`
     followed by a `load(tail_)`, racing a producer's `exchange(tail_)` then
     `load(exec_state)`. This is a **Dekker StoreLoad**, closed by a **`seq_cst`
     fence between the store and the load on each side** — the symmetric
     `store(release) · fence(seq_cst) · load(acquire)` form (002 §close-out; ADR-004).
     Plain release/acquire leaks wakeups here; the producer keeps its cheap `acq_rel`
     exchange (no per-enqueue `seq_cst`). The dramatic ~48% loss needs *both* sides
     downgraded; with the `acq_rel` exchange retained the residual x86 leak is small
     (~0.05–0.09%) but nonzero, so the fence is load-bearing.

  Both the relaxed-CAS `head_` guard and the no-fence Dekker guard (calibrated
  `lost == 0` vs `lost > 0`) are permanent CI regression tests.

## Hybrid handler execution

An actor's protocol is the set of `handle` overloads it declares. The return type
selects the execution mode per message type — detected at compile time, no
runtime branch:

```cpp
class Order : public Actor<Order, Sequential> {
public:
    void        handle(const Ship&);          // sync  — drained inline
    quark::task<> handle(const Query& q);     // async — suspends the activation
};
```

- **Sync handler** — executed directly on the worker lane. Cheapest path; the
  worker moves to the next message with no suspension.
- **Async handler** — returns `quark::task<>`. The worker *suspends the activation*
  at the first `co_await`. **The single-executor invariant still holds**: the
  actor's mailbox does not advance and no other worker may execute it until the
  task completes. The worker lane itself is freed to serve other activations
  while the task is suspended — suspension parks the *activation*, not the thread.

  For a `Sequential` actor this is normative: a drain step dispatches one message
  and returns **`Completed`** or **`Suspended`**; on `Suspended` the worker
  **parks the drain** — it must **not advance the mailbox** to the next message.
  [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md) caught a competing
  design whose drain loop advanced past a suspended handler, breaking
  single-executor; the loop must park, not proceed. When the suspended task
  completes it **re-schedules through the admission gate** (015) rather than
  resuming the drain inline on the completing thread.

Because an async handler holds the actor across suspension points, an actor doing
heavy async I/O naturally serializes its own messages. Use `Reentrant` /
`MaxConcurrency<N>` (see `005`) to relax this deliberately.

**Inbound stream draining obeys the same rule** (024). A stream handler drains a
*batch* off a per-stream ring inside one activation, under the single-executor
invariant; a **suspended** stream handler does **not** advance the drain — the
stream's split `disp`/`tail` cursors freeze at the parked frame until 015 re-admits,
giving exactly-once dispatch (proven: `concur_violations = 0`, no wedge). See
[024-Streaming-and-Inbound-Streams.md](024-Streaming-and-Inbound-Streams.md).

### Execution vehicle — passive + stackless core

The engine's **core execution vehicle is passive + stackless run-to-completion**: an
actor is a passive object (state + mailbox) scheduled onto one of N worker lanes only
when it has messages; **it owns no stack of its own.** A sync handler runs inline on the
worker's own stack; an async handler suspends the *activation*, not the thread, pinning
only its live coroutine frame. This is normative and was proven the best vehicle by
[ADR-015](decisions/ADR-015-actor-execution-vehicle-passive-stackless-vs-fibers.md)
against pooled/​per-actor stackful-fiber alternatives: **192 B per idle actor** (identical
at 10⁶ and 10⁷ actors, ~28× under the 023 footprint ceiling), a depth-bounded suspended
frame (656 B–2.9 KB for nesting depth ≤ 8) that beats a fiber at every such depth with no
guard page, sync p99 96.4 ns / 41.8 M msg/s/core drain with **zero context-switch** and
**zero hot-path allocation**. A green thread pins a stack and a VMA the moment it runs, so
"pool fibers across idle actors to cut cost" gives no density win — it was measured and
**disproven**.

A handler may opt into a fourth compile-time execution mode for work that must call
**un-colorable blocking code** (a legacy/foreign-C leaf that cannot become a coroutine):

```cpp
quark::blocking<Result> handle(const Decode&);   // thread-backed (default), asm-free
quark::fiber<Result>    handle(const Parse&);    // stackful, gated — un-colorable chains
```

Both suspend the activation **exactly like `quark::task<>`** — single-executor and mailbox
FIFO hold via an explicit `Parked` seal (002) and 015 re-admission — but offload the
un-colorable leaf **off the mailbox lane**, freeing it to serve other actors (a co-resident
actor ran at 9.31 M msg/s during a 50 ms blocking call). This is an **opt-in, off-hot-path,
per-blocking-invocation adapter — never on the hot path, never per-actor.** Idle and
async-suspended actors still pin no fiber; the stackful `quark::fiber<>` form owns a 16 KiB
stack **per in-flight call** (P-capped, 022-shed), and its stackful asm + sanitizer shims are
confined to the PAL (019). Its measured, physically-unique win: it suspends an un-colorable
foreign-C call chain in place with all locals byte-intact (the stackless `co_await`
equivalent is a compile error). Detailed trade surface and budgets: ADR-015, 002, 015, 023.

## Cancellation

A queued message may transition `Queued → Cancelled` by a **generation-gated CAS**
on its descriptor's packed `gen_state` word (48-bit generation; see `003-Memory.md`
for why 48 and why one packed word). **No queue scan, no removal.** The worker skips
cancelled entries lazily when it reaches them during drain; each skip **counts
against the drain budget** (002) so a mass-cancel cannot monopolize the lane.
In-flight (`Running`) messages observe cancellation through the `std::stop_token` in
their `MessageContext`.

**Claim-CAS vs. late-cancel race.** When the drain claims a message
(`Queued → Running` CAS) it can lose the race to a cancel that flips the same word
first. The rule (ADR-004): if the claim-CAS fails because the message was cancelled
under it, the drain **reclaims it as a tombstone** — one free, no handler runs, no
second reclaim — exactly as if the skip had been observed first. So a late cancel
racing the claim is always resolved to a single clean reclamation, never a
double-free and never a handler on cancelled state.

## Reentrancy

Disabled by default (`Sequential`). An actor opts in via the `Reentrant` policy,
which permits the worker to begin the next message while an async handler is
suspended, or `MaxConcurrency<N>` to cap the number of concurrently in-flight
handlers. These policies change execution semantics only — placement, ordering of
*enqueue*, and mailbox ownership are unchanged.

**Crucially, reentrant handlers interleave only at `co_await` points.** The
synchronous regions between suspension points still run mutually exclusively on
the actor's lane, so there are no data races on actor state — a reentrant actor is
cooperatively scheduled on a single lane. The full model, and the **quiescence**
primitive that `Restart` (007), snapshots (012), budget (002), and memory
reclamation (003) all build on, is defined in `015-Reentrancy-and-Quiescence.md`.

## Reply ordering for concurrent `ask`s

Resolved by [ADR-007](decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md):

- **Requests** always stay **mailbox-FIFO** — concurrency never reorders what the actor
  *receives*.
- **Replies** follow the execution policy:
  - **Sequential** → replies are delivered in **request order** (handlers complete in
    request order anyway).
  - **Reentrant** / `MaxConcurrency<N>` → replies are delivered in
    **handler-completion order** (a fast handler that started later may reply first).
- Each `ask` is an independent **one-shot** routed through a **shard-pooled,
  monotonic-generation `ReplyCell`** — never the caller's frame. Completion and
  suspension race a single **win-arbitration CAS** followed by a **release-publish**
  handshake, with the awaiter as the sole cell-release point; this closes the
  UAF/ABA/lost-wakeup window a naïve deposit-then-CAS would open (proven under
  ASan+TSan: ABA 200k, lost-wakeup 2M, Reentrant-cancel K=64 — all clean).
- Reply delivery **re-admits through the 015 gate**, never inline on the completing
  lane, so it cannot re-enter the target actor.

## Open questions

- (Reply ordering for concurrent `ask`s is resolved above — ADR-007.)
- (Reentrancy fairness and quiescence are resolved in `015`.)
