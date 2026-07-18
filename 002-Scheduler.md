# 002 — Scheduler

## Model

The scheduler schedules **activations**. Workers are transient lanes that borrow
activations from shards; they never own actors.

## Worker

A worker is an execution lane (one per hardware thread, typically pinned). Thread
affinity and NUMA topology are obtained through the Platform Abstraction Layer
(019, Linux/Windows/macOS backends); where a platform exposes no affinity or NUMA API,
the scheduler **degrades gracefully** — locality preference (below) collapses to a
single domain rather than failing. Its loop:

1. **Select** the next activation (own shard first — see below).
2. **Acquire** execution ownership (single atomic CAS on the actor's exec state,
   `Idle → Running`, **acquire** ordering).
3. **Drain** the mailbox until it reports `Empty`, reports `Busy`, or the drain
   budget is exhausted (see *Drain results*).
4. **Release** ownership via the close-out protocol below.
5. **Requeue** the activation if messages remain (`Running → Scheduled`).

A worker that fails to acquire ownership (another worker already holds it) skips
the activation — it does not block.

### Drain results

A drain step returns one of three results — the third exists because the mailbox
is an intrusive Vyukov MPSC (003), whose emptiness is **not linearizable**:

| Result | Meaning | Worker action |
|---|---|---|
| **Empty** | `head_` reached the tail; no producer mid-publish | run the release close-out |
| **Busy** | a producer has published (`tail_.exchange`) but not yet linked its node — a transient window, *not* emptiness | **bounded spin**, then if still `Busy`, `Running → Scheduled` and re-enqueue (never spin unbounded) |
| *(more pending)* | drain budget exhausted with work left | `Running → Scheduled` (see *Fairness*) |

`Busy` must never be misread as `Empty`: doing so would strand a published message
until the next unrelated wakeup.

### Release close-out (load-bearing)

Releasing ownership on an apparent `Empty` is a **read-only** protocol — the
consumer must not mutate the queue while it is racing a producer:

The rendezvous is spelled as a **symmetric store + `seq_cst` fence + load on both
sides** — the canonical, ISA-independent Dekker form (ADR-004):

```
consumer:  exec_state.store(Idle, release);  atomic_thread_fence(seq_cst);  tail_.load(acquire);
producer:  tail_.exchange(desc, acq_rel);    atomic_thread_fence(seq_cst);  exec_state.load(acquire);
```

1. `exec_state.store(Idle, release)` then `fence(seq_cst)` — the release publishes
   the consumer-private `head_` for the next worker (001); the fence is the
   consumer's half of the wakeup rendezvous.
2. **Read-only** `tail_.load(acquire)` probe (no dequeue, no `head_` write).
3. If the probe shows work (or `Busy`), **re-acquire** via
   `CAS(Idle → Running, acquire)` **before touching `head_` again**, and resume
   draining. If the CAS loses, another worker already owns the actor — done.

> **Two obligations, two orders — do not collapse them (ADR-003/004).** The
> release-store does double duty. As the `head_` **publish** it needs only *release*
> (the single-writer handoff, S1-proven). But the store-then-load-of-`tail_`, racing
> a producer's `exchange(tail_)`-then-load-of-`exec_state`, is a **Dekker StoreLoad
> rendezvous** — the one reordering x86-TSO permits (a store buffered past a later
> load to a different address). Plain release/acquire does **not** fence StoreLoad, so
> a wakeup can leak; the **`seq_cst` fence between the store and the load on each
> side** closes it (executed control: **0 / 300,000** lost with the fence). The
> producer keeps its cheap `acq_rel` exchange — no per-enqueue `seq_cst` cost.
> **Magnitude note (ADR-004 correction):** the dramatic ~48% loss only occurs if
> *both* sides are downgraded; with the producer's `acq_rel` exchange retained the
> residual x86 leak is ~0.05–0.09%. So calibrate the CI guard to **`lost == 0` vs
> `lost > 0`**, not to a 48% threshold — the fence is load-bearing regardless of the
> small magnitude. Keep it a permanent regression guard, alongside the relaxed-CAS
> `head_` guard (001).

> **Never reintroduce the mutating variant.** An earlier close-out that ran a
> *mutating* `try_dequeue` inside the `Empty → release` recheck window silently
> **lost and leaked a message**. ADR-002 proved it: the broken close-out dropped
> **200k/200k** nodes; the read-only-probe + reacquire-before-mutate form dropped
> **0/200k**. Keep this as a permanent regression test. See
> [ADR-002](decisions/ADR-002-mailbox-mpsc-hot-path-r2.md).

## Sharding

```
ActorId → hash → Shard
```

A **shard** owns:

