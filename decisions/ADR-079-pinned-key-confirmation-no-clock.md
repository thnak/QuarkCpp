# ADR-079 — Pinned wire: a new session counts only after the initiator proves its keys, and Msg1 freshness stops depending on the clock (issue #53)

Status: **Proposed, rev. 3** (2026-11-02). Design only. Rev. 1 and rev. 2 were each red-teamed the same day (round 1: 9 findings, two P0; round 2: 6 P1 and 3 P2, verdict
"needs rev. 3"); rev. 3 answers both rounds and has NOT been red-teamed again. Proof outstanding (*Evidence required*). Amends ADR-068 (replay rule, H10 limits, red-team
items 4 and 5) and ADR-072 item 4 (a handshake-identified connection supersedes). Consumer: AeroCoWorker network security assessment M8 (R12, TH-05, TH-06, SEC-05, SEC-06),
RFC 002 section 6e. Wire: Msg1 and Msg2 keep their sizes (154 and 58 bytes); one Msg1 field changes meaning; the Noise prologue label becomes `quark-pinned-key/v2`.

## Problem
ADR-068 refuses a replayed Msg1 by two rules: its timestamp must be within `max_clock_skew_ns` (default 120 s) of the responder's wall clock, and strictly greater than the
latest timestamp accepted from that initiator key (`PinnedReplayGuard`, `include/quark/adapters/sodium/pinned_handshake.hpp`). Two defects:

1. **Restart replay (SEC-05).** The guard is in memory. After the responder restarts (or evicts the key), a Msg1 captured inside the skew window is accepted once. The responder
   installs a session for the initiator on the REPLAYER's new connection (`SecureTransport::handle_anonymous_opener`: `install_session_from_result` then
   `identify_inbound(token, peer)`, which supersedes the live connection), and the real initiator's traffic goes to a connection nobody can read until it redials. The replayer
   learns no key; the harm is availability, and the attacker needs only a capture and the ability to open a TCP connection (any LAN host).
2. **Clock lock-out (SEC-06).** Both rules read wall clocks. An initiator that restarts with its clock behind stamps a Msg1 older than the latest the responder holds and outside the
   skew window, and is silently refused until the clock catches up (`next_timestamp()` rises only within one process). Anyone who can spoof NTP on the LAN can push a responder's
   clock out of the window of every initiator, or pull it back to widen it for old captures.

## Options considered (the three in the issue)
| Option | Fixes SEC-05 | Fixes SEC-06 | Cost | Verdict |
|---|---|---|---|---|
| (a) responder nonce round trip before Msg1 | yes | no (the clock stays unless the nonce replaces it) | one RTT on every connect; the responder must send bytes to a connection that has not proven it is an admitted initiator, which breaks "zero bytes to a prober" (ADR-068, ADR-072 T3) | rejected |
| (b) persisted per-initiator record on the responder | yes | no | a durable write before every Msg2 and a storage seam Quark does not own; a lost or rolled-back file reopens the hole | rejected |
| (c) persisted monotonic counter on the initiator | no (the responder's record is still in memory) | yes | a storage seam on every initiator and its crash safety | rejected as the whole answer |

None removes the real cause: the responder tries to PREVENT a replayed Msg1 from being accepted, and cannot without state that survives a restart or a clock it can trust. This ADR
makes an accepted replay HARMLESS, and then needs neither.

## Decision
### D1 — Key confirmation for a new connection: the handshake result is provisional until a frame opens under its key
After the responder accepts a Msg1 that opens a NEW inbound connection (all existing checks: admitted key, role bit, NodeId derivation, decrypt) it derives the transport keys as
today, but the result is PROVISIONAL. While provisional:
- it is not visible to `has_session()`, the router or placement, and no application or schema frame is sent on it;
- it does NOT replace, supersede or close an existing session or connection for that initiator;
- it is keyed by the inbound connection token (monotonic: `accept_serial_`), so two provisional results for one key never collide;
- its lifetime is the connection's: a dedicated `TcpTransport::provisional_timeout_ns` (default 5 s, applying ONLY to connections that hold a provisional result, not to every inbound
  connection) kills the connection, and `SecureTransport` drops the entry on the close notification (T3). There is no per-key cap: provisional connections stay in `pending_in_`,
  so the existing global and per-IP pending caps bound them, which is the same exposure junk connections already have today.

