# 011 — Timers and Scheduled Work

Delayed and periodic sends, plus the deadline mechanism that 001/007/009 depend
on. Timers never run user code off-lane: a timer firing simply **`tell`s** a
message, preserving the single-executor invariant.

## Data structure: hierarchical timing wheel

Each shard owns a **hierarchical timing wheel** (Varghese & Lauck) keyed on
`steady_clock`:

- O(1) insert and O(1) per-tick expiry for the common case (many short-lived
  timers — every message deadline is a timer).
- Cache-friendly array-of-buckets, no pointer chasing per tick.
- Single-writer per shard (the shard's worker), so **no locks**.

A small **heap overflow tier** holds far-future timers that exceed the wheel's
span, promoted into the wheel as time advances.

### Alternatives considered

- **`std::priority_queue` / binary heap**: O(log n) insert/expire and pointer
  chasing; fine for few timers but deadlines make timers high-volume. Kept only as
  the overflow tier for sparse far-future entries.
- **Single global timer thread + one wheel**: cross-thread enqueue contention and
  wakeup storms. Rejected — per-shard wheels keep timers in the actor's locality
  domain.
- **Decision:** per-shard hierarchical wheel + heap overflow tier.

## Advancing the clock

Hybrid, to avoid both an extra thread per shard and timer lag on idle shards:

- **Active advance:** a worker advances its shard's wheel between drains, so busy
  shards fire timers with no extra thread.
- **Timekeeper:** one low-frequency node-level timekeeper thread ensures **idle**
  shards still fire due timers, using the targeted-wakeup mechanism (002) — it
  wakes exactly the shard whose next timer is due, never a broadcast.

### Alternatives considered

- **Dedicated timer thread per shard**: doubles thread count, adds cross-thread
  wakeups into the shard.
- **Purely worker-driven**: idle shards would never fire timers until the next
  message — unacceptable for deadlines on quiet actors.
- **Decision:** worker-driven when active, single timekeeper backstop for idle
  shards via targeted wake.

## API

Scheduled work is expressed as a future `tell`:

```cpp
// one-shot: deliver Msg to self after a delay
quark::TimerHandle h = self().schedule_after(500ms, Reminder{...});

// periodic
quark::TimerHandle p = self().schedule_every(1s, Tick{});

// to another actor
other.schedule_after(deadline, Msg{...});

h.cancel();   // lazy cancellation (below)
```

A timer firing enqueues its message on the **target actor's mailbox** through the
normal path (006), so ordering, placement, and the single-executor invariant all
hold. Timer callbacks are *messages*, never arbitrary lambdas run on the timer
lane — this keeps all user code on the actor's lane.

## Deadlines

Every message carrying a `deadline` (004) registers an entry in the receiving
shard's wheel at enqueue. On expiry before completion:

- a `Queued` message flips to `Cancelled` (a tombstone, 001/003 — no queue scan);
- a `Running` message observes cancellation through its `std::stop_token`;
- the outcome is a deadline failure (007) and increments `deadline_misses` (009).

A deadline is a **local monotonic instant** on this node's clock. When a deadline
crosses a node boundary the transport ships the *remaining duration* and the
receiver reconstructs a local instant — so a wheel entry is always local. The
cross-node translation rules (transit accounting, inheritance, suspend semantics)
are defined in `018-Clocks-and-Deadlines.md`.

## Cancellation

Consistent with the engine-wide **lazy-cancellation** philosophy: `cancel()` flips
a flag on the timer entry. The wheel skips cancelled entries when their bucket
fires rather than removing them eagerly, keeping insert/cancel O(1) and lock-free.
Periodic timers stop rescheduling once cancelled.

## Idle-timeout deactivation (ADR-008)

Activation-lifecycle deactivation (`IdleTimeout<Ms>` / `KeepAlive`, 005) is driven by the
per-shard wheel, resolved by
[ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md):

- On a drain **Empty** edge, the worker reads `idle_ticks` from the packed operational word
  (013) and **arms a per-activation `Deactivate` entry** (single-writer, lock-free);
  `idle_ticks == 0` (`KeepAlive`) arms nothing. The timekeeper backstop covers idle shards.
- On fire the wheel `tell`s an internal `Deactivate` on the actor's **own lane**, preserving
  single-executor. The `Deactivate` handler evicts **only after a `seq_cst` close-out**
  confirms no pending non-`Deactivate` descriptor — a `[Deactivate, M]` FIFO race **aborts
  eviction** (no message loss), M is dispatched.
- **Re-arm-with-cancel:** each activation holds **one** armed-deactivation token, bumped on
  the busy edge, so live entries stay **O(1) under flap** (a stale token is lazily skipped
  on fire).
- A **live** `IdleTimeout` change reconciles the existing idle population by a shard-local
  sweep (see 005) rather than waiting for the next arm.

The `idle_ticks` encoding ceiling is tied to the **build-only wheel granularity**;
far-future arming rides the heap overflow tier.

## Priority interaction (ADR-010)

The deadline wheel stays **orthogonal to priority banding** (002): deadlines fire via the
wheel and `tell` onto the actor's mailbox (single-executor preserved), and priority orders
**activations across actors**, never messages within a mailbox — so priority never reorders
a deadline-carrying message within its actor's mailbox.

A **deadline-unified `EdfBanded` policy** (folding these deadlines into a scheduling band
key) was **evaluated and deferred** by
[ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md): under overload,
deadline-banding can degrade **below** plain FIFO (the EDF-domino effect — proven). The
scheduler's `band_of()` is the extension point if a future opt-in EDF policy is pursued,
and it **must preserve per-actor FIFO**.

## Dependencies

Std-only: `std::chrono::steady_clock` for time, plain arrays for the wheel. No
external timer library.

## Open questions

- Wheel granularity/tier sizing vs. deadline precision requirements (µs-level
  deadlines vs. ms buckets).
- (Cross-node deadline propagation is resolved in `018-Clocks-and-Deadlines.md`.)
- Drift between the timekeeper's tick and per-shard active advance.