- its activation queues,
- a local allocator (`std::pmr` memory resource — see `003`),
- its metrics counters.

Placement is stable: the same `ActorId` always maps to the same shard, so an
actor's state, mailbox, and allocations share a locality domain.

## Locality preference

A worker selects activations in this order:

1. its **own shard**,
2. a shard on the **same NUMA node / socket**,
3. a **remote shard**.

Shards are assigned to workers to maximize (1). (2) and (3) are the stealing path.

## Work stealing

Stealing is a **fallback only** — it runs when a worker's preferred shards are
empty. It follows the same priority order:

1. own shard, 2. same NUMA/socket, 3. remote node.

Stealing moves the *right to execute* an activation, not the actor's ownership or
allocations; the actor's state stays home, so stealing costs a queue pop, not a
migration.

## Wakeup

**Targeted, never broadcast.** Each worker has its own wait signal
(`std::atomic` wait/notify). On an **empty → non-empty** transition of a shard
queue, exactly **one** sleeping worker is resumed. This avoids the thundering
herd of a broadcast wake.

**Wakeup and deschedule ride the exec-state machine, never mailbox emptiness.**
This is normative. The intrusive Vyukov mailbox (003) has **non-linearizable
emptiness** — there is a transient window where a message is published but the
queue still reads empty — so a wakeup keyed on "mailbox became non-empty" would
lose wakeups. Instead, wakeup is driven by the actor's exec-state transitions:
enqueue drives `Idle → Scheduled` (waking one worker), and the worker relinquishes
via `Running → Idle` only through the read-only close-out above. Any code path that
wires wakeup or deschedule to mailbox emptiness reintroduces lost wakeups. The
protocol is only race-free when the close-out rendezvous carries the **`seq_cst`
fence** (see the close-out note): ADR-002's original 20M-race test passed because it
did not isolate the Dekker StoreLoad window; ADR-003 built the isolating control and
ADR-004 refined the magnitude — with the producer's `acq_rel` exchange retained the
x86 leak is small (~0.05–0.09%) but nonzero, so the fence is load-bearing and the CI
guard is `lost == 0` vs `lost > 0`. See
[ADR-004](decisions/ADR-004-mailbox-mpsc-hot-path-r4.md).

## Streaming activations (024)

An inbound stream (024) does **not** get a second scheduler. Its per-stream ring is
drained through **one reusable `StreamActivationDescriptor`** on the actor's ordinary
mailbox, posted only on the ring's empty→nonempty edge. That activation rides the
exec-state wakeup (`Idle → Scheduled`) and the **`seq_cst` Dekker close-out
verbatim** — **never keyed on ring emptiness** (ring emptiness, like mailbox
emptiness, is non-linearizable). A budget-exhausted stream drain yields
`Running → Scheduled` for fairness exactly like a mailbox drain. When a *suspended*
stream handler completes, the 015 admission gate re-enters `StreamChannel::drain` via
a stream-descriptor-aware continuation bound to `(StreamChannel*, disp)` — the
activation is **transferred, not parked**, so the descriptor is not re-enqueued and
the buffered window cannot be orphaned.

## Blocking/fiber adapter completion — the `Parked` exec-state (ADR-015)

The exec-state machine gains an explicit fourth state for off-hot-path
`BlockingHandler`/`FiberHandler` calls (001):

```
Idle → Scheduled → Running → (Idle | Scheduled | Parked)
```

