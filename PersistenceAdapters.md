# Persistence storage adapters (012)

Quark's durability is built as a **seam, not a hardcoded database**. One narrow contract — the `Store`
concept — sits between the durability *mechanism* (snapshots, event log, recovery, fencing) and the
*backend* (RAM, a file, SQLite, RocksDB, …). Swapping backends touches only an adapter struct; the
snapshot/event-log/recovery code in [`snapshot.hpp`](include/quark/core/snapshot.hpp) and
[`event_log.hpp`](include/quark/core/event_log.hpp) never changes.

> This is the "multiple databases via adapter" model. A backend is a ~200-line struct that satisfies
> seven methods; everything above it — `Persistent<Snapshot|EventSourced, Sync|Batched>`, commit
> staging, replay, recovery, split-brain fencing — is backend-agnostic and already proven.

## The seam — one concept, seven methods

The `Store` concept ([`persistence.hpp`](include/quark/core/persistence.hpp)) is a **compile-time
concept**, not a virtual base, so the default in-process store pays zero dispatch (a runtime
`StateStore` erasure is still expressible over it for heterogeneous arrays):

```cpp
template <class S>
concept Store = requires(S& s, ...) {
    { s.acquire_fence(id) }            -> std::same_as<FenceToken>;   // hand out a strictly-greater owner token
    { s.last_seq(id) }                 -> std::same_as<SeqNo>;        // highest committed seq (resume the counter)
    { s.load_snapshot(id) }            -> ...optional<SnapshotRecord>;// latest state checkpoint
    { s.save_snapshot(id, tok, snap) } -> result<void>;              // write a checkpoint (fence-checked)
    { s.append(id, tok, seq, bytes) }  -> result<void>;              // one event
    { s.append_batch(id, tok, batch) } -> result<void>;              // ALL-or-NONE atomic commit
    { s.read_log(id, from) }           -> result<EventCursor>;       // replay tail (seq ≥ from)
};
```

The records are opaque: `SnapshotRecord{fence, through_seq, bytes}` and `EventRecord{seq, bytes}`,
where `bytes` is the **016 canonical-tagged** encoding (schema-evolution safe). A backend never
interprets the payload — it only stores and returns bytes keyed by `(ActorId, seq)`.

## The contract every adapter MUST honour

These are the invariants the mechanism relies on; an adapter that breaks one corrupts state. The
shared conformance harness ([`tests/adapters/store_conformance.hpp`](tests/adapters/store_conformance.hpp))
checks all of them against a real reopen ("crash"):

| Invariant | Rule | Why |
|---|---|---|
| **Fencing** | `acquire_fence(id)` returns a token strictly greater than any yet issued for `id` and records it as owner. Any write with an **older** token is rejected with `errc::unavailable`. | Split-brain: a partitioned/superseded old activation cannot overwrite a newer owner's state (012/010). Must be **durable** — a stale writer stays fenced across a restart. |
| **Strict sequence** | `append`/`append_batch` reject any `seq ≤ last_seq`. | Replay is totally ordered; a snapshot names the exact prefix (`through_seq`) it subsumes. |
| **Atomic batch** | `append_batch` commits **every** record or **none** — validated whole *before* any durable mutation. | `EventLog::commit()` flushes a multi-event handler commit once; a crash/failure mid-batch must not leave a torn partial commit (ADR-009 C7). |
| **Snapshot ≤ tail** | `save_snapshot` rejects `through_seq > last_seq`. | An over-advanced checkpoint would skip un-replayed events (silent loss). |
| **Durability (for `PersistMode::Sync`)** | An acknowledged write survives process exit / power loss (fsync before returning). | The whole point of a durable backend. |

## At-rest encryption (020 §5) — the decision

