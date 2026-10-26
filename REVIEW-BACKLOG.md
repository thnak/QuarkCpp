# Review backlog — locks, performance, memory, persistence

Findings from the 2026-10-26 review of the 2026-10-25 security commits plus an Opus read-only sweep
of the codebase. Complements [SECURITY-TRACKING.md](SECURITY-TRACKING.md) (security remediation matrix).

**Status of the Opus findings:** read from code only — none were reproduced, built or run. Re-verify
each one before fixing. Line numbers are as of the review and will drift.

Legend: `[ ]` open · `[~]` fixed in working tree, untested/uncommitted · `[x]` done and tested.

## A. Fixed in the working tree (uncommitted, no new tests yet)

Full suite on `root@192.168.0.138` (GCC 15.2, Release): 216/216 pass. ASan/UBSan/TSan and Clang not yet run (A8).
Note: when shipping the tree from Windows, strip CRs from `*.sh` on the Linux side.

- [~] A1 Handshake spoof/hijack: ignore Authenticate frames for a parked engine from a different
  source IP; `known` no longer bypasses the per-IP cap (`secure_transport.hpp`).
- [x] A2 Pending inbound TCP sockets: 5 s hello deadline + per-IP cap of 64 (`tcp_transport.hpp`); tested in tcp_transport_pre_auth_test.
  **Incomplete — see C3.**
- [~] A3 Tagless decode `reserve` capped at 1024 (`wire.hpp`).
- [~] A4 Removed the `~SecureTransport` write through the raw `inner_` pointer.
- [~] A5 Audit sink moved out from under `pending_mu_` in `handle_authenticate_frame`.
- [~] A6 Stale include comment in `security.hpp`.
- [x] A7 Tests added: hello timeout + per-IP socket caps (`tcp_transport_pre_auth_test`), handshake hijack-ignore
  + known-peer-no-bypass (`security_wire_hardening_test::handshake_spoof_resistance`, verified to FAIL on the
  pre-fix SecureTransport). Existing loopback tests pass with the new per-IP caps (full suite 216/216).
- [ ] A8 Run ASan/UBSan/TSan (`-j1`) and Clang on thnak-remote. (The `stream_drain_zero_rmw` failure seen earlier
  was a CRLF artifact of shipping a Windows checkout to Linux — the index has `*.sh eol=lf` — not a repo bug;
  full suite is 216/216 once the CR is stripped.)
- [x] A9 Concurrent-overshoot of `max_pending_handshakes_` (cap check and insert are separate
  critical sections): harmless only if receive is single-threaded — confirm, or make atomic.

## B. Persistence — not production-ready

- [x] **B1 CRITICAL — snapshot nonce reuse (fixed: random per-record nonce stored as 8-byte prefix; test added).** `at_rest.hpp:129-135,191`: nonce is a deterministic hash of
  (actor, kind, through_seq, fence). Snapshot flushes always pass `through_seq = 0`
  (`metadata.hpp:411`) and a Dormant activation (same fence) is reused on wake
  (`engine.hpp:1170-1172,1214,1227`), so different plaintexts are sealed under one nonce → GCM
  keystream XOR leak and tag forgery. Fix: random 96-bit nonce stored with the record, or a persisted
  counter. Add a test that two seals of the same context never share a nonce.
- [x] B2 HIGH (fixed: fence acquired first; ordering test in engine_lazy_persistence_test, fails on old order) — recovery loads state **before** `acquire_fence` (`engine.hpp:1200` then `:1214`); a stale
  owner can write in between. Acquire the fence first, then load.
- [~] B3 HIGH (InMemoryStore + FileStore fixed with a mutex; SqliteStore/RocksStore still open) — stores used from many threads with no locking (`file_store.hpp:162-172` `table_`;
  `sqlite_store.hpp:61-67,114-120` shared connection). Spec 012 (lines 127-133) requires serialized
  access and a two-thread conformance stress; `store_conformance.hpp` has no threads.
- [ ] B4 HIGH — fence/seq checks outside the write transaction (`sqlite_store.hpp:86-93,108-114`;
  `rocksdb_store.hpp:81-87,101-112`; `acquire_fence` is a non-atomic RMW at `:58-62`).
- [~] B5 HIGH (FileStore/InMemoryStore: acquire_fence failure returns invalid token 0 and writes under 0 are refused; SQLite exec()/COMMIT/BEGIN error handling + Rocks still open) — swallowed errors: SQLite `exec()` ignores return codes (`:162`); failed `COMMIT` returns
  success (`:120`); failed `BEGIN` runs inserts in autocommit (partial batch, breaks ADR-009 C7);
  `acquire_fence` returns the *current* owner's token on failure (`file_store.hpp:73`,
  `sqlite_store.hpp:68`, `rocksdb_store.hpp:61`); `FileReminderStore` ignores `durable_flush` failure
  (`reminder_service.hpp:342,227`). Make `acquire_fence` return `result`.
- [ ] B6 HIGH — read errors look like "no data" (SQLite `:73-82,124-131,164-173`; Rocks `:69-70,152`):
  recovery seeds default state and the next flush overwrites the real snapshot.
