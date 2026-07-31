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

## Mailbox hot-path baseline (ADR-029 r7, ADR-031 r8, ADR-032 r9, ADR-033 r10 judgment)

The intrusive Vyukov mailbox (003) won its 7th consecutive design round in
ADR-029, then successfully defended against two fresh challengers (a
segmented-ring/epoch-gated design and a 4th-generation Treiber-push/batch-
reversal design) in ADR-031 r8, then again in ADR-032 r9 against a resident
sequence-numbered ring with overflow valve (SBR-v5) and a segmented Treiber-
push/bounded-batch-reversal design (SEG-REX), then again in ADR-033 r10
against a hazard-pointer/RCU-paired SEG-REX descendant (SEG-HP) and an
explicit opt-in per-caller policy tag (`DeliveryMode<OrderFirst|
ThroughputFirst<K>|LatencyFirst>`) — none dislodged it. Measured
numbers from all rounds are real data points on different hosts/load
conditions, so all are recorded rather than treated as one canonical figure:

- **F2 — steady-state zero-RMW dequeue**: 0 cross-core RMW on the steady
  multi-node dequeue path; the one unavoidable RMW is confined to the
  boundary/stub re-arm. Reconfirmed in ADR-031.
- **F3 — throughput/latency**: ADR-029 measured p50 374–434ns, single-producer
  throughput 6.8–7.9M msg/s, aggregate P=4 throughput 15.4–17.3M msg/s — this
  fell short of a claimed floor (≤100ns / ≥10M msg/s), recorded as a weighed
  loss on that specific number (the reference host was under heavy, unrelated
  multi-tenant contention, load average ~17 on a 32-core box, a real but
  non-dismissing confound). ADR-031, on different hardware, measured
  occupancy-1 latency p50=60.0ns / p999=129–178ns — comfortably inside the
  250ns/50µs hard budgets on that host. Both are real, host-dependent
  measurements; the budget-compliance verdict (pass) is what carries across
  hosts, not either single number.
- **P=4 scaling gap — mechanism isolated (ADR-031), reconfirmed and both
  fresh fixes rejected (ADR-032 r9), still open.** The gap between
  single-producer and P≥4 aggregate throughput is caused by shared `tail_`
  cache-line contention: a same-shape independent-cache-line control measured
  contended P3/P1 = 0.73–0.88× versus uncontended P3/P1 = 2.5–2.6×
  (near-linear), isolating the mechanism rather than leaving it a
  profiled-but-unexplained hypothesis. ADR-032 r9 measured the same effect
  directly on the incumbent (P=1→4 aggregate enqueue throughput degrading
  ~24–29%: 28.97→21.93 Mops/s gcc, 24.81→17.62 Mops/s clang) and tried two
  purpose-built fixes: SBR-v5's ring valve engaged its overflow path at a
  small fraction of peak load and then ran *slower* than the incumbent alone
  (8.1M vs 13.5M ops/s at P=4), and also broke its own stated global-ticket-
  order guarantee (up to 858,983 out-of-order deliveries/run); SEG-REX's
  segmented push lost to the incumbent by 3–4× at every producer count and
  had a sanitizer-invisible reclamation deadlock. Both were disqualified, not
  merely slower-by-preference. Closing the gap — not just diagnosing it —
  still needs a materially different producer-side design; ADR-032 flags
  pairing SEG-REX's bounded-segment reversal (which *did* achieve flat p999
  independent of backlog depth) with real hazard-pointer/RCU reclamation as
  the most promising untried direction. See ADR-031, ADR-032, and
  [TAIL-CONTENTION.md](TAIL-CONTENTION.md) for the mechanism walkthrough.
- **Busy/Empty/mid-publish tri-state contract validated as the reference
  correctness property for future designs (ADR-032).** SBR-v5's C2 claim
  (paused-producer correctly reports Busy, never folded into Empty) passed
  cleanly under 300+ trials specifically because it reused this contract
  unmodified — any future segmented/ring redesign of the mailbox must
  reproduce this same tri-state handling, not just match throughput.