`Parked` is a **sealed** state that **fails every admission CAS** — while an actor is
parked on an in-flight blocking/fiber call, no worker may claim it (single-executor is
preserved by the seal, not by luck; a `-DQUARK_PARK_IDLE=1` control that parks as `Idle`
instead was proven to double-execute and heap-UAF). Unlike an async `co_await` (which the
completing thread re-admits on itself), a blocking/fiber leaf completes on a **carrier**
(an offload thread, or a borrowed fiber's origin worker). Its completion is therefore a
**structurally new third-party waker**: carrier → actor, re-admitting via a
`Parked → Scheduled` CAS. That CAS carries the **same `seq_cst` Dekker rendezvous** as the
close-out, but it is a **distinct StoreLoad pair** (carrier-vs-consumer, not
producer-vs-consumer) and so needs its **own** isolating CI control — a 3-party litmus in
which dropping the carrier-side fence must leak a wakeup. Resume of the stackful form is
**origin-worker-pinned** (015): a foreign frame may hold thread-affine state (a pthread
mutex owner, `errno`/TLS), so cross-worker resume is UB.

> **Platform note (ADR-015).** The producer's half of the close-out fence is **elided to
> zero instructions on x86** (TSO makes the `acq_rel` exchange a full StoreLoad barrier),
> but this is x86-only — a real `dmb ish` is retained on ARM64 behind the PAL
> `store_load_barrier()` (019). The `Parked`/resume release-acquire handshake, like the
> whole close-out, is TSO-proven only and carries a deferred ARM64 weak-memory re-gate.

## Fairness

The **drain budget** bounds how long one actor may hold a worker. When the budget
is exhausted with messages still pending:

```
Running → Scheduled
```

the activation is re-enqueued behind others, so no single hot actor monopolizes a
lane. The budget is configurable per actor via the `DrainBudget<N>` policy (see
`005`) and has an engine-wide default.

**Skipping a cancelled tombstone (003) counts against the drain budget.** A
tombstone skip is cheap but not free, and a mass-cancellation leaves an arbitrarily
long run of tombstones; if skips were budget-free, draining them would monopolize
the lane for `O(N)` before yielding. Charging each skip against the budget bounds
that: ADR-004 proved a 10M all-cancelled mailbox yields the lane (`BudgetExhausted`
after 1024 skips, ~21 µs) once skips are budgeted, versus an unbounded stall when
they are not.

## Priority scheduling — K-band per-shard run-queue (ADR-010)

Resolved by [ADR-010](decisions/ADR-010-priority-and-fairness-scheduling-policy.md)
(D1, proven 7/7): `Priority<P>` (005) is an **engine-level compile-time scheduling
policy**, not a change to the mailbox.

- The shard's single activation run-queue generalizes to `std::array<ActivationMpsc, K>`
  — **one FIFO band per priority level**, under the `PriorityBands<K, Anti>` policy. The
  band index is resolved **once at startup** from `Priority<P>` and is **constant per actor
  type** (no per-message recompute).
- **`UniformFIFO` (K=1) is the default** and is a **distinct type** that objdumps
  **byte-identical** to today's single per-shard MPSC (proven, both compilers, -O2/-O3) —
  the uniform case pays nothing. *Disable-priority must resolve to `UniformFIFO`, never
  `PriorityBands<1>`* (which keeps the anti-starvation turn-counter store); this is a 005
  Validation rule.
- **Enqueue** picks the band-queue by a **compile-time array subscript** on the **same**
  single `tail_.exchange` — **0 added cross-core RMW**. The draining worker does an O(K)
  **relaxed** non-empty probe + `countr_zero` (TZCNT) to select the top non-empty band.
  **K is capped at 8** — beyond that the K-way probe hits cross-core coherence-miss
  inflation that can breach the 100 ns local-tell budget under adversarial producer
  pressure.
- **Per-actor mailbox FIFO is inviolable**: priority orders **activations across actors**,
  never messages within one actor's mailbox (proven: 0 inversions to a middle-band actor
  under cross-band saturation).
- High-band dispatch p99 **9.3 µs vs 2.94 ms** (~316×) under 10%/90% saturating mixed load
  vs `UniformFIFO`.

The banded run-queue is **single-consumer**: a stealer must win a per-shard **drain-owner
CAS** (`std::atomic<Worker*>`, null→self per drain session — a **cold** edge, not
per-enqueue/select) before popping. Banded `select` **bounded-spins on Busy** exactly like
the mailbox drain (a single-pass non-spinning probe can strand a `Scheduled` activation
after its lone targeted wakeup). The **exec-state wakeup and `seq_cst` Dekker close-out are
unchanged** — they live on the per-actor mailbox, orthogonal to the per-shard run-queue;
the consumer non-empty probe compares `tail_` to the constant `&stub_`, never the moving
`head_`.

### Anti-starvation is a policy knob (guaranteed, not best-effort)

| `Anti` policy | Mechanism | Bound |
|---|---|---|
| **`RotatingReserve<M>`** (default) | Every M-th select turn services the next non-empty band under a round-robin cursor (consumer-local, non-atomic) | `(d+1)·K·M` select turns — proven tight for **every** band incl. middles; tunable via M independent of priority separation |
| `WeightedDRR<w…>` | Deficit-round-robin: per-band share `w_i/Σw` | proportional; but couples the low-class wait to the weight ratio, and (for suspend-heavy handlers) stretches to O(Σ quantum) scheduling cycles — `RotatingReserve` is the safer default |

`RotatingReserve<M>` injects a periodic high-band p999 spike (~one low-band drain budget,
freq ~1/M; measured 27 µs) — the honest fairness tax, tunable via M.

## Open questions

Resolved: budget accounting for `Reentrant` actors — the drain budget is the
`Paused` seal of the shared admission gate and counts **admitted** handlers per
scheduling turn, not suspended ones (a suspended handler frees the lane and
consumes no further budget). See `015-Reentrancy-and-Quiescence.md`.
