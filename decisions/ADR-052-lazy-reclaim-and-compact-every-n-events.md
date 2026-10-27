# ADR-052 — Reclaim evicted lazy activations; fixed-count `CompactEveryNEvents<N>` (#24b/#24c)

Status: Accepted (opt-in reclaim; honest caveats below). Evidence: `tests/engine_lazy_reclaim_test.cpp`
(10^5 distinct lazy keys return `lazy_owned`/`id_table` to baseline; reactivation; default-off),
`tests/persistence_compact_every_test.cpp` (InMemoryStore + FileStore, reopen, empty-tail, fence).

## 1. CompactEvery
ADR-024 chose wall-clock `CompactEvery<T>` + a background `CompactionJob`. That needs a persistence
I/O pool and a timer-driven internal message; it is not implemented here. This ADR ships ADR-024's
documented **fixed-count fallback**, named `CompactEveryNEvents<N>` (not `CompactEvery<N>`, to avoid
colliding with ADR-024's `T` = time):
- `EventLog::compact_if_due<Cadence>(state)` — called after `commit()` with the state folded through
  the last commit; after >= N events past the last snapshot it saves a snapshot at
  `through_seq = next_seq-1`, then `store.truncate_log(id, fence, through_seq)`. Synchronous, on the
  handler-completion path, never on the dispatch drain loop. Failure of either step is returned,
  loses nothing, and is retried at the next call.
- Store seam: a refinement concept `CompactableStore` (`truncate_log`), NOT added to `Store`, so
  existing/third-party adapters still compile. `truncate_log` is fence-checked, refuses unless a
  snapshot with `through_seq >= requested` exists, idempotent. Implemented in `InMemoryStore` and
  `FileStore` (WAL rewritten to `{fence, snapshot, tail}` in `<file>.compact`, fdatasync, atomic
  `durable_rename`, fd reopened; replay now raises `last_seq` to a snapshot's `through_seq` so a fully
  truncated log still continues its sequence). **Not done:** SQLite/RocksDB adapters (a
  `DELETE ... WHERE seq<=?` / `DeleteRange` is trivial but untestable here: both are opt-in builds
  not compiled in this tree) and `EncryptedStore` forwarding; they simply do not model
  `CompactableStore` yet. Engine auto-wiring of the cadence into an actor policy pack is also not
  done: the actor (or its EventLog owner) calls `compact_if_due`.
- Rejected: counting inside `EventLog::commit()` (it has no `State`, and snapshot serialization must
  not hide inside a commit). Rejected: truncating without requiring the covering snapshot (a bug
  would silently drop un-snapshotted events).

## 2. Reclaim on idle eviction (`EngineConfig::lazy_reclaim_grace_ms`, default 0 = off)
Today eviction only parks the activation Dormant; the actor, `Activation`, `Schedulable`, `LazyOwned`
and `id_table` entry live forever. Freeing from the evicting worker is unsafe: (a) the worker is
running inside that activation; (b) any producer may hold the `Schedulable*` it got from `resolve()`
between `resolve()` and `post()` (the id table is read lock-free, and `ActorRef` holds no refcount).

Design (all on the shard's drain owner, the already-sole writer of `lazy_owned`/`id_table`, so no new
synchronization and nothing added to the drain loop except one cold branch on the already-cold
"went Dormant" path):
1. On a clean eviction, `note_evicted` records `due = now + idle_ticks + grace` (idle_ticks so any
   stale wheel entry that still holds the activation's pointers has fired) and queues it.
2. The wheel-tick `reclaim_sweep`, phase A: CAS `Dormant -> Retired` (new `ExecState`; loses cleanly to
   a racing readmit) and unlink the id-table entry (entry memory kept, `next` intact for readers
   standing on it).
3. After another grace period, phase B: if a stale-pointer producer stranded a message in the Retired
   mailbox, relink + `Retired -> Scheduled` (rescue, no loss); else free entry, actor, activation.
`lazy_owned` became `unordered_map<Schedulable*, LazyOwned>` (O(1) erase, no element moves).
Semantics: a reclaimed actor is re-created by the broker on next touch; non-persistent state is lost
(this is what passivation means), persistent state recovers through the existing path.
Diagnostics: `Engine::lazy_owned_size()`, `Engine::id_table_size()`.

### Caveats (honest)
- **Not a proof of safety against arbitrarily stalled producers.** A thread descheduled between
  `resolve()` and `post()` for longer than ~2x `lazy_reclaim_grace_ms` can still touch freed memory.
  Grace is time-based, hence opt-in and default off; choose it far above worst-case scheduling stalls.
- A rescued stranded message is handled by the old instance, not a fresh one (it was never freed).
- The retire queue is scanned linearly per wheel visit while non-empty (fine at 10^5; a bucketed
  queue is the follow-up if 10^6+ idle at once matters).
- `ReplyCell`/timer/reminder holders of raw `Activation*` for a lazy actor are assumed to be absent
  when the mailbox is empty and the activation Dormant (a parked ask would keep it non-Dormant).

### Rejected alternatives
- Epoch/hazard pinning in `resolve()`/`ActorRef`: sound, but adds a store+fence (or RMW) to every
  producer lookup for lazy actors; violates "no work added to the hot path".
- Type-stable recycled slots: avoids UAF but a stale pointer would deliver to a different actor id.
- Freeing the actor object only (keeping Activation/id entry): does not return containers to baseline.
- Keep-active on failed passivation flush: deferred, unchanged from ADR-050.

## Hot-path impact
`Activation::wake_after_enqueue`, `drain_step`, `close_out*` untouched. `run_activation` gains one
branch executed only when an activation just went Dormant (cold, once per eviction), and the wheel
tick gains an `empty()` check. No allocation on the drain path (`retire_q`/map allocate in the cold
broker/sweep). No new bench was added; the full suite (230 tests) passes and the existing
`bench-gate` covers the untouched dispatch path.
