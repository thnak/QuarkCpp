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

**Tier scheme (ADR-023, proven).** 5 fixed tiers × 64 slots (6-bit indexing) on a
64µs base tick, giving a ~19.09h native span before the heap-overflow tier takes
over. This is a **`BuildOnly` constant set** (consistent with 013's Reconfig-class
table), not a runtime knob.

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

### Active-advance algorithm (ADR-023, proven)

`advance_to()` scans an `occupied_mask` bit-window (`std::countr_zero` over the
masked range) at each tier-0 wrap segment, firing **every occupied slot in that
window** before jumping — it does *not* sample a single slot per iteration and
jump past the rest. (Sampling-and-jump was a fatal bug found and fixed during
ADR-023's proof: it silently skipped due timers under a large catch-up gap.) A
guard is required when the jump step equals the full slot count, to avoid a
shift-by-bit-width UB. Measured cost is near-flat across catch-up gap size — 1.24×
cost for a 100× larger gap — which is the empirical evidence that a finer base
tick does not reintroduce a "catch-up cost grows with gap" tension.

### Drift bound (ADR-023, proven)

The wheel publishes exactly **one** `next_due_hint_` relaxed atomic per shard — no
`seq_cst` fence needed, since this is a one-directional publish, not a Dekker
pair. The node-level timekeeper reads only that hint across all shards and
**reprograms its own sleep to the global next-due minimum**
(`sleep_for(min(next_due - now, idle_check_interval))`) rather than polling on a
fixed cadence alone. Proven bound: idle-shard lateness ≈ `idle_check_interval` +
OS-wake jitter (measured headroom 250–700µs at a 1ms floor on reference
hardware), independent of base tick size.

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

**Fire-time CAS ordering (ADR-023, proven, normative).** The wheel's fire-time CAS
on a message descriptor's `gen_state` must check the **generation first** and
treat a mismatch as an unconditional no-op, before branching on state. (The fatal
bug found in ADR-023's proof was a CAS-failure branch that inspected only state
bits, letting a stale deadline spuriously `request_stop()` an unrelated, later
message recycled into the same descriptor slot.) The CAS must also preserve any
legitimate nonzero `flags` bits already on the `Queued` word rather than
hardcoding them to zero.

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
- **On-demand passivation (ADR-028 Phase 8).** `ActorRef<A>::passivate()` posts the SAME
  `Deactivate` control descriptor through the SAME shared interlock (a CAS guarding the
  activation's one dedicated control descriptor against a double-post from the two independent
  triggers) — equally soft/advisory as the wheel: the actor drains every message already
  queued/in-flight and retires at the next genuinely-idle instant, never a hard mid-handler
  interrupt. Sequential-only, same restriction as `IdleTimeout<Ms>` above. The
  `on_deactivate()` lifecycle hook and the deactivation-time persistence flush (012) fire from
  the same `close_out_retire()` call site regardless of which trigger fired — automatic and
  on-demand retirement are byte-identical from the actor's point of view.

The `idle_ticks` encoding ceiling is tied to the **build-only wheel granularity**;
far-future arming rides the heap overflow tier.

The re-arm-with-cancel token mechanism rides the same tier scheme (ADR-023) and
was re-verified O(1) under flap with the ADR-023 tier/tick change — cancel cost is
at parity across tier0, tier4, and the overflow tier.

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

**TimerEntry pool-exhaustion policy (ADR-023).** Non-blocking, non-allocating: a
saturating `deadline_tracking_dropped` counter (009-style), never a heap
fallback.

## Open questions

- *(Wheel granularity/tier sizing vs. deadline precision: resolved — 5 fixed
  tiers × 64 slots on a 64µs base tick, a `BuildOnly` constant set. See "Data
  structure" above, ADR-023.)*
- (Cross-node deadline propagation is resolved in `018-Clocks-and-Deadlines.md`.)
- *(Drift between the timekeeper's tick and per-shard active advance: resolved —
  a single relaxed `next_due_hint_` per shard drives the timekeeper's targeted
  sleep; proven bound ≈ `idle_check_interval` + OS-wake jitter. See "Drift bound"
  above, ADR-023.)*
- **Shard-fairness / starvation (tracked against 002/ADR-010, not here).** A
  continuously-loaded shard can starve sibling shards' drain-owner acquisitions —
  and hence their timer advance — unboundedly when `shard_count > worker_count`.
  The timekeeper's targeted wake cannot rescue a worker that is busy rather than
  parked; this is the actual remaining open liveness gap, reproduced during
  ADR-023's debate, and belongs to 002/ADR-010, not to the wheel design itself.
