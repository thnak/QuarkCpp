# 012 — Persistence

Optional durability for actor state: survive restart (007), survive node loss
(010), and recover deterministically. Persistence is **opt-in per actor** and, by
default, requires **no external database** — the built-in store is an append-only
log plus snapshots on the local filesystem. Heavier backends plug in behind one
seam.

> An actor without a persistence policy is pure in-memory and pays nothing.

## Two models, chosen by policy

```cpp
class Account : public quark::Actor<Account,
                    Sequential,
                    Persistent<Snapshot,  PersistMode::Sync>> {};

class Ledger  : public quark::Actor<Ledger,
                    Sequential,
                    Persistent<EventSourced, PersistMode::Batched>> {};
```

| Model | Stores | Recovery | Cost |
|---|---|---|---|
| **Snapshot** | Latest serialized state | Load last snapshot | Cheap read; write proportional to state size |
| **EventSourced** | Append-only log of state-changing events | Replay log (from last snapshot + tail) | Cheap append; auditable + replayable; log must be compacted |

### Alternatives considered

- **Snapshot-only:** simplest, but loses the audit/replay/debugging value that
  makes event sourcing attractive for financial/MES-style domains.
- **Event-sourcing-only:** powerful but forces log compaction complexity on every
  persistent actor, including trivial ones.
- **Decision:** offer both as a policy; Snapshot is the low-friction default,
  EventSourced is opt-in. EventSourced periodically writes a snapshot to bound
  replay length (snapshot = compaction checkpoint).

## Storage seam

```cpp
struct StateStore {
    virtual std::expected<Snapshot, error>  load(ActorId) = 0;
    virtual std::expected<void, error>      save(ActorId, Snapshot) = 0;
    virtual std::expected<void, error>      append(ActorId, EventBytes) = 0;
    virtual EventCursor                     read_log(ActorId, SeqNo from) = 0;
};
```

Default: **per-shard append-only log file + periodic snapshot files** (a WAL, not
an LSM engine). Sequential writes; durable flush is governed by `PersistMode` and
issued through the Platform Abstraction Layer (019) — `fdatasync` (Linux),
`F_FULLFSYNC` (macOS), `FlushFileBuffers` (Windows) — so the store logic is
OS-agnostic. No external
process. Adapters (RocksDB, SQL, object storage) implement `StateStore` for teams
that need them — never linked into a default build.

Serialization is the single mechanism defined in `016-Serialization.md`, shared
with the wire (010) — one reflection-free `describe` per type, not two. Durable
records (snapshots and events) always use 016's **canonical tagged encoding**
(never the wire fast path) and are prefixed with `{type_key, schema_version}` so
future code can read old bytes; breaking changes are handled by the migration
chain (016). A type persisted here **must** have a `describe` — enforced at
Validation (008).

## When state is persisted — `PersistMode`

| Mode | Semantics | Trade-off |
|---|---|---|
| `Sync` | Persist the mutation before the message completes / before an `ask` reply is sent | Durable; adds store latency to the message |
| `Batched` | Persist asynchronously, coalescing writes; ack before durable | Fast; bounded window of loss on crash |

`Sync` is the safe default for actors whose replies imply durability (payments,
inventory commits). `Batched` suits high-throughput actors that tolerate replay
gaps. The choice is per actor and does not change handler code.

### Interaction with the single-executor invariant

**EventSourced staging fence (ADR-009 C7).** Events raised by a handler **stage in a
per-message buffer** and become durable only at the **handler-completion commit point**
(015). A **throwing handler commits nothing** — so a subsequent `Restart` → reload replays
only pre-poison committed state and can never resurrect a poison handler's partial append
(proven: stage `debit(100)` then throw → durable log empty, reconstructed balance 0; normal
path → log `[100]`). This closes durable "Resume-on-corrupted-state."

Under `Sync`, a persistent actor's write happens on its own lane between messages;
because execution is sequential (001), there is no concurrent mutation to
serialize against its own log. For `Reentrant` actors, commit steps still execute
in the synchronous region at handler completion on the single lane (015), so they
are **naturally serialized**; each commit takes a monotonically increasing commit
sequence number. Per-event appends need no quiescence; a consistent point-in-time
**snapshot** uses `quiesce(Drain)`. See `015-Reentrancy-and-Quiescence.md`.

## Log compaction and checkpoint mode (EventSourced, ADR-024, proven)

```cpp
class Ledger : public quark::Actor<Ledger,
                   Sequential,
                   Persistent<EventSourced, PersistMode::Batched,
                              /* Cadence = */ CompactEvery<30000>>> {};
```

`Cadence` is consulted **only** when `Model = EventSourced`; Snapshot-model and
non-persistent actors never instantiate it — zero cost when unused.

- **The checkpoint write is always asynchronous/background, orthogonal to
  `PersistMode`.** `PersistMode` continues to govern only per-event-commit
  durability timing (`Sync` = commit before reply; `Batched` = fire-and-forget,
  coalesced). The periodic snapshot/compaction write is a separate, always-
  background side channel that never blocks the actor's lane in either mode —
  proven flat, 4–48µs handler duration independent of injected store delay (F2).
- **Idle-deactivation is a required compaction trigger, not an optimization
  (C6).** Without it, a wall-clock-only cadence can be starved indefinitely by an
  idle-deactivation window shorter than `CompactEvery<T>`, silently degrading the
  replay bound to "the actor's entire lifetime."
