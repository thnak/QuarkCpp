# ADR-079 — Pinned wire: a session counts only after the initiator proves its keys, and Msg1 freshness stops depending on the clock (issue #53)

Status: **Proposed, rev. 2** (2026-11-02). Design only. Rev. 1 was red-teamed the same day (9 findings, two P0: see *Red-team round 1*); rev. 2 answers them and
has NOT been red-teamed again. Proof outstanding (*Evidence required*). Amends ADR-068 (replay rule, H10 limits, red-team items 4 and 5), ADR-072 item 4
(a handshake-identified reconnect supersedes) and the renegotiation install in `SecureTransport`. Consumer: AeroCoWorker network security assessment M8
(R12, TH-05, TH-06, SEC-05, SEC-06), RFC 002 section 6e. Wire: Msg1 and Msg2 keep their sizes (154 and 58 bytes); one Msg1 field changes meaning and
`kPinnedVersion` goes to 2.

## Problem
ADR-068 refuses a replayed Msg1 by two rules: its timestamp must be within `max_clock_skew_ns` (default 120 s) of the responder's wall clock, and strictly
greater than the latest timestamp accepted from that initiator key (`PinnedReplayGuard`, `include/quark/adapters/sodium/pinned_handshake.hpp`). Two defects:

1. **Restart replay (SEC-05).** The guard is in memory. After the responder restarts (or evicts the key), a Msg1 captured inside the skew window is accepted
   once. The responder installs a session for the initiator on the REPLAYER's connection (`SecureTransport::handle_anonymous_opener`:
   `install_session_from_result` then `identify_inbound(token, peer)`, which supersedes the live connection), and the real initiator's traffic goes to a
   connection nobody can read until it redials. The replayer learns no key; the harm is availability. The same hole exists on the REKEY path: a captured Msg1
   injected into a live stream with `from = X` takes `install_renegotiated`, which swaps both ciphers and zeroes the sequence counters at once
   (`secure_transport.hpp` about line 618, `peer_session.hpp:132-147`).
2. **Clock lock-out (SEC-06).** Both rules read wall clocks. An initiator that restarts with its clock behind stamps a Msg1 that is older than the latest the
   responder holds and outside the skew window, and is silently refused until the clock catches up (`next_timestamp()` rises only within one process). Anyone
   who can spoof NTP on the LAN can push a responder's clock out of the window of every initiator, or pull it back to widen it for old captures.