- [~] B7 HIGH (FileStore fixed: mid-file CRC failure marks the log corrupt, no truncation, ops refused; torn tail still truncated; FileReminderStore still open) — replay truncates at the first CRC mismatch and deletes later committed records
  (`file_store.hpp:186-192`; `reminder_service.hpp:370-384`). A torn mid-file record in
  `FileReminderStore` (`:332-335`) drops later appends.
- [ ] B8 MED — replay trusts record contents (`file_store.hpp:274-289` unchecked indexing, CRC32 is not a
  MAC, batch seq can go backwards `:209-216`); FENCE records are plaintext under `EncryptedStore`;
  truncation/rollback of the file is undetectable.
- [ ] B9 MED — no directory fsync after create/rename (`file_store.hpp:167`; `reminder_service.hpp:229`;
  `pal/linux_x86_64/file_io.hpp:88-89`; Windows `MoveFileEx` without write-through). Reminder
  `compact()` ignores flush failure, then renames; failed compaction leaves `fd_` closed (`:219-229`).
- [ ] B10 MED — `ReminderService` re-register leaves a stale due-index entry (`:437-439,539-542`): fires
  early with the new due time; a later-rescheduled one-shot fires at the old time and is removed
  (`:431-440,494-515`). Store errors in `tick` ignored (`:509,513,522`).
- [~] B11 MED (InMemoryStore + FileStore reject lower through_seq; adapters still open) — stale snapshots can overwrite newer ones; no store rejects lower `through_seq`
  (`persistence.hpp:182-195`). Spec 012 keys snapshots by (fence, through_seq).
- [x] B12 MED — transport nonce reuse when the handshake is disabled: a reconnect resets seq to 0 under
  the shared cipher (`secure_transport.hpp:139-143,387-399,755-759`).

## C. Memory

- [x] C1 HIGH (fixed: per-peer byte caps data 64 MiB soft / control 256 MiB hard + posted-bytes cap in send(); tcp_transport_backpressure_test) — outbound TCP queues unbounded (`tcp_transport.hpp:347-350`; post queue in
  `pal .../net.hpp:286`). ADR-046 gate only sees the peer's mailbox signal, not local socket backlog.
  Add a per-peer byte cap shedding data frames (never control), with a drop counter.
- [ ] C2 HIGH — idle eviction frees nothing: `engine.hpp:1238` `lazy_owned` never erased;
  `close_out_retire` only goes Dormant (`activation.hpp:665-671,709`). ADR-028 residual #3 does not
  disclose this.
- [x] C3 HIGH (fixed: per-IP cap on identified inbound conns + strict peer-address binding; tcp_transport_pre_auth_test) — **unauthenticated hello bypasses the pending-socket caps** (`tcp_transport.hpp:628-637`
  `register_identified`): a fresh fake NodeId moves the socket into `conns_`, escaping `max_pending_in_`
  and the per-IP cap; each holds fd + `Conn` + up to 4 MiB reassembly. Also (security, not executed):
  on the larger-id node a hello claiming a *real* peer's id wins dedup against the legitimate inbound
  connection (`:654-666`) and black-holes traffic. Cap identified-but-unauthenticated connections per
  IP; make the peer slot authoritative only after the secure session exists. **Completes A2.**
- [ ] C4 HIGH — `FileStore` keeps all history in RAM and one fd per actor (`file_store.hpp:128,157,309`);
  replay reads whole file (`:177,249-260`); no compaction (ADR-024/012 `CompactEvery` unimplemented);
  each passivation appends a full snapshot (`metadata.hpp:411`).