- **P-scaling gap: two fresh candidates tried and rejected, but the
  achievable ceiling for the whole design family is now empirically bounded
  (ADR-033 r10).** SEG-HP (a hazard-pointer/RCU-paired descendant of
  ADR-032's SEG-REX) was disqualified by a *new* tri-state-contract
  violation — `try_dequeue()` returns `Empty` at an ordinary,
  non-force-sealed segment-rotation boundary even though the producer has
  already committed guaranteed-forthcoming work (195/200 isolated repro;
  13,353/240,000 in one multi-lane stress run) — plus a ~370-400x p999
  regression under synchronized rotation-burst contention (91.9-112.8µs vs.
  the incumbent's 250-286ns). `DeliveryMode<OrderFirst|ThroughputFirst<K>|
  LatencyFirst>`, an explicit opt-in per-caller policy tag rather than a
  mailbox redesign, passed every safety/correctness gate cleanly but its
  central throughput claim (>=3x at P=K=8) fell to 1.2x-1.5x real /
  2.1x-2.7x idealized-ceiling — root-caused, via an idealized
  collision-free control, to the single-executor invariant itself: exactly
  one consumer thread still merges all K shards, and that consumer's
  finite capacity to service concurrent cross-core coherence traffic from
  K live producers is the new bottleneck. **Any future multi-shard-plus-
  single-consumer mailbox proposal attacking this gap should be evaluated
  against a ~2x-2.7x realistic ceiling, proven via an idealized
  collision-free control, not an assumed near-linear scaling with shard
  count K.** See ADR-033 and [TAIL-CONTENTION.md](TAIL-CONTENTION.md).

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

### Idle backoff before park (ADR-035)

A worker parking (blocking on its `std::atomic` wait signal) the instant its scan finds nothing
means every producer that lands a message on an already-parked worker pays a real OS wake syscall
(futex / `WaitOnAddress`) synchronously inside its own `tell()` call — measured on one host at
31.0%/15.8%/4.4% of sends at 1/2/4 producers for a fast-draining single-shard workload. Before
committing to `park()`, a worker runs a **bounded, read-only pre-park spin**
(`EngineConfig::pre_park_spin_limit`, default 256): re-probe `any_work()` (a plain acquire load,
the same probe `park()`'s own rescan already uses) between bounded `cpu_relax()` pauses; a hit
re-enters the ordinary scan path, a full miss falls through to `park()` completely unmodified. The
spin **never writes the idle bitmask** — a producer's targeted wake only fires a syscall when a
worker's idle bit is set, so a message landing during the spin is picked up by the spin's own probe
instead, and `park()`'s announce/fence/rescan Dekker rendezvous (the sole source of the
no-lost-wakeup guarantee above) is reached in exactly the state it would have been in without the
spin. `pre_park_spin_limit = 0` disables it (a predictable branch is the only added cost). Measured
(one host, `decisions/ADR-035-worker-park-wake-backoff-policy.md`): the wake-syscall ratio dropped
from ~26-31%/~15%/~4-6% to ~0.02-0.26% at 1/2/4 producers; throughput improved modestly (single
digit to low double-digit percent); a genuinely idle worker still converges to near-0% CPU. An
adaptive (EWMA-gated exponential) alternative was proven to win more on throughput but was
rejected as the default: its measured worst-case single spin round cost ~380µs on the proving
host — worse than the wake-syscall cost it was fixing, and in tension with this section's own
bounded-spin discipline (`busy_spin_limit` above). A hybrid (adaptive gating + the tighter fixed
spin shape) is recorded as an unproven round-2 target.

### Activation-scoped post-drain linger (ADR-036)

ADR-035 cut the OS-wake-syscall rate, but a separate cost remained: even without a syscall firing,
an activation's mailbox draining to empty triggers a full `Running->Idle` exec-state transition
(CAS + run-queue enqueue + idle-bitmask scan) — measured at 18-25% of messages in a tight
single-producer loop against a fast-draining actor. Before `drain_step` commits to that transition
on an empty mailbox, it runs a **bounded, read-only post-drain linger**
(`EngineConfig::activation_linger_spin_limit`, default 32, sequential drain path only): re-poll
THIS activation's own mailbox a bounded number of `cpu_relax()`-paced times; a hit is dispatched
directly (the linger's own successful dequeue result flows straight into normal processing, never
a second dequeue — the naive "re-enter the loop" implementation would drop or duplicate the
message it already popped, closed by construction, not by convention). `exec_` is **never written**
during the linger — it stays `Running` the whole time, so a racing producer's CAS (which expects
`Idle`) fails cheaply and silently: no run-queue enqueue, no idle-bitmask touch, no `activations`
metric increment for that message. On a full miss, `close_out()` runs completely unmodified.

**Measured (`decisions/ADR-036-activation-linger-idle-churn-reduction.md`), regime-qualified, not
uniform:** under sustained backlog/contention (a slow-relative-to-producers consumer, or multiple
concurrent producers), the default (32) cuts activation churn sharply (one measured case:
23.98%→1.56%) with a real throughput win (+26.4%, one measured case) and 27-40% faster `post()`
latency. Under sparse/idle-ish traffic (gaps between sends exceeding the spin window), the default
does **not** reliably meet its own reduction target (one measured case: 93.10% vs 92.31%
baseline — essentially no effect) — there is no single fixed bound that helps both traffic shapes
simultaneously. A larger bound (256) closes the sparse-traffic gap but **regresses badly under
contention** (down to -50.6%, one measured case) because the linger's re-poll contends with the
same mailbox-tail cache line producers need — this is worse overlap than ADR-035's `pre_park_spin`
sees, since the linger fires precisely when a message is plausibly imminent. 32 is the proven,
shipped default; **256 is an explicitly rejected default**, not merely untested. Sequential drain
only — `drain_step_governed_seq` (022 admission bookkeeping runs per-message inside its own loop,
not before it) and `drain_step_reentrant` (015's own Parked-based mechanism) are both unaffected.
A cache-line-isolation alternative targeting the fixed per-transition cost instead of transition
frequency was proven and rejected in the same round: the underlying false-sharing mechanism is
real in isolation (4.45× in a synthetic control) but is swamped by park/wake round-trip cost in the
real engine (-1.6%, i.e. the wrong direction, against a pre-declared 3%-improvement bar) — not
worth shipping as a performance change.

## Broadcast schedules activations, not messages (ADR-019)

Best-effort broadcast (`Topic<M>`, [ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md))
adds **no** new scheduler contract. A `publish(M)` lowers to **N ordinary ADR-002
tells sharing one immutable refcounted payload**: each per-subscriber delivery is an
ordinary mailbox enqueue that drives that subscriber's exec-state `Idle → Scheduled`
wake through the **verbatim** targeted-wakeup + `seq_cst` Dekker close-out above —
the fan-out schedules **activations, not messages**, and never a broadcast-specific
scheduler entity. **At-most-one executor per subscriber is unchanged**: the shared
payload is fanned as N thin descriptors onto N independent mailboxes, so each
subscriber activates under its own exec-state CAS exactly as for a discrete tell.

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

## Reply-stream terminal edge (ADR-018)

An outbound streaming reply runs the 024 ring **backward** (callee = producer,
caller = consumer). Its terminal edge — **cancel, deadline (018), or close** —
reuses the **`seq_cst` Dekker close-out verbatim**: the terminal CAS does a
**two-part wake** in one shot — it **arms the caller's drain** (`notify_enqueued`,
`Idle → Scheduled` on the consumer) **and** bumps `credit_gen + notify` so a callee
stalled on a full credit window wakes to observe teardown. As on the inbound path
(§Streaming activations), wakeup is **never keyed on ring/credit emptiness** — that
emptiness is non-linearizable; both halves ride the exec-state / credit-generation
machine. This is the 024 inbound arming rule run in reverse. See
[ADR-018](decisions/ADR-018-outbound-streaming-replies.md).

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

### Shard-fairness / starvation gap (tracked, cross-referenced from ADR-023)

A continuously-loaded shard can starve sibling shards' **drain-owner
acquisitions** — and hence their timer advance (011) — unboundedly when
`shard_count > worker_count`. This was reproduced during ADR-023's debate: the
011 timekeeper's targeted wake cannot rescue a worker that is busy rather than
parked, so a saturated worker starves every shard behind it in its assignment,
independent of which timer design 011 adopts. This is a **required fix tracked
here** (002/ADR-010), not a 011 concern — the fix belongs to drain-owner
acquisition fairness across shards assigned to one worker, not to the timer
wheel.

## Open questions

Resolved: budget accounting for `Reentrant` actors — the drain budget is the
`Paused` seal of the shared admission gate and counts **admitted** handlers per
scheduling turn, not suspended ones (a suspended handler frees the lane and
consumes no further budget). See `015-Reentrancy-and-Quiescence.md`.