- **Fence-ordered publish discipline is normative**, not incidental — these three
  rules closed the two fatal gaps (S3, C1) found during ADR-024's
  cross-examination and are part of the contract for any `Store` adapter that
  opts into background compaction, not an adapter-specific implementation
  detail:
  1. Durable snapshot files are named/keyed by `(fence, through_seq)` so a
     slower, stale writer can never overwrite a newer one by completion order.
  2. The in-memory hot-cache publish is a compare-and-swap keyed on
     `(fence, through_seq)`, never an unconditional store.
  3. The `CompactionApplied` close-out message itself carries the issuing
     activation's fence/epoch and is dropped as a no-op if it doesn't match the
     current activation's fence or doesn't strictly advance the watermark.
- **`Store` adapter concurrency obligation.** Any `Store` adapter used with
  background compaction must serialize cross-thread access to its shared
  per-actor state (closed here via a per-shard/per-actor CAS gate) —
  `store_conformance.hpp`-style tests must include a genuine two-thread stress
  case per adapter (`FileStore`, `InMemoryStore`, and any future
  `SqliteStore`/`RocksStore`) rather than assuming internal locking from
  marketing claims ("ACID").
- **`CompactEveryNEvents<N>` (fixed-count cadence) is retained as a documented
  alternative/fallback policy, not the default.** For actor types where a hard,
  exact replay-count bound matters more than avoiding a periodic lane-latency
  spike (e.g. actors already running exclusively under `PersistMode::Sync`,
  where the added synchronous checkpoint cost is proportionally smaller), it
  remains a fully proven, safe cadence policy (8/8 CORRECT) that a per-actor-type
  override may select.
- **Open follow-up (not yet proven): hybrid early-trigger addendum.** The
  wall-clock-only cadence above has a disclosed residual — an unbounded-within-
  one-interval tail under sustained high-rate bursts. An optional secondary
  byte- or count-threshold could fire an *early* `CompactionTick` between
  wall-clock ticks (reusing the fixed-N counter, proven exact, as the
  early-trigger condition, but still routed through the async background path,
  not a synchronous one). This was not itself red-teamed or proven in ADR-024
  and is the natural next-iteration target if a tighter replay bound becomes a
  hard requirement (e.g. driven by 023 recovery-time budgets).

## Recovery

- **On activation** of a persistent actor: `StateStore::load` the snapshot, then
  (EventSourced) replay the tail log to reconstruct current state, **before** the
  first message is delivered. Recovery is part of activation, not a message.
- **On `Restart` (007):** reload from the store rather than reconstructing empty
  state, so a restart recovers durable state, not just fresh state. The poison
  message is still dead-lettered so recovery + poison don't loop. **Reload returns
  `std::expected`** (adapter exceptions caught at the store seam); a reload failure
  **escalates** (Stop + dead-letter survivors under the held quiescence seal, 015) rather
  than releasing onto empty state (ADR-009 C6). The **fencing-token bump** happens on
  reconstruct.
- **On re-placement (010):** the new owning node loads the actor's state from the
  store keyed by `ActorId` (`type_key` + key, 008). This requires the store be
  reachable from the new node — see below.

## Placement, mobility, and fencing

- **Local-only store** ⇒ actors are effectively pinned to a node's disk; migration
  requires state transfer.
- **Shared/replicated store** ⇒ actors are mobile: any node can recover any actor.

To prevent split-brain double-activation across a partition (010), each activation
acquires a monotonically increasing **fencing token** persisted with the state;
the store rejects writes carrying a stale token, so a partitioned old activation
cannot corrupt state after re-placement.

## Encryption at rest (optional)

Durable records may hold sensitive state. **At-rest encryption is optional
envelope encryption** ([020-Security](020-Security)): 016's canonical tagged
bytes are produced first (so schema evolution/migration is unchanged), then sealed
with a per-actor/per-shard data key wrapped by a `Keyring` seam before hitting the
disk record. The fencing token above is integrity-relevant and is covered by the
record's AEAD tag, so a stale token cannot be spliced onto fresh ciphertext. A
build without at-rest encryption configured pays nothing and links no crypto.

## Dependencies

Std + the Platform Abstraction Layer's file I/O and durable-flush backend
(`fsync`/`FlushFileBuffers`) for the default WAL store. Databases and object stores
are optional adapters behind `StateStore`.

## Open questions

- *(Log compaction cadence for EventSourced actors, and whether snapshots are
  synchronous checkpoints or background: resolved — wall-clock `CompactEvery<T>`
  cadence, checkpoint write always async/background regardless of `PersistMode`,
  idle-deactivation as a required trigger. See "Log compaction and checkpoint
  mode" above, ADR-024.)* *(Schema evolution: resolved, 016.)*
- Exactly-once effects: combining at-least-once delivery (010) + idempotent,
  fenced persistence to get effectively-once semantics. *(For the `ask`-retry-across-restart
  case this folds into `OnRestartAsk<Retry<N, IdempotencyKey>>` (007, ADR-009): a fenced
  idempotent retry commits its durable effect exactly once — proven effect==1 vs unfenced==2.
  Effectively-once across nodes remains as resolved in 017.)*