**Decision (P4):** the durable record layer is encrypted with `EncryptedStore<S>`
([`at_rest.hpp`](include/quark/core/at_rest.hpp)) whenever a deployment holds data whose compromise
matters. A bank deployment **MUST** wrap every durable adapter (`FileStore`, `SqliteStore`,
`RocksStore`) with `EncryptedStore` backed by a KMS/HSM `Keyring`, **and** run on encrypted volumes
with encrypted backups. The plain adapters remain plaintext by default: at-rest crypto never links
unless the deployment opts in, and `MockCipher` is test-only framing, never production crypto.

**Threat model.** Protects against a stolen disk, volume snapshot, backup, or a storage operator who
can read files but not the live process/KMS: the canonical 016 bytes are AEAD ciphertext on disk.
It does **not** protect against a compromised process, a compromised KMS/key, or traffic on the
network (use the mTLS transport), and it does **not** hide metadata: actor TypeKey/key (the
`<type>-<key>.qwal` filename), sequence numbers, record sizes/counts, fences and timestamps remain
visible. Volume encryption covers that metadata; it is defense in depth, not a replacement.

**Mechanism.** The layer above produces the canonical 016 `encode_record` bytes FIRST, so schema
evolution/migration is untouched; `EncryptedStore` then seals only the opaque `record` bytes.
Snapshots bind `(actor, Snapshot, through_seq, fence)`, events/batches bind `(actor, Event, seq)` in
the AEAD associated data, and the nonce is derived from the same context. Consequently a ciphertext
cannot be spliced onto another actor, another sequence, a snapshot vs event, or the wrong fence —
`open_at_rest` fails and the store read errors. Fencing, strict-seq, atomic-batch and
snapshot≤tail are delegated unchanged to the wrapped adapter.

**Key management / ownership.** The platform operator owns the KMS/HSM, the per-actor (or
per-shard) data keys, access policy, backups, and rotation. The application owns the decision to
wrap each store and the `Keyring` implementation. `StaticKeyring` is a single-tenant dev aid, not a
KMS. The current envelope has no key-id field: rotate by rewrap (read with the old key, seal with
the new, atomically replace) or a dual-key `Keyring`; online rotation/key-id versioning is a
documented seam, not implemented.

**Deployment checklist.**

- [ ] Every durable store is wrapped in `EncryptedStore`; no raw `FileStore`/SQLite/RocksDB handle
      is reachable by application code.
- [ ] `Keyring::data_key(actor)` comes from a KMS/HSM or OS keystore, not a source constant.
- [ ] Volumes, snapshots and backups are encrypted (metadata + defense in depth).
- [ ] Key access is least-privilege and audited; the key is not in images/logs/argv.
- [ ] A restore drill proves an old backup opens with its key; a rotation/rewrap runbook exists.
- [ ] Tamper/wrong-key/cross-actor controls pass (`security_at_rest_store_test`).
- [ ] `MockCipher` is never linked into a production build.

**Verification.** `security_at_rest_seal_test` proves the primitive's fence/seq AAD binding;
`security_at_rest_store_test` proves the store decorator over both `InMemoryStore` and `FileStore`:
raw ciphertext (no canonical bytes on disk), batch sealing, reopen/replay, tamper rejection,
cross-actor splice rejection, and wrong-key rejection.

## The adapters

| Adapter | Backing | Durable? | Dependency | Status |
|---|---|---|---|---|
| [`InMemoryStore`](include/quark/core/persistence.hpp) | RAM (`unordered_map`) | No (process-lifetime) | std-only | **shipped** — the reference; exercises every seam property |
| [`FileStore`](include/quark/core/file_store.hpp) | append-only WAL file per actor + `fdatasync` | **Yes** | std-only + POSIX | **shipped + verified** (`persistence_filestore_durable_test`) |
| [`SqliteStore`](include/quark/adapters/sqlite_store.hpp) | SQLite (embedded, single-file, ACID) | **Yes** | `libsqlite3` (opt-in) | **shipped**, built when `QUARK_WITH_SQLITE=ON` |
| [`RocksStore`](include/quark/adapters/rocksdb_store.hpp) | RocksDB (embedded LSM KV) | **Yes** | `librocksdb` (opt-in) | **shipped**, built when `QUARK_WITH_ROCKSDB=ON` |

