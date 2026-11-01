# ADR-075 — Pinned NodeId: 63 bits of the key plus an application-assigned role bit (issue #63)

Status: Proposed. Amends ADR-068 section 1 (identity) and meets ADR-074 D3 (client role, NodeId bit 63). No wire change.
Consumer: AeroCoWorker ADR-044 (LAN group: every device a peer, some devices attached as clients), RFC 002 section 6e item 1.

## Problem
ADR-068 derives a pinned NodeId from all 64 bits of the first 8 bytes of SHA-256(Ed25519 key). ADR-074 D3 (merged later, #38) reads bit 63
of a NodeId as the CLIENT marker (`is_client_node`): a member never dials a client (`keep_local_dial`, `local_is_client`), and SWIM, relay and
placement refuse client ids. The two together make bit 63 of a pinned id a coin flip: about half of all pinned keys were classed as clients.

Evidence (#49): after #38 `pinned_flood_check` and `pinned_tcp_check` became flaky; 3 of 8 Windows runs failed, exactly the runs whose two
random keys had different bit 63. Commit 43f2c11 masked it in the tests (redraw keys until bit 63 is clear); the product defect stayed. For
AeroCoWorker a LAN group of peers would have had half its devices unreachable by dialling (a member never dials a "client").

## Decision (option 1 of the issue, refined)
The ROLE is the application's, recorded with the key in its trust data; the NodeId's bit 63 states it; the other 63 bits come from the key.
- `NodeId = (LE64(SHA-256(key)[0..8]) & ~kClientNodeBit) | (role == Client ? kClientNodeBit : 0)`. A key whose 63 bits are all zero (2^-63) is refused.
- `PinnedRole {Member, Client}` in `core/pinned_key.hpp`. `PinnedTrust::role_of(cluster, key)` returns the admitted role or nullopt; its default
  is "admitted => Member", so existing providers keep working. `PinnedKeyBook::admit(cluster, key, role = Member)`; `key_of(cluster, node)` answers
  only when the id's bit 63 equals the admitted role, so the directory and the admission check agree.
- `PinnedIdentity::generate/from_secret_key(..., role)` knows its own role and derives its own NodeId with it.
- Handshake, no wire change (Msg1 and Msg2 already carry the NodeId). Each verifier checks (a) the low 63 bits equal the derivation from the
  authenticated key, (b) bit 63 equals the role ITS trust data holds for that key. Failure is silent (zero bytes, ADR-072). The initiator applies
  the same checks to the responder (`key_of` + derivation before it sends anything, and the role is re-read when Msg2 arrives).
- Role conflict in the book: a key is admitted in ONE role per cluster. Admitting it again in the other role is REFUSED (returns false, nothing
  changes); the application changes a role with `revoke()` then `admit()`. Refusing rather than replacing means a role flip is never a side effect
  (a stale "admit as Member" cannot demote a Client). Roles are per cluster.
- Collision refusal (and the A/-A rule) works on the 63-bit space: the book keys on the 63 key bits, so two keys that collide there are refused
  whatever role either holds.
- Nothing else in ADR-074 changes: cluster.hpp, relay and placement keep reading bit 63.

## What ADR-074 turned out to depend on (found while proving this)
`SecureTransport::handle_anonymous_opener` (the pinned accept path, #45/#48) required the opener to be the LOWER NodeId (`peer < self`), ignoring
D3's role rule. A client id has bit 63 set, so it is always numerically higher: a pinned client dialling a member was refused. It was never
exercised because no pinned id could be reliably a client. Fixed to `local_is_client(peer, self)` (the same predicate the dialler uses): between
member and client the client opens, between same-role peers the lower id (unchanged). T3a fails without this change.

## Claims and tests (`tests/adapters/pinned_nodeid_role_check.cpp`, real TCP + SecureTransport + Noise IK on the pinned wire; each with a control)
- T1 two members whose raw SHA-256 bit 63 differs (50 key pairs, drawn until they differ) link and deliver both ways, one session each, neither is a
  client. CONTROL: the same keys under `NodeIdDerivation::LegacyRaw` (the old derivation, a test-only knob in `PinnedOptions`/`PinnedKeyBook`/identity):
  one key of every pair is classed a client, the lower member never dials it, no link (0 of 4).
- T2 a Member key presenting bit 63 set, and a Client key presenting it clear, are refused with zero bytes and the refusal is the role check; a wrong key
  part is refused; an initiator holding the peer as Member sends nothing to a client-bit id. CONTROL: `PinnedOptions::check_role_bit = false` accepts both.
  Book policy: same-role re-admit is a no-op, other-role admit refused and role unchanged, revoke then admit changes it, `key_of` follows the role bit.
- T3 a device admitted as Client: `keep_local_dial` keeps its dial, it dials and delivers both ways (real TCP), a member's dial to it forms nothing (no
  delivery, no session), SWIM `admit` refuses it, placement never makes it eligible. CONTROL: the same key admitted as Member links by the lower-id rule, is
  admitted by SWIM and eligible.
- T4 the pinned checks run with fully random keys (the masking loop in `pinned_test_util.hpp` is removed): see results.
- The Noise vector check is unchanged and does not use a NodeId (it drives the Noise state machine with the official vector's keys and payloads), so the
  derivation change cannot affect it.

## Results
Windows 11, MSVC Release, sodium + mbedTLS, full ctest: 280 of 280 passed. `pinned_flood_check` and `pinned_tcp_check` 20 runs each: 0 failures.
Linux (thnak-server, GCC 15, libsodium 1.0.18): ASan+UBSan and TSan, pinned_*, noise_ik, security_client_role, multi_cluster_isolation, admission_*,
tcp_admission_redial: 16 of 16 each, no sanitizer report. (The vector file needed LF endings after the Windows copy; unrelated to the change.)

## Residuals
- The role is as trustworthy as the application's trust data: both ends must hold the same role for a key or they refuse each other (by design, silent).
- 63-bit key space instead of 64: collisions are refused by the book, an application with its own provider must do the same (as in ADR-068).
- Changing a device's role changes its NodeId (bit 63): the application re-pairs it (`revoke` then `admit`) and peers treat it as a new id; live sessions are
  not touched (#41).
- Not covered: the 63-bit collision refusal is by construction (slot = key bits), not by a found collision.
- `LegacyRaw` exists only for the control; it must never be selected outside tests.
