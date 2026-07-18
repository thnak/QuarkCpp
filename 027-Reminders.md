# 027 — Reminders (Durable Scheduled Wake-Ups)

011 gives every actor a **timer**: `schedule_after(500ms, Tick{})` rides a per-shard, in-memory,
monotonic-clock timing wheel — precise, zero-alloc, and **gone the instant the actor deactivates or
the process restarts**. That is the right tool for a retry backoff or a heartbeat. It is the *wrong*
tool for "charge this subscription at 9 PM," "expire this session tomorrow," or "run end-of-day
settlement" — work that must survive a restart, must fire even though the actor was passivated hours
ago, and is scheduled against **wall-clock/civil time**, not a monotonic stopwatch.

That second tool is a **reminder**, and Quark does not have one yet. This spec adds it — as a
*distinct* subsystem from 011, not an extension of the wheel — and pins the property that makes it
usable at scale: a reminder is **durable, coarse, at-least-once, and jittered**, deliberately trading
the timer's precision for the ability to schedule millions of them without a thundering herd.

> **The locked decision.** A **reminder** is a durable record `{actor, name, due (wall-clock), period,
> payload}` persisted through the **012 `Store` seam**, owned by exactly one node via **010/026 HRW
> placement**, scanned from a **coarse bucketed due-index**, fired with **deterministic jitter** across
> a window, delivered **at-least-once** as a normal `tell` on the actor's own lane (reactivating a
> passivated actor), and governed by **022** so a mass-due wave sheds/paces instead of melting the
> node. Precision is the thing traded: reminders are bucket-granular (± jitter), timers stay ms-precise.
>
> **The mechanism is decided (SEGSTREAM) and the scale claim is proven** — see
> [ADR-017](decisions/ADR-017-durable-reminder-mass-due-scale-gate.md), which won a
> design→debate→prove gate (3 competing designs, red-team cross-exam, executed C++23) over the
> in-RAM-wheel-mirror and per-row-jitter alternatives. The load-bearing mechanism: the due segment
> binds to **one bounded 024 `StreamChannel` drained under a 022 fire-rate token bucket** — `spread`
> is the **drain rate**, so the peak dispatch is `fire_rate` **exactly** and peak in-flight is the
> **credit window**, not N. Reference implementation: [`reminder_service.hpp`](include/quark/core/reminder_service.hpp)
> (`ReminderService` + the `ReminderStore` seam, `InMemoryReminderStore` + crash-durable
> `FileReminderStore`, plus opt-in `Sqlite`/`Rocks` reminder adapters —
> [PersistenceAdapters.md](PersistenceAdapters.md)); proof re-measured from a clean build on the dev box in
> [`reminder_bench.cpp`](bench/reminder_bench.cpp) and gated by [`reminder_segstream_test.cpp`](tests/reminder_segstream_test.cpp);
> runnable demo in [`samples/14_durable_reminders`](samples/14_durable_reminders/main.cpp).

## Timer vs. reminder — one axis, two ends

| | **Timer** (011, shipped) | **Reminder** (this spec) |
|---|---|---|
| Storage | in-RAM per-shard timing wheel | **durable — the 012 `Store`** (FileStore/SQLite/RocksDB) |
| Clock | monotonic `BootClock` (018/019) | **wall-clock / civil time** (`pal::wall_now`, CLOCK_REALTIME) |
| Survives restart | no | **yes** (re-read from the store) |
| Survives passivation | no | **yes** (firing triggers a cold activation, ADR-008) |
| Fires for a *deactivated* actor | no | **yes** |
| Accuracy | precise (ms buckets) | **coarse (bucketed) ± jitter** |
| Delivery | best-effort, in-memory | **at-least-once** (catch-up after downtime) |
| Handler expectation | none | **idempotent** (017) — may fire late or twice |
| Cost | ~ns, zero-alloc | a durable write per (de)register; a bucket scan per tick |
| Right for | backoff, heartbeat, deadline | billing, session expiry, end-of-day, "at 9 PM" |