**How each meets the contract:**

- **FileStore** — one `.qwal` file per actor. Every op appends one CRC32-framed record
  (`FENCE`/`SNAPSHOT`/`BATCH`) and `fdatasync`s; a batch is a **single** framed record (one CRC, one
  sync) ⇒ atomic on disk. On open the file is replayed; a **torn trailing record** (bad length/CRC
  from a crash mid-write) is detected and truncated, so recovery never sees a partial write. Fencing
  owner is journaled ⇒ durable across restart.
- **SqliteStore** — three tables (`meta`, `snapshots`, `events`); a batch is **one SQL transaction**
  (`BEGIN IMMEDIATE … COMMIT`) with `PRAGMA synchronous=FULL` ⇒ ACID atomicity + durability. Bonus:
  the state is queryable/inspectable with any SQLite tool.
- **RocksStore** — one LSM DB; keys are **big-endian** `"E"+actor+seq` so a range scan returns events
  in order; a batch is one `WriteBatch` written with `sync=true` ⇒ atomic + durable, with an LSM's
  append-optimized write path for high commit rates.

## CMake option matrix

```bash
# std-only core + reference + durable FileStore — nothing extra to link:
cmake -B build                                  # InMemoryStore + FileStore always available

# opt in to a library backend (links the lib ONLY when requested):
cmake -B build -DQUARK_WITH_SQLITE=ON           # needs libsqlite3-dev  -> quark::persistence_sqlite
cmake -B build -DQUARK_WITH_ROCKSDB=ON          # needs librocksdb-dev  -> quark::persistence_rocksdb
```

When an option is ON, CMake locates the library, exposes an INTERFACE target carrying its include +
link, and (with tests on) builds a crash-durability conformance check (`sqlite_store_check` /
`rocksdb_store_check`) so the adapter is *verified*, not just compiled. If the dev package is missing,
configure fails with a message naming the package to install — never a confusing link error.

> The SQLite/RocksDB adapters — for **both** the event `Store` and the reminder `ReminderStore` seams —
> are written against their stable public APIs but are **not compiled on the current dev box** (their
> `-dev` headers aren't installed there); they are syntax- and concept-checked (`static_assert(Store<…>)`
> / `static_assert(ReminderStore<…>)`) under GCC and Clang. Install `libsqlite3-dev` / `librocksdb-dev`
> and flip the option to build + run their conformance checks (`*_store_check` and
> `*_reminder_store_check`).

## A parallel seam: the `ReminderStore` (027)

Durable **reminders** (027 / [ADR-017](decisions/ADR-017-durable-reminder-mass-due-scale-gate.md))
reuse the **same backend families** through a *parallel* seam, because their access pattern differs:
the event `Store` is keyed `(actor, seq)` for replay, whereas reminders are keyed `(actor, name)` with
a due-time-ordered index and a full-set replay on cold open. Forcing both through one interface would
bloat the seam every adapter implements, so `ReminderStore`
([`reminder_service.hpp`](include/quark/core/reminder_service.hpp)) is kept minimal and distinct — five
methods:

```cpp
template <class S>
concept ReminderStore = requires(S& s, const ReminderRow& row, ReminderKey key, std::int64_t bucket) {
    { s.put(row) }              -> std::same_as<result<void>>;                 // UPSERT (durable)
    { s.remove(key) }           -> std::same_as<result<void>>;                 // cancel / one-shot done
    { s.load_all() }            -> std::same_as<result<std::vector<ReminderRow>>>; // replay on cold open
    { s.checkpoint(bucket) }    -> std::same_as<result<void>>;                 // "resolved through here"
    { s.checkpoint_bucket() }   -> std::same_as<std::int64_t>;                 // last checkpoint
};
```

The **same four provider families** as the event `Store` now ship for reminders, so a deployment picks
one storage engine for both seams:

| Adapter | Backing | Durable? | Ordered due-index | Dependency | Status |
|---|---|---|---|---|---|
| [`InMemoryReminderStore`](include/quark/core/reminder_service.hpp) | RAM (`unordered_map`) | No (process-lifetime) | in-RAM `std::map` (service) | std-only | **shipped** — the reference |
| [`FileReminderStore`](include/quark/core/reminder_service.hpp) | CRC32-framed append-only WAL (`.qrem`) + `fdatasync` | **Yes** | rebuilt on open (`compact()` bounds it) | std-only + POSIX | **shipped + verified** (`reminder_store_conformance_test`) |
| [`SqliteReminderStore`](include/quark/adapters/sqlite_reminder_store.hpp) | SQLite (embedded, single-file, ACID) | **Yes** | native `rem_due` SQL index | `libsqlite3` (opt-in) | **shipped**, built when `QUARK_WITH_SQLITE=ON` |
| [`RocksReminderStore`](include/quark/adapters/rocksdb_reminder_store.hpp) | RocksDB (embedded LSM KV) | **Yes** | native — big-endian due prefix in the key | `librocksdb` (opt-in) | **shipped**, built when `QUARK_WITH_ROCKSDB=ON` |

**How each meets the seam:**

- **FileReminderStore** — one `.qrem` file. Every `put`/`remove`/`checkpoint` appends one CRC32-framed
  record and `fdatasync`s (one sync/op, ADR-017 F1p); on open the log is replayed into the live-row
  map and a **torn trailing record** (crash mid-write) is detected and truncated. `compact()` rewrites
  only live rows, bounding cold-open rebuild to O(live).
- **SqliteReminderStore** — a `reminders` table keyed `PRIMARY KEY(actor_type, actor_key, name_hash)`
  (so `put` is an `ON CONFLICT … DO UPDATE` UPSERT) with `PRAGMA synchronous=FULL` (fsync/op) and a
  `rem_due` index on `scheduled_due_ns`, so `load_all()` returns rows **pre-sorted by due time** — the
  service rebuilds its due-index without re-sorting. The checkpoint is one row in `rem_meta`.
- **RocksReminderStore** — keys are **big-endian** `"R" + due + actor + name_hash`, so a range scan
  returns reminders in due order (native on-disk due-index); a small `"I"` (key → current due) index
  lets a re-arm `Delete` the stale due-keyed row so an UPSERT **replaces, never duplicates**. Each
  mutation is one `WriteBatch` with `sync=true` ⇒ atomic + durable, on the LSM's append-optimized path
  (built for reminder churn: a periodic reminder re-armed on every fire is a durable upsert per fire).

Every durable adapter is checked against **one shared crash-durability harness**
([`tests/adapters/reminder_store_conformance.hpp`](tests/adapters/reminder_store_conformance.hpp)) —
put/remove/UPSERT/checkpoint, then **reopen on the same path** and assert nothing acknowledged was
lost, upserts don't duplicate, and same-name-different-actor keys stay distinct. `FileReminderStore`
runs it on this box (`reminder_store_conformance_test`, in the default `ctest` run); the
SQLite/RocksDB reminder checks (`sqlite_reminder_store_check` / `rocksdb_reminder_store_check`) build
behind the same `QUARK_WITH_*` flags as the event-store checks, so the adapters are *verified* wherever
the dev headers are installed — not merely compiled.

## Adding a new backend (Postgres, S3, Cassandra, …)

1. Write `struct MyStore { /* the 7 methods */ };` in `include/quark/adapters/`.
2. `static_assert(Store<MyStore>);` — the concept tells you at compile time if a method is missing.
3. Honour the contract table above (fencing, strict-seq, atomic batch, snapshot ≤ tail, durability).
4. Add a `QUARK_WITH_MYSTORE` option + `find_package` block in
   [`cmake/QuarkPersistenceAdapters.cmake`](cmake/QuarkPersistenceAdapters.cmake) and a
   `tests/adapters/mystore_check.cpp` that calls `run_store_conformance`.

No engine, snapshot, event-log, or recovery code changes — that is the entire point of the seam.
