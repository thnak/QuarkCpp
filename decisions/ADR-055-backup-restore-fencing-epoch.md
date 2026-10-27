# ADR-055 — Online backup / restore of a `Store` with a fencing epoch (#23)

Status: Accepted (InMemoryStore + FileStore; honest caveats below). Evidence:
`tests/persistence_backup_restore_test.cpp` (round trip through the byte codec and image file for both
stores; backup under a concurrent committing writer; stale-owner rejection; FileStore reopen; corrupt
image rejection). Spec: 012 §"Online backup / restore".

## Verification of the issue's claims
- "spec 012 has no backup, restore or epoch": correct. No such seam existed (grep of specs and
  headers). Fencing tokens are **per actor** (`acquire_fence(id)` returns `owner(id)+1`); there is no
  store-wide epoch today, which is why this ADR introduces one only at restore time.
- "Acceptance gate: restore into a fresh cluster with a higher epoch, old owner's writes rejected":
  met by the tests, with the caveat on post-cut tokens below.

## Decision
1. **Optional concept `BackupableStore`** (persistence.hpp), in the `CompactableStore` style; the base
   `Store` is not widened. `snapshot_to(BackupImage&)` / `restore_from(const BackupImage&, FencingEpoch)`.
   `BackupImage` is store-neutral: per actor `{id, owner, last_seq, snapshot?, log}` + `epoch`.
2. **Consistency model.** `snapshot_to` copies all actors under the store's own mutex: one global cut,
   per-actor a committed prefix and atomic across actors (a multi-event `append_batch` is never half
   visible; actors committed in lockstep never show the later one ahead). Writers block only for the
   copy. FileStore lazily replays every WAL in its directory while holding the lock, so a backup of a
   large cold store stalls writers for that duration (acceptable v1; a per-actor cut is the escape
   hatch for DB adapters that cannot hold one lock, and they must document the weaker guarantee).
3. **Fencing epoch** `FencingEpoch = FenceToken`. `restore_epoch(img, min_epoch) = 1 + max(image epoch,
   every owner, every snapshot fence, min_epoch)`. `restore_from` sets **every** restored actor's owner
   to it (FileStore journals a FENCE record, so it survives reopen). A writer with any token `<=` the
   epoch fails the normal fence check with `errc::unavailable`; the next `acquire_fence` returns
   `epoch+1`. **Sharing with #22:** a leader lease / election term is the same object (a monotone
   integer compared with `<` by the store); a lease layer can pass its term as `min_epoch` or take the
   returned epoch as its floor without a new store seam.
4. **Restore targets a FRESH store**: refused if the store has loaded actors (or, for FileStore, any
   non-empty `.qwal`). Images are validated (`validate_backup`) before any mutation.
5. **Byte form** (`backup.hpp`): magic `QBAK0001`, host-endian fields, trailing CRC32. Image files are
   written `tmp` + flush + `durable_rename`.

## Caveats / not done
- **Tokens issued after the cut.** An old owner that acquired a token greater than the image's maximum
  after the backup is NOT fenced unless the operator passes `min_epoch` >= the highest token the old
  cluster may have issued (tested). The image cannot know the future; this is an operational
  obligation of the restore procedure (stop/fence the old cluster first, or supply a bound).
- **Per-actor tokens, not a global one.** Actors absent from the image keep no epoch; a later first
  `acquire_fence` on such an actor starts from 1. Restore into a store that will see ids outside the
  image should pass a `min_epoch`-aware higher layer (not implemented).
- **FileStore restore is not crash-atomic across actors** (each actor file is one flushed write; a crash
  mid-restore leaves a partial directory; discard and redo). Failure paths delete files written so far.
- **EncryptedStore** does not forward `snapshot_to`/`restore_from`, so it does not model the concept.
  Backing up the inner store captures sealed bytes, which restore only under the same keyring (key
  rotation across a restore is untested). The image file itself is plaintext of whatever the store
  holds and carries only a CRC, not a MAC.
- **SqliteStore / RocksStore**: not done (opt-in builds not compiled in this tree, cannot be tested
  here). Both could implement it with a read transaction / snapshot iterator.
- Not covered: the Reminder store, cluster-level quiescing, incremental backups, streaming sinks
  (the image is materialized in RAM), and engine-level automation (`Engine::backup()`).
- Observed while writing this (not changed): `FileStore::truncate_log` writes `{fence, snapshot,
  tail}`; if `through_seq` is below the stored snapshot's `through_seq`, surviving tail events have
  seq <= snapshot through_seq and replay (which raises `last_seq` at the snapshot record first)
  would flag the file corrupt. Restore deliberately writes events before the snapshot to avoid this.