The two are siblings, not rivals: once a reminder's bucket is loaded, its *local* dispatch to the
actor rides the same 011 mechanism (a `tell` on the lane). 011 is the local delivery vehicle; 027 is
the durable, civil-time, node-spanning scheduler that feeds it.

## The model

A reminder is registered from a handler and named, so it is idempotent to re-register and cheap to
cancel:

```cpp
// From inside a handler — durable, survives restart & passivation:
co_await self().remind_at(civil_time("21:00"), Every{24h}, Charge{amount});   // periodic, wall-clock
co_await self().remind_after(72h, Expire{});                                  // one-shot, relative
self().cancel_reminder("charge");                                            // by name

// Firing delivers Charge{...} to this actor as a normal message (006), on its own lane:
void handle(const Charge& c) { /* idempotent: dedup on c.period_id */ }
```

- **Named.** `(actor, name)` is the key; re-registering a name overwrites (upsert), so a handler that
  re-arms on every run does not accumulate duplicates. `cancel_reminder(name)` deletes the row.
- **The payload is a described message** (008/016) — the same canonical-tagged bytes persistence and
  the wire use, so a reminder registered by v1 code fires correctly into v2 code (schema evolution).
- **Firing is a `tell`**, never a lambda: single-executor, placement, and FIFO all hold (011 §API).
  If the target is passivated, the fire path reactivates it first (ADR-008) — measured cold activation
  is ~100 ns, but *a million at once is a wave*, which §Scale governs.
- **`period` = 0** ⇒ one-shot (the row is deleted after a confirmed fire). `period > 0` ⇒ the next due
  is computed from the *scheduled* instant (not the actual fire time), so jitter and lateness do not
  accumulate drift across periods.

## Storage — the 012 `Store` seam, indexed by due-time

Reminders are persisted through the **same adapter families** as actor state (012 / PersistenceAdapters.md),
via a parallel `ReminderStore` seam the same backends implement (the row is the durable **truth**;
[`reminder_service.hpp`](include/quark/core/reminder_service.hpp)):

```cpp
template <class S>
concept ReminderStore = requires(S& s, const ReminderRow& row, ReminderKey key, std::int64_t bucket) {
    { s.put(row) }             -> std::same_as<result<void>>;                 // UPSERT (durable)
    { s.remove(key) }          -> std::same_as<result<void>>;                 // cancel / one-shot completion
    { s.load_all() }           -> std::same_as<result<std::vector<ReminderRow>>>; // replay live rows on cold open
    { s.checkpoint(bucket) }   -> std::same_as<result<void>>;                 // durable "resolved through here"
    { s.checkpoint_bucket() }  -> std::same_as<std::int64_t>;                 // last checkpoint (rebuild skip)
};
```

SEGSTREAM keeps the **ordered due-index in RAM** — a `map<due_bucket → keys>`, so a tick's begin-peek
is `O(log S)` and pulls only `bucket[now]`, never the whole population (**O(due-now)**, proven flat at
~24 ns/idle-tick from 10³ to 10⁶ dormant; `reminder_bench.cpp` §B). The index is rebuilt from
`load_all()` on cold open (`O(total-owned)`; `compact()` bounds this to `O(live)`). **Four backends
ship — full parity with the event `Store` families** (PersistenceAdapters.md): `InMemoryReminderStore`
(reference) and the crash-durable, std-only `FileReminderStore` (an append-only, CRC32-framed WAL +
`fdatasync` with torn-tail truncation on replay — the reminder analogue of `file_store.hpp`, **exactly
one `fdatasync` per register/cancel**), plus the opt-in `SqliteReminderStore` (a `reminders` table with
a native `rem_due` SQL index, so `load_all()` returns rows pre-sorted by due time) and
`RocksReminderStore` (big-endian due-prefixed keys ⇒ a native on-disk due-ordered scan), each behind
its `QUARK_WITH_*` flag. All durable backends pass one shared crash-durability harness
(`reminder_store_conformance.hpp`); `FileReminderStore` runs it in the default `ctest`. A `checkpoint`
records the last fully-resolved bucket so a restart skips it.

## Scale — the 9 PM / one-million-reminders problem