- [x] C5 HIGH (fixed in S4: subject check + caps + counters; third-party revocation needs signed revocations - 020 has none, left as residual) — uncapped gossip-fed maps: `last_pressure_` (`cluster.hpp:932,1502`),
  `RevocationRegistry` union-only with full copy per change (`revocation_registry.hpp:33-41,85-86`;
  any member can revoke any other's cert), `CapabilityRegistry` (`capability_registry.hpp:195-200`).
  Accept entries only for `m.from`/verified members; cap the revocation set.
- [x] C6 LOW (gates_ half fixed in S4; peer_schemas_ part is secure_transport, not S4) — congestion `gates_` never evicted (`governance.hpp:313-316`); `peer_schemas_` not cleared
  on failed renegotiation (`secure_transport.hpp:555-561`).

## D. Lock retention

- [x] D1 HIGH — `sweep_rotation`/`sweep_revocations` (`secure_transport.hpp:258-271,193-201`) take each
  session mutex while holding `map_mu_` (shared); a long seal blocks `on_peer_disconnected`
  (`:141`, needs `map_mu_` exclusive) and, on writer-preferring locks, every `try_get_session`.
  Copy session pointers out, release `map_mu_`, then inspect; make `generation`/`renegotiated_at` atomic.
- [x] D2 HIGH — timer service fires user code under its mutex (`timer_service.hpp:106-120,207-241`);
  catch-up after suspend = thousands of ticks under the lock; a message destructor cancelling a timer
  would self-deadlock (`:248`; plausible, not executed). Pop due entries under the lock, fire outside.
  FIXED (verified real: old code deadlocked in tests): tick pops due entries under `mu_`, fires/destroys
  outside it (adv_mu_ keeps tick order); cancel racing a popped entry is at-most-once. Test:
  `tests/timer_service_reentrancy_test.cpp`.
- [x] D3 MED — audit sink called under the session lock (`secure_transport.hpp:722,731`); same class as A5.
- [x] D4 MED — unsynchronized shared state (TSan would flag): `receiver_` (`:305` vs `:742`),
  `revoked_snapshot_` (`:179,191` vs `:515-517,592-594,666-668`), `frames_sealed_on_key()`
  (`:266`, `peer_session.hpp:80`).
- [ ] D5 MED — handshake crypto runs on the single TCP I/O thread (`drive_handshake` → `advance()` from
  `do_read`, `tcp_transport.hpp:603-607`); add a per-IP *rate* limit and/or move to a pool.
- [x] D6 LOW — `send()` holds the session lock across `inner_->send` (`:241`); synchronous
  `LoopbackTransport` SWIM Ping→Ack re-enters and self-deadlocks (tests only). Engines destroyed and
  256-entry scans under `pending_mu_` (`:508,585,626,825`).

## E. Performance

- [x] E1 HIGH (fixed: Transport::peer_accepts_type, sorted advert + binary search, test in security_handshake_test) — every remote send copies the peer schema (≤32 KiB) under node-wide `schema_mu_`
  (`distribution.hpp:78-82`, `secure_transport.hpp:297-302`, linear `contains` at `wire.hpp:632-636`).
  Regression from F17. Use a `shared_ptr<const>` / sorted set and a boolean "accepts type" API.
- [x] E2 HIGH (fixed: shared detail::RateBucket + sustained-rate test in security_wire_hardening_test) — rate limiters starve instead of limit: `last_ns` advances even when integer division adds
  0 tokens (`distribution.hpp:348-355`, `cluster.hpp:1412-1419`). Above ~4 k frames/s a peer gets ~0
  admitted; on the control plane it sheds SWIM acks → false suspicion. Advance `last_ns` only by the
  time converted into tokens. Add a sustained-rate test.
- [ ] E3 HIGH — remote send ≈ 6 allocs, 3 payload copies, 1 syscall (`distribution.hpp:302-309`;
  AAD without `reserve` `secure_transport.hpp:768-781`; `seal()` instead of `seal_into()` `:234-237`;
  `std::function` closure `tcp_transport.hpp:187`; eventfd write per post `pal net.hpp:283-288,327-331`;
  double copy in `wire_codec.hpp:103-111`; inbound `wire_codec.hpp:132`, `secure_transport.hpp:701,713`).
- [ ] E4 MED — false sharing: `messages_processed` shares a line with producer-written
  `mailbox_enqueued`/`activations` (`metrics.hpp:147-153`, `activation.hpp:432-434,455,493`).
- [ ] E5 MED — global mutexes on `ask` (`actor_ref.hpp:383-393` `cp_mu_`; `reply_cell.hpp:177,192`;
  `envelope_pool.hpp:51,70`).
- [ ] E6 MED — `DistributedRouter::tell` locks SWIM `mu_` + `shared_ptr` copy + O(n) HRW even for local
  sends (`distribution.hpp:180`, `cluster.hpp:545-548`); congestion gate takes a shared lock
  (`governance.hpp:307-311`) contradicting its "one relaxed load" comment (`:248`).
- [ ] E7 MED — disk I/O on scheduler threads: recovery + `acquire_fence` on the broker lane
  (`engine.hpp:1200-1214`), deactivation snapshot inline on a worker (`activation.hpp:688-695`).
- [ ] E8 LOW — per-peer send/receive share one session mutex (`secure_transport.hpp:224,715`); SWIM
  re-sorts the roster per gossip (`cluster.hpp:1383-1393,1452-1467`); `FrameStream::feed` always copies
  (`wire_codec.hpp:160,173`).

## F. Other review nits (from the original review)

- [ ] F1 `QUARK_ALLOW_INSECURE` compile-time bypass of the plaintext gate: ensure CI/release builds
  cannot define it; document.
- [ ] F2 Hashed rate-limit buckets (SWIM control + per-peer data, 256 slots): a spoofed id can burn a
  legitimate peer's bucket (targeted DoS, not only memory growth).
- [ ] F3 Local-send ACL (F11) is opt-in; document that the default is trusted internal sends.
- [ ] F4 Not yet reviewed: CI/supply-chain commits (`18e40fa`, `714569e`, `fix(ci)` series), `47f7179`
  in depth, and the new security tests.

## Suggested order

1. B1 (cryptographic break) · 2. E2 + E1 (regressions from the 2026-10-25 commits) · 3. C3 + C1
(complete A2, bound queues) · 4. B2–B7 (persistence correctness) · 5. D1/D2 (lock nesting) ·
6. the rest. Each fix needs a test (load-bearing invariant) and, on hot paths, a bench
(CLAUDE.md "Working within this repo").