**Confirm.** The initiator, right after it processes Msg2, sends an empty sealed `Confirm` frame (a new `FrameKind`; the decoder must accept it) as the FIRST frame after Msg1 on that
connection: it emits `Confirm` under the session lock before the session is released to application senders. The first frame on that connection that OPENS under the
initiator-to-responder key confirms the provisional result; the empty `Confirm` is consumed, any other opening frame is also delivered. A frame that does not open is dropped and
counts against the bound in T1.

**Promotion** (responder, on the I/O thread, in this order; any failed step closes the connection and installs nothing):
1. refuse if a provisional or promoted result for the same peer with a HIGHER token has already been promoted (record `last_promoted_token[peer]`): a delayed `Confirm` of an
   older connection cannot displace a newer one;
2. re-check the revocation fingerprint set (`revoked_snapshot_`) AND the admission snapshot, whether or not `admission_.enabled()`, and run `close_hook_`;
3. BIND the connection (`identify_inbound`) first, and only if it is still alive install the session under the peer, then send the schema frame. (Bind-then-install: a connection that
   died in between never swaps live keys.)
`sweep_revocations` and `on_admission_published` also iterate the provisional store and drop entries whose peer is now revoked or denied. This promotion is the supersede.

**Transport changes this needs:**
- **T1.** An unidentified inbound connection allows exactly one frame today (the Authenticate opener) and is killed on any other. After the opener yields a provisional result the
  connection is treated as TENTATIVELY identified as that peer: `from` is restored from the token's peer for nonce and AAD, frames are handed to `SecureTransport` marked provisional,
  and `SecureTransport` accepts only a frame that opens under the provisional key; everything else is dropped. The bound counts frames that FAIL to open (default 8) and the pre-auth
  byte bound is enforced in `FrameStream` as a length-prefix check: an unpromoted inbound connection accepts `max_body` 512 bytes, not the 4 MB default. Frames that follow the
  confirming frame in the SAME read chunk are processed as identified-peer frames: promotion on the I/O thread applies synchronously (no posted bind between frames of one chunk).
- **T2.** `Transport::send_to_inbound(token, frame)` (default: not supported). `TcpTransport` implements it for a pending inbound connection with a write path that pumps this Msg2
  (today `on_conn` pumps nothing until the hello progresses). Msg2 uses it, not the NodeId route, because binding is the supersede.
- **T3.** `Transport` reports an inbound token's close (`handle_dead` of a pending connection included) so the provisional entry is erased.