The defining failure mode: a million actors each with a reminder due at 21:00. Fire them naively and
the node melts — a million durable reads, a million cold activations, a million messages, in one tick.
The design makes this **graceful by construction**, in order of leverage:

1. **The drain rate — the primary lever, and the reason accuracy is traded.** A reminder never fires
   on its exact instant. The winning mechanism (SEGSTREAM, ADR-017) binds the due segment to **one
   bounded 024 `StreamChannel` drained under a 022 fire-rate token bucket**: `spread` is expressed as
   a **drain rate** `fire_rate`, so the peak dispatch is **`fire_rate` exactly** — a *structural*
   credit-window bound, not a statistical smear. With `fire_rate = 3333/s`, 1 M reminders "at 21:00"
   drain as a flat **3333/s for ~300 s** instead of 1 M in one bucket. Deterministic (drain rank =
   `splitmix64(actor ^ name)`) so the smear is byte-identical across restarts and compilers.
   **This is what "lower accuracy than a timer" buys.** *(Measured on the dev box, clean build:
   10⁶-at-one-instant → peak 3333/bucket == `fire_rate`, 300× flatter than the naive spike;
   `reminder_bench.cpp` §A.)*

2. **Owner-sharded, no global scan.** Each reminder is owned by exactly one node — HRW `place(actor)`
   over the live set (010/026), the same function as actor placement, so *no coordinator and no
   duplicate firing*. A node scans only its ~`1M/N` reminders. On churn, reminders re-place with
   minimal disruption (only ~`1/N` move to the new/leaving node), and the new owner simply finds them
   in its next scan — the durable store is the handoff.

3. **Coarse bucketed tick.** The service ticks at bucket granularity and loads only the due bucket, so
   per-tick work is proportional to *what is due now*, not to the total reminder population.

4. **Bounded reactivation pipeline + 022 governance.** Firing feeds the normal admission path, so
   022's **shed-don't-buffer**, bounded queues, and rate limiter cap the reactivation wave: due
   reminders drain through a bounded-concurrency pipeline, and if the node is saturated they fire a
   little later rather than exhausting memory. Overdue-but-unfired is safe because delivery is
   at-least-once (below).