## Options considered (the three in the issue)
| Option | Fixes SEC-05 | Fixes SEC-06 | Cost | Verdict |
|---|---|---|---|---|
| (a) responder nonce round trip before Msg1 | yes | no (the clock stays unless the nonce replaces it) | one RTT on every connect; the responder must send bytes to a connection that has not proven it is an admitted initiator, which breaks "zero bytes to a prober" (ADR-068, ADR-072 T3) | rejected |
| (b) persisted per-initiator record on the responder | yes | no | a durable write before every Msg2 and a storage seam Quark does not own; a lost or rolled-back file reopens the hole | rejected |
| (c) persisted monotonic counter on the initiator | no (the responder's record is still in memory) | yes | a storage seam on every initiator and its crash safety | rejected as the whole answer |

None removes the real cause: the responder tries to PREVENT a replayed Msg1 from being accepted, and cannot without state that survives a restart or a clock it
can trust. This ADR makes an accepted replay HARMLESS, and then needs neither.

## Decision
### D1 — Key confirmation: a handshake result is provisional until a frame opens under its key
After the responder accepts a Msg1 (all existing checks: admitted key, role bit, NodeId derivation, decrypt) it derives the transport keys as today, but the
result is PROVISIONAL. While provisional:
- it is not visible to `has_session()`, the router or placement, and no application, schema or rekey frame is sent on it;
- it does NOT replace, supersede or close an existing session or connection for that initiator (ADR-072 item 4 now applies at confirmation, not at Msg1);
- it is keyed by the inbound connection token, not by peer, so two provisional results for one key never collide;
- its lifetime is the lifetime of that inbound connection: the existing pending-inbound timeout of `TcpTransport` (set to 5 s for the pinned wire) kills the
  connection, and `SecureTransport` drops the provisional entry when the transport reports the connection closed (T3). No new timer, and no per-key cap:
  provisional connections stay in `pending_in_`, so the global and per-IP pending caps bound them (see D2 for why a replay cannot be repeated).

**Promotion.** The first frame on that connection that OPENS under the initiator-to-responder key confirms it. At that moment, and not before:
re-run the admission check and `close_hook_` exactly as `install_session_from_result` does today (a revocation between Msg1 and confirmation is honoured),
install the session under the peer, call `identify_inbound(token, peer)` (this is the supersede), send the schema frame. The confirming frame goes through
`accept_recv_seq` like any other, so a duplicate is refused. The initiator sends an empty sealed `Confirm` frame right after it processes Msg2 (zero extra round
trips; the responder waits at most half a round trip before it may send). Any other frame that opens under the key also confirms and IS delivered; only the
empty `Confirm` is consumed. The initiator's session is live as soon as it processes Msg2 (it generated the ephemeral, so it is not exposed to replay).

**Transport changes this needs (rev. 1 omitted them; they are part of this decision):**
- **T1.** `TcpTransport` today allows exactly one frame, the Authenticate opener, on an unidentified inbound connection and kills it on anything else
  (`tcp_transport.hpp` about lines 1057-1066, `wire_codec.hpp` about line 148). After a provisional result exists for the token, the connection may carry
  exactly one further frame kind class: sealed frames from that peer, up to a small bound (default 8 frames, 64 KB) until promotion or close. It must be told
  by `SecureTransport` that a provisional result exists (a token-keyed call), so a connection that never completed an opener still dies on its second frame.
- **T2.** `send_authenticate_frame` routes by NodeId through `conns_`; an unidentified connection is not there, and binding it (`identify_inbound`) is the
  supersede trigger. Add `Transport::send_to_inbound(token, frame)` (default: not supported; `TcpTransport` implements it for a pending inbound connection) and
  use it for Msg2.
- **T3.** `Transport` gets a close notification for an inbound token so `SecureTransport` can erase the provisional entry.
Frames that arrive on a provisional connection before they are opened are NOT routed to the application and NOT counted as session traffic; a frame that fails
to open is dropped and counts against the bound in T1.

### D1r — The rekey path gets the same rule (a pre-existing hole)
A rekey Msg1 arrives as an Authenticate frame with `from` set on an identified connection. Its result is staged in a NEXT slot of the `PeerSession`
(`stage_next`), not installed: sends continue under the current keys, and receive tries current then next. The first frame that opens under next promotes it
(swap, reset counters, drop the old keys). An unconfirmed next slot is dropped after the same 5 s. This makes an injected old Msg1 harmless on a live session.
The initiator's own rekey installs at Msg2 as today (it generated the ephemeral).

### D2 — Msg1 freshness is dedup by ephemeral key, with no clock
The 8-byte timestamp field of the Msg1 payload becomes an opaque 8-byte `nonce` (fresh `randombytes` per Msg1; Msg1 stays 154 bytes). The skew rule and the
strictly-increasing rule are removed; the handshake reads no wall clock (`clock_unix_ns`, `max_clock_skew_ns` and `next_timestamp` are deleted).
In their place the responder keeps, PER ADMITTED INITIATOR KEY, a bounded ring of the last 32 ephemeral public keys `e` it accepted from that key (1 KB per key; the
number of keys is bounded by the application's admitted set). An identical `e` for that key is refused silently (zero bytes), as a replay is refused today.
A ring per key, not one global FIFO: an admitted member that handshakes thousands of times can only evict its own entries, never another initiator's.
Only a Msg1 that passed admission, role and decrypt is inserted, so an unadmitted party cannot fill anything. `e` is bound by the AEAD associated data of the
encrypted static key (`noise.hpp` about lines 387-388), so a mutated `e` fails the tag: dedup by `e` is sound.
`kPinnedVersion` becomes 2; the version is in the Noise prologue, so a v1 and a v2 peer refuse each other silently (zero bytes) instead of a v2 responder timing
out a v1 initiator that never sends `Confirm`. The pinned wire has not shipped (all its ADRs are Proposed), so this is free.

Consequences: SEC-06 holds by construction and NTP spoofing no longer matters to the handshake; a replay inside one responder boot and inside the ring gets zero
bytes; a replay after a restart or after ring eviction gets Msg2 and a provisional entry that D1 makes harmless, **once per captured Msg1 per boot** (the ring
then holds its `e`), which bounds what a stockpile of captures can do to the provisional pool.

### D3 — Controls kept as options (tests only)
`PinnedOptions::key_confirmation = false` restores install-at-Msg1 and in-place rekey; `PinnedOptions::legacy_timestamp_rule = true` restores the timestamp,
skew and monotonic rule with the timestamp in Msg1 and `kPinnedVersion` 1. Both exist only to run the failing control of each claim, like `replay_guard` and
`check_role_bit` today, and are refused outside `QUARK_ALLOW_INSECURE` builds.

## Claims (each proven by a test with a failing control; `tests/adapters/pinned_confirmation_check.cpp`, real TCP + SecureTransport + Noise IK)
- **C1 (SEC-05).** Capture a genuine Msg1, restart the responder, replay it on a new connection: the replayer receives at most Msg2 (58 B) and nothing else for
  6 s; the genuine initiator, connected before or after, delivers 50/50 both ways with no loss; the replayer's connection is closed. CONTROL:
  `key_confirmation = false` makes the replayer's connection the session and the initiator's frames stop arriving.
- **C2 (SEC-06).** An initiator whose clock is set back 10 minutes (injected clock, new process) connects on the first attempt and delivers 50/50. CONTROL:
  `legacy_timestamp_rule = true` refuses it for the whole step-back.
- **C3 (replay in a boot).** The same Msg1 sent twice to a running responder: the second gets zero bytes. CONTROL: dedup disabled accepts it.
- **C4 (no supersede before proof).** A live session for X, then a replay of an old Msg1 of X on a new connection after a responder restart: the live session
  is not closed or displaced and keeps delivering; the replayer's connection never receives an application frame; Msg2 does reach the replayer (T2).
  CONTROL: `key_confirmation = false` displaces it.
- **C5 (genuine reconnect supersedes).** A genuine reconnect supersedes at confirmation, no duplicate, one reconnect counted: `pinned_tcp_check` T6 passes
  with only the adaptations T1-T3 require.
- **C6 (stockpile bound).** 50 DISTINCT captured Msg1s of X replayed after a restart, with a genuine connect raced against them: the genuine one confirms; live
  provisional entries never exceed the pending caps; each captured Msg1 is accepted at most once; memory returns to baseline within the 5 s lifetime.
- **C7 (privacy unchanged).** `pinned_privacy_check` P1-P3 pass unchanged.
- **C8 (rekey injection).** A live pair; clear the responder's ring (or restart it); inject a captured old Msg1 into the live stream as an Authenticate frame
  with `from = X`: the session still delivers 50/50, keys unchanged. CONTROL: `key_confirmation = false` breaks it.
- **C9 (ring is per key).** Capture X's Msg1; admitted member Y performs 4097 handshakes; replay X's Msg1 without a restart: zero bytes. CONTROL: a single
  global ring of 4096 entries (test-only) answers it.
- **C10 (mixed version).** A v1 initiator against a v2 responder and the reverse: both refuse with zero bytes, no half-open connection.
- **C11 (send race).** N threads send immediately after Msg2: the exact application stream arrives 50/50, no loss, no duplicate, whichever frame confirms.
- **C12 (revocation at promotion).** Revoke X between its Msg1 and its confirming frame: no session is installed.
- The H10 limit tests of ADR-068 (restart replay, clock step-back) are replaced by C1 and C2; their old behaviour is kept as the controls.

## Residuals and what this does not do
- After a restart, or after a ring eviction, a replayer holding a captured Msg1 gets one Msg2 (58 B) per capture. This tells it the responder is alive and still
  admits that key (a membership oracle for a key whose Msg1 it already holds; wider than before because the captured Msg1 no longer expires). It was already true
  under ADR-068's restart residual.
- A stockpile of M captured Msg1s can occupy provisional slots for at most 5 s each, once each per boot: denial bounded by M and by the pending caps.
- Removing the clock removes the only bound on how long a captured Msg1 stays usable; it is now bounded by the ring and by D1. That is the intended trade and
  the reason D1 and D1r are the load-bearing part.
- The responder cannot send first on a new connection: it waits for the `Confirm` (half a round trip).
- A man in the middle who forwards a genuine Msg1, Msg2 and the genuine `Confirm` unchanged is a relay, as before.
- Not decided here: option (c) stays available if a later requirement needs the responder to REFUSE (not neutralise) a cross-restart replay.
- Severity note for sequencing: the harm D1 removes is availability only (no key or plaintext is exposed), and the transport changes T1-T3 are not small. The
  owner may defer the implementation; D2 must NOT ship without D1 and D1r.

## What changes
`include/quark/adapters/sodium/pinned_handshake.hpp` (Msg1 field, per-key ring replaces `PinnedReplayGuard`, options, delete the clock rules, `kPinnedVersion`),
`include/quark/adapters/sodium/noise.hpp` (expose the received `e`), `include/quark/core/secure_transport.hpp` (provisional store by token, promotion, `Confirm`
frame, staged next slot on rekey), `include/quark/core/peer_session.hpp` (`stage_next`, receive tries current then next, promotion), `include/quark/core/transport.hpp`
(`send_to_inbound`, inbound close notification), `include/quark/net/tcp_transport.hpp` and `include/quark/net/wire_codec.hpp` (T1: bounded sealed frames on a
connection with a provisional result; T2; T3; the 5 s pending-inbound timeout for the pinned wire), `tests/adapters/` (the new check; `pinned_tcp_check` T6 and the
H10 tests adapted). ADR-068 section 7 and ADR-072 item 4 get a pointer here. No change to `cluster.hpp`, SWIM, relay or the mbedTLS path (D1r changes
`PeerSession`, which the mbedTLS path shares: the staged slot is opt-in per engine, so mbedTLS renegotiation keeps its in-place swap).

## Red-team round 1 (2026-11-02, independent, read the code) and what became of it
| # | Sev | Finding | Disposition |
|---|---|---|---|
| 1 | P0 | The confirmation frame cannot reach the responder: an unidentified inbound connection allows exactly one opener frame and is killed on any other | **Accepted**, T1 |
| 2 | P0 | Msg2 has no route on a provisional connection (it routes by NodeId; binding is the supersede) | **Accepted**, T2 |
| 3 | P1 | The rekey path installs in place at once; an injected old Msg1 replaces a live session's keys after a restart or eviction | **Accepted**, D1r, C8 |
| 4 | P1 | One global 4096-entry FIFO is emptied by any admitted member's handshakes | **Accepted**, ring per key, C9 |
| 5 | P1 | Per-key cap with oldest-drop lets stockpiled replays evict the genuine provisional entry | **Accepted**: no per-key cap; each capture is single-use per boot (D2); C6 uses distinct captures |
| 6 | P2 | `confirm_timeout` has no timer; provisional connections hold pending slots | **Accepted**: lifetime is the connection's, killed by the pending-inbound timeout, T3 |
| 7 | P2 | No version bump: mixed v1/v2 half-opens | **Accepted**, `kPinnedVersion` 2, C10 |
| 8 | P2 | Promotion must re-check admission and `close_hook_`; confirm goes through `accept_recv_seq`; first frame may be application data | **Accepted**, D1 text, C11, C12 |
| 9 | P3 | Msg2 on replay is a membership oracle, now unbounded in age | **Accepted as residual** |
Claims C1, C4, C5, C6 as written in rev. 1 would have failed; C2, C3 and C7 held. Design points the red-team tried to break and could not: a mutated `e` (the AEAD
associated data covers it), nonce or key reuse from the confirmation frame (directional keys, fresh per handshake, distinct nonce per direction), a replayer opening
the confirmation, the schema frame and `has_session` before confirmation, and the rejection of option (a).

## Evidence required before Accepted
A second red-team round on rev. 2. C1-C12 green under Release, ASan+UBSan and TSan on Linux (GCC and Clang). 20 runs of `pinned_tcp_check` and
`pinned_flood_check` with 0 failures. `pinned_privacy_check` unchanged.