### D1' — The rekey path is NOT changed (a decision, with its reason)
A rekey Msg1 travels inside an ESTABLISHED, identified connection as an Authenticate frame with `from` set. Injecting one needs an attacker who can write into that live TCP stream
(sequence numbers), i.e. an on-path attacker, and such an attacker can already reset or close the connection. A staged "next" key slot (rev. 2's D1r) would stop a replayed rekey
Msg1 from swapping keys, but round 2 showed it costs a responder-to-initiator loss window on every legitimate rekey, drops the #48 throttle unless re-specified, and needs a defined
overwrite rule: three new defects to defend against an attacker who already has a simpler attack. So the rekey path keeps today's in-place install, with the ring of D2 applied to
rekey Msg1 too (an identical Msg1 gets zero bytes). It is a documented residual (see *Residuals*) and tests C8 and C8b pin it.

### D2 — Msg1 freshness is dedup by ephemeral key, with no clock
The 8-byte timestamp field of the Msg1 payload becomes an opaque 8-byte `nonce` (fresh `randombytes` per Msg1; Msg1 stays 154 bytes). The skew rule and the strictly-increasing rule are
removed; the handshake reads no wall clock (`clock_unix_ns`, `max_clock_skew_ns` and `next_timestamp` are deleted).
In their place the responder keeps, PER ADMITTED INITIATOR KEY, a ring of the last 32 ephemeral public keys `e` it accepted from that key (1 KB per key; the number of keys is bounded
by the application's admitted set). An identical `e` for that key is refused silently (zero bytes). A ring per key, not one global FIFO: an admitted member that handshakes thousands
of times can only evict its own entries. Only a Msg1 that passed admission, role and decrypt is inserted, so an unadmitted party fills nothing. `e` is bound by the AEAD associated
data of the encrypted static key (`noise.hpp` about lines 387-388), so a mutated `e` fails the tag.
**Version.** The version rides in the Noise prologue label, not in the payload field (`kPinnedVersion` is a payload field; `version_window` and `version_ok` accept a neighbour). So the
label becomes `quark-pinned-key/v2` and the pinned `version_window` becomes 0: a v1 and a v2 peer then fail the Noise MAC and refuse each other silently. The pinned wire has not
shipped (its ADRs are all Proposed), so there is no migration.

Consequences: SEC-06 holds by construction and NTP spoofing no longer matters to the handshake. A capture can be replayed again after a responder restart AND after the key's own
ring rotates (32 handshakes or rekeys by that key); each replay gets Msg2 and a provisional entry that D1 makes harmless. Rev. 2's statement "once per capture per boot" was wrong and is withdrawn.

### D3 — Controls kept as options (tests only)
`PinnedOptions::key_confirmation = false` restores install-at-Msg1; `PinnedOptions::legacy_timestamp_rule = true` restores the timestamp, skew and monotonic rule, the timestamp in Msg1
and the v1 label. Both exist only to run the failing control of each claim, like `replay_guard` and `check_role_bit` today, and are refused outside `QUARK_ALLOW_INSECURE` builds.

## Claims (each proven by a test with a failing control; `tests/adapters/pinned_confirmation_check.cpp`, real TCP + SecureTransport + Noise IK)
- **C1 (SEC-05).** Capture a genuine Msg1, restart the responder, replay it on a new connection: the replayer receives at most Msg2 (58 B) and nothing else for 6 s; the genuine
  initiator, connected before or after, delivers 50/50 both ways with no loss; the replayer's connection is closed. CONTROL: `key_confirmation = false` makes the replayer's connection
  the session and the initiator's frames stop arriving.
- **C2 (SEC-06).** An initiator whose clock is set back 10 minutes (injected clock, new process) connects on the first attempt and delivers 50/50. CONTROL: `legacy_timestamp_rule = true`
  refuses it for the whole step-back.
- **C3 (replay in a boot).** The same Msg1 sent twice to a running responder: the second gets zero bytes. CONTROL: dedup disabled accepts it.
- **C4 (no supersede before proof).** A live session for X, then a replay of an old Msg1 of X on a new connection after a responder restart: the live session is not closed or displaced
  and keeps delivering; the replayer's connection never receives an application frame; Msg2 does reach the replayer (T2). CONTROL: `key_confirmation = false` displaces it.
- **C5 (genuine reconnect supersedes).** A genuine reconnect supersedes at promotion, no duplicate, one reconnect counted: `pinned_tcp_check` T6 passes with only the adaptations T1-T3 require.
- **C6 (stockpile).** 50 DISTINCT captured Msg1s of X replayed after a restart, a genuine connect raced against them: the genuine one confirms; live provisional entries never exceed the
  pending caps; memory returns to baseline within the provisional timeout. **C6b:** X performs 33 handshakes, then capture #1 is replayed: it IS accepted (provisional) and is harmless
  (C4 holds); this pins the honest limit of the ring.
- **C7 (privacy unchanged).** `pinned_privacy_check` P1-P3 pass unchanged.
- **C8 (rekey regression).** A legitimate rekey under bidirectional load (100 000 frames per second each way, rekey forced) loses nothing in either direction; an idle rekey works.
  **C8b:** an identical rekey Msg1 sent twice on the established stream: the second gets zero bytes (ring applies to rekey).
- **C9 (ring is per key).** Capture X's Msg1; admitted member Y performs 4097 handshakes; replay X's Msg1 without a restart: zero bytes. CONTROL: a single global 4096-entry ring answers it.
- **C10 (mixed version).** A v1 initiator against a v2 responder and the reverse: both refuse with zero bytes, no half-open connection, on the Noise MAC. CONTROL: the label left at v1
  with `version_window` 1 accepts.
- **C11 (send race, burst).** N threads (N from 1 to 32) send immediately after Msg2, and the initiator writes `Confirm` plus 200 small frames in ONE TCP write: all 200 arrive, in order,
  no loss, no duplicate, whichever frame confirms. CONTROL: the T1 bound applied per read chunk kills the connection.
- **C12 (revocation at promotion).** Between its Msg1 and its confirming frame, X is revoked (a) through the admission snapshot, (b) through `set_revoked` on the fingerprint set, and
  (c) in a deployment with admission disabled: in all three no session is installed. CONTROL: promotion without the re-check installs.
- **C13 (older confirm).** Two connections from X; the older one's `Confirm` is delayed (proxy) until after the newer one promoted: the newer session survives and the older connection is closed.
- **C14 (bind-then-install).** The connection is reset after the confirming frame is read and before the bind completes: no session is installed and the live session's keys are unchanged.
- **C15 (pre-auth bounds).** A frame header declaring 4 MB on an unpromoted inbound connection kills it at the length prefix; 8 non-opening frames kill it; the provisional timeout reaps
  an idle connection on a quiet responder with no other traffic.
- The H10 limit tests of ADR-068 (restart replay, clock step-back) are replaced by C1 and C2; their old behaviour is kept as the controls.

## Residuals and what this does not do
- A replay after a restart, or after the key's ring rotates, gets Msg2 (58 B). That tells the replayer the responder is alive and still admits that key (a membership oracle for a key whose
  Msg1 it already holds; wider than before because a captured Msg1 no longer expires). It was already true under ADR-068's restart residual.
- Replays cost the responder the DH work of a Msg1 and a provisional slot for up to 5 s each, bounded by the pending caps and per-IP caps (the same exposure as junk connections, plus the DH).
- Rekey: an ON-PATH attacker who can inject into an established stream can replay an old rekey Msg1 after the ring has forgotten it and swap the session's keys, which breaks that
  session until the initiator redials. That attacker can already reset the connection; D1' records why this is not fixed here. Nothing is exposed.
- Removing the clock removes the only bound on how long a captured Msg1 stays usable; it is now bounded by the ring and by D1. D1 is the load-bearing part.
- The responder cannot send first on a new connection: it waits for `Confirm` (half a round trip).
- A man in the middle who forwards a genuine Msg1, Msg2 and the genuine `Confirm` unchanged is a relay, as before.
- Not decided here: option (c) stays available if a later requirement needs the responder to REFUSE (not neutralise) a cross-restart replay, and the staged next-key slot stays available if
  an on-path attacker becomes in scope.
- Sequencing note: the harm D1 removes is availability only. The transport changes T1-T3 are not small. The owner may defer implementation; D2 must NOT ship without D1.

## What changes
`include/quark/adapters/sodium/pinned_handshake.hpp` (Msg1 field, per-key ring replaces `PinnedReplayGuard`, prologue label v2, pinned `version_window` 0, options, delete the clock rules),
`include/quark/adapters/sodium/noise.hpp` (expose the received `e`), `include/quark/core/secure_transport.hpp` (provisional store by token, promotion order, `Confirm` frame, revocation and
admission sweeps over the store, schema frame after promotion), `include/quark/core/transport.hpp` (`send_to_inbound`, inbound close notification), `include/quark/net/tcp_transport.hpp`
and `include/quark/net/wire_codec.hpp` (T1: tentative identification, the 512-byte pre-auth `max_body`, the failing-frame bound, synchronous promotion within a chunk; T2 write path; T3;
`provisional_timeout_ns`; a `Confirm` FrameKind the decoder accepts), `tests/adapters/` (the new check; `pinned_tcp_check` T6 and the H10 tests adapted). ADR-068 section 7 and ADR-072
item 4 get a pointer here. No change to `peer_session.hpp`, `cluster.hpp`, SWIM, relay or the mbedTLS path.

## Red-team history
**Round 1 (rev. 1):** 1 P0 the confirmation frame is killed by the one-opener rule; 2 P0 Msg2 has no route on a provisional connection; 3 P1 rekey installs in place; 4 P1 one global FIFO is
emptied by any member; 5 P1 per-key cap with oldest-drop lets replays evict the genuine entry; 6 P2 no timer; 7 P2 no version bump; 8 P2 promotion must re-check, first frame may be data;
9 P3 membership oracle. Answered in rev. 2.

**Round 2 (rev. 2) and its disposition in rev. 3:**
| # | Sev | Finding | Disposition |
|---|---|---|---|
| 1 | P1 | The version is in the payload field, not the prologue; `version_window` 1 lets a v2 responder accept a v1 initiator that never confirms | **Accepted**: prologue label v2, `version_window` 0, C10 |
| 2 | P1 | D1r opens a responder-to-initiator loss window on every legitimate rekey; idle rekey strands the initiator; no claim covered it | **Accepted by removing D1r** (D1'); C8 pins a legitimate rekey |
| 3 | P1 | D1r drops the #48 rekey throttle; undefined `stage_next` overwrite | **Resolved by removing D1r** |
| 4 | P1 | T1's frame bound is per read chunk and `identify_inbound` is posted: a genuine burst after Confirm dies or is dropped with `from` = 0 | **Accepted**: tentative identification, bound counts failing frames, synchronous promotion within a chunk, C11 burst |
| 5 | P1 | Promotion misses the fingerprint revocation set and runs only if admission is enabled; sweeps skip the store | **Accepted**: promotion steps 1-3, sweeps over the store, C12 (a)(b)(c) |
| 6 | P1 | "Once per capture per boot" is false: the key's own handshakes rotate its ring | **Accepted**: claim withdrawn, honest limit, C6b |
| 7 | P2 | Pre-auth bytes buffered to 4 MB; `from` must be restored; `on_conn` pumps nothing for Msg2; no `handle_dead` hook | **Accepted**: T1 byte bound 512, T1 `from`, T2 write path, T3, C15 |
| 8 | P2 | No ordering across connections: an older connection's delayed Confirm can displace the newer | **Accepted**: promotion step 1 (`last_promoted_token`), C13 |
| 9 | P2 | Promotion races with connection close and swaps live keys in place | **Accepted**: bind-then-install, C14 |
| 10 | P3 | New `FrameKind` needed; per-engine flag; timeout reused for all inbound | **Accepted**: `Confirm` FrameKind, `provisional_timeout_ns` only for provisional connections; the flag is moot without D1r |
Claims that rev. 2 would have failed: C10, C11, C12; C6 and C9 were weak. Design points both rounds tried to break and could not: a mutated `e`, nonce or key reuse from `Confirm`,
a replayer opening `Confirm`, the schema frame and `has_session` before confirmation, token ABA, filling the ring by an unadmitted party, the ADR-076 glare cases, and the rejection of option (a).

## Evidence required before Accepted
A third red-team round on rev. 3. C1-C15 green under Release, ASan+UBSan and TSan on Linux (GCC and Clang). 20 runs of `pinned_tcp_check` and `pinned_flood_check` with 0 failures.
`pinned_privacy_check` unchanged.