5. **At-least-once + catch-up — completion-gated (normative).** The service confirms a fire (advances
   the row's next-due / deletes a one-shot / advances the checkpoint) **only *after* the fire has
   run** — never on dispatch. This ordering is load-bearing, not a detail: ADR-017 measured the eager
   "advance-then-fire" anti-pattern **losing 709 committed reminders across 13 crash trials**, while
   the completion-gated ordering lost **0 across 42 crash/SIGKILL trials**. Because the durable **row
   is the truth** and is mutated only on completion, a crash before confirmation leaves the row, so on
   reopen the reminder is due again and **re-fires with the same `(name, scheduled_due)` dedup key**.
   After downtime the service fires everything overdue since the last checkpoint — coarsely — so
   nothing is lost, but a reminder **may fire late or more than once**. Handlers must therefore be
   **idempotent** (017): dedup on that deterministic key, carried in the fired message.

The result: the worst case degrades to "the 21:00 reminders all fire within their spread window,
paced by the node's capacity," never "the node falls over at 21:00."

## Wall-clock anchoring (018)

Reminders fire at **civil/absolute time**, so they read a **real-time** clock
(`pal::wall_now` → `CLOCK_REALTIME`), *not* the monotonic `BootClock` that deadlines use (018 —
`pal::now`). This is a deliberate second clock domain: a deadline must be immune to wall-clock steps
(NTP, admin set-time), a reminder must *follow* them (21:00 is 21:00 even after an NTP correction).
The consequences the spec pins:

- **Clock steps / DST.** A backward step can make a bucket "become due again"; the checkpoint +
  at-least-once + idempotency contract absorbs it (a re-fire is tolerated, never a crash). A forward
  jump fires the skipped buckets as catch-up.
- **Timezones are the caller's business.** The store holds a UTC `WallInstant`; "21:00 Asia/Ho_Chi_Minh"
  is resolved to UTC at registration by the caller (a timezone library is an optional adapter, never
  in the std-only core), and periodic re-resolution across DST boundaries is a documented seam.

## Self-debate

- **Reminder vs. "durable timer"?** Named "reminder" (Orleans' term) precisely because it is *not* a
  more-durable timer — the accuracy, clock domain, delivery guarantee, and scale model are all
  different. Calling it a durable timer would invite callers to expect ms precision and exactly-once,
  which the scale design cannot give. The name encodes the trade.
- **Jitter is lossy — is smearing acceptable?** Yes, and it is the whole point. A caller who needs
  "exactly 21:00:00.000" wants a timer on an always-on actor, not a reminder. Reminders exist for work
  where "within a few minutes of 9 PM, reliably, for a million actors" is the actual requirement, and
  for that, spread is a feature. The window is configurable down to 0 for low-cardinality reminders.
- **At-least-once vs. exactly-once?** At-least-once. Exactly-once for a *side-effecting* fire (charge a
  card) is the 017 problem, solved the 017 way (idempotency key + atomic effect-commit), not by the
  scheduler pretending it can fire exactly once across restarts and partitions. The scheduler
  guarantees *at-least*-once and hands the dedup key to the handler.
- **A separate `ReminderStore` seam, or reuse the event `Store`?** Separate. The event `Store` is
  indexed by `(actor, seq)` for replay; reminders are indexed by `(due_bucket, actor, name)` for a
  time scan. Same physical backends, different access pattern — forcing both through one interface
  would bloat the seam every adapter must implement. Keeping them parallel keeps each minimal.
- **Per-reminder or per-actor ownership?** Per-actor (owner = `place(actor)`), so all of an actor's
  reminders and its state co-locate on one node — the same locality the cache-affinity work buys, and
  it means a fire is a *local* `tell`, never a cross-node hop.

## Non-goals

- **Not a cron/workflow engine.** No DAGs, no dependencies-between-jobs, no calendar expressions
  beyond a `WallInstant` + `period`. Those compose *above* reminders, in user actors.
- **Not sub-second precision.** Bucket-granular ± jitter. Need ms precision → use an 011 timer.
- **Not exactly-once.** At-least-once by contract; exactly-once effects are 017's job.
- **Not a general distributed job queue.** A reminder targets a *specific actor identity*; it is not a
  work-stealing queue of anonymous tasks (that is the 025 `StatelessPool`).
- **Not a timezone database.** The core stores UTC instants; civil-time/DST resolution is an adapter.

## Interaction

- **011 (Timers)** — the ephemeral sibling and the *local delivery vehicle*: once a due bucket is
  loaded, dispatch to the actor is an ordinary `tell`, and short in-process follow-ups still use the
  wheel. 027 is durable + civil-time + node-spanning; 011 is in-memory + monotonic + local.
- **012 (Persistence)** — reminders ride the same adapter families through the parallel `ReminderStore`
  seam; the durable store *is* the cross-restart/cross-owner handoff (PersistenceAdapters.md).
- **010 / 026 (Placement)** — reminder ownership is `place(actor)`; re-placement on churn moves only
  ~`1/N` reminders and needs no coordination (the durable row is the source of truth).
- **022 (Governance)** — the mass-due wave is shed/paced by the bounded reactivation pipeline; a
  reminder storm cannot violate the node's overload ceilings.
- **018 / 019 (Clocks/PAL)** — reminders read a new `pal::wall_now` (CLOCK_REALTIME) domain, distinct
  from the monotonic deadline clock; a wall-clock source is a small new PAL seam.
- **001 / ADR-008 (Activation)** — firing reactivates a passivated actor (cold activation), then the
  actor may re-arm the next period; idle-deactivation and reminder-reactivation are complementary.
- **017 (Delivery)** — at-least-once + a deterministic dedup key; side-effecting handlers dedup there.
- **008 / 016 (Metadata/Serialization)** — the payload is a described message, stored and re-decoded
  as canonical-tagged bytes, so reminders survive schema evolution.
- **009 (Observability)** — new metrics: due-lag (fire time − scheduled), fire rate, overdue depth,
  reactivation-storm shed count — the surface that proves the scale contract holds in production.

## Dependencies

The **012 `Store` seam** (a durable backend — `FileStore` is the std-only default; SQLite/RocksDB are
opt-in) plus a **wall-clock PAL source** (019, `CLOCK_REALTIME`, a small addition to the existing
clock seam). No other new dependency; the std-only build gets durable reminders via `FileStore`. A
timezone/civil-calendar library is an **optional adapter**, never linked into the core.

## Open questions

- **Bucket granularity `B` and drain rate** — *resolved for the reference case:* `B = 1 s`,
  `fire_rate = 3333/s` (spread ≈ 300 s) proven near-uniform (ADR-017); kept as **per-reminder-class
  config**, not one global value. Guardrail: `fire_rate = 0` (fire-all fast path) must be **clamped
  above a cardinality threshold** at registration — an unclamped mass-due with no drain rate collapses
  10⁶ into one bucket and melts the node.
- **Catch-up storm bound after long downtime** — a node offline for a day wakes to many overdue
  buckets. Per-segment drain paces each segment, but the *cross-segment* backlog rate still needs an
  explicit cap policy (a bounded catch-up cap exists but is not yet independently benchmarked —
  ADR-017 residual #4).
- **Physical cross-node handoff on re-placement** — the durable row lives on the *old* owner's disk;
  a re-placed actor's reminders reach the new owner only via a **021 state-transfer / shared-replicated
  store**. The placement math + per-row owner filter are proven on one box (ADR-017 C3p); the physical
  two-node handoff firing exactly once is the **single tie-breaking follow-up experiment** (residual #1).
- **Cold-open rebuild bound** — SEGSTREAM's `open()` rebuilds the due-index `O(total-owned)`;
  `compact()` bounds the WAL to `O(live)`, but a durable **index snapshot** (as the wheel-mirror design
  proved) should be ported into recovery to bound rebuild latency (residual #3).
- **Civil-time / DST / timezone re-resolution** — a backward wall-clock step re-arms a drained
  segment → a re-fire wave, correctness-safe via idempotency but load-ungoverned beyond the same drain;
  the clock-step policy and where recurring civil schedules re-anchor across DST remain open (residual #5).

## Maturity

**Accepted** ([ADR-017](decisions/ADR-017-durable-reminder-mass-due-scale-gate.md)). The load-bearing
claim — *a mass-due wave (10⁶ reminders at one civil instant) flattens instead of melting the node* —
is **empirically proven** (and by two independent designs). The `design → debate → prove` loop ran 3
competing designs through red-team cross-examination and executed C++23 (sanitizers + benchmarks); the
**SEGSTREAM** design won on a full sweep of CORRECT verdicts, and the numbers were **re-measured from a
clean build on the dev box**:

| Property | Proven result | Where |
|---|---|---|
| Mass-due flattens | 10⁶-at-one-instant → **peak == `fire_rate` (3333/bucket), 300× flatter than N**; deterministic + restart/compiler-stable | `reminder_bench.cpp` §A, test §1 |
| O(due-now) scan | idle tick **flat ~24 ns** from 10³ to 10⁶ dormant (ordered-map begin-peek) | `reminder_bench.cpp` §B, test §2 |
| Durable + at-least-once | **zero committed-reminder loss** across crash (ADR: 42 SIGKILL trials); eager-advance control loses 709 | test §3 (`FileReminderStore` reopen) |
| Idempotent re-fire | duplicate delivery → one effect on `(name, scheduled_due)` | test §4 |
| Register/cancel | **one `fdatasync`/op**, O(1) index | `reminder_bench.cpp` §C |

**Named residuals that are NOT promoted to a promise** (tracked as open questions above, from ADR-017):
physical cross-node durable-row handoff (a 021 seam), whole-node RSS under *real* ADR-008 cold
reactivations (bounded only while `fire_rate ≤ passivation throughput` — a 022/023 tuning constraint),
and a durable index snapshot to bound cold-open rebuild. These are integration/hardware seams, not the
027 scale gate — that gate is met.
