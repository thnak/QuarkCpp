# ADR-069 — Application-driven admission snapshots, eviction and authority-only revocation (020 section 1a, 021)

Status: **Proposed** (2026-10-31). Not Accepted: it is implemented and tested on the Windows/MSVC build only; the sanitizer
runs (ASan/UBSan, TSan) and the GCC/Clang builds the repo's standard calls for are still owed (section 7). Issue #41
(AeroCoWorker ADR-044 Q2). Consumer references: AeroCoWorker RFC 002 section 6e item 4 and gate G10 (b); ADR-044 rules L6b
("close every session with a removed device at once") and L6c ("never open one, relay to it or serve it again").
Spec first: 020-Security section 1a and 021 "Application-driven eviction" are **Draft amendments** written before the code.

## 1. Problem

Admission is pluggable (`NodeAuthority::admitted(NodeId)`) but only as a static check; three gaps (issue #41):

1. nothing lets the application **evict** a connected node: close its sessions and drop it from membership at once;
2. revocation is gossiped fingerprints that **any admitted member can issue against a third party** (`revocation_registry.hpp`
   header; REVIEW-BACKLOG 102-103), so one compromised member could evict everyone;
3. nothing says "only my authority decides, never peer-issued revocation".

The existing code was mapped before designing (verified, not assumed):

- `SecureTransport` admits by handshake only. `set_handshake_admission` is a pre-authentication gate on the **claimed** NodeId; after a
  handshake completes nothing re-asks anyone. A live session stays usable until `sweep_revocations` matches a **fingerprint**.
- `SecureTransport::deliver` calls the receiver synchronously. **There is no inbound queue in Quark above the socket** (TcpTransport
  reads and calls the receiver on its loop thread; DistributedRouter then posts into actor mailboxes). So "queued from a denied
  session" can only mean frames received by the inner transport but not yet handed to `deliver`.
- `TcpTransport::set_relay_forwarding(const NodeAuthority&)` already consults `authority.admitted(origin) && admitted(dest)` per transit
  frame. That is a relay-authorisation hook; nothing new has to be invented there.
- Gossip revocation reaches `RevocationRegistry::merge` through `SwimMembership::revocation_merge_`; nothing in that path consults any
  authority. Membership (`SwimMembership`) is single-driving-thread; only a few members are thread-safe.

## 2. Decision

1. **`AdmissionSnapshot`** (`include/quark/core/admission.hpp`): immutable, generation-numbered, sorted storage, tri-state
   `Admit | LogExchangeOnly | Deny`, closed world (absent key is Deny). `lookup` is `noexcept`, lock-free, allocation-free.
2. **`AdmissionAuthority`**: holds the current snapshot behind `std::atomic<std::shared_ptr<const AdmissionSnapshot>>` (the repo idiom,
   as `revoked_snapshot_`). `publish(next)` swaps, then runs listeners synchronously; refuses null, an equal-or-older generation, and a
   call from inside a listener. It **is a `NodeAuthority`** (`admitted` is true only for Admit), so `place_admitted` and the relay hook
   work unchanged and every generation applies to them with no wiring.
3. **`SecureTransport::set_admission`** (`secure_transport_admission.hpp` holds the gate): every inbound frame, outbound frame and
   handshake completion asks the *current* snapshot (nothing cached across generations). A generation fence (shared lock around each
   operation that acts on an answer; `publish`'s listener takes the exclusive lock as an empty barrier after the swap) makes
   "publish has returned" imply "no operation that read an older generation is still running". On publish the transport aborts in-flight
   handshakes of denied peers, closes denied live sessions (erase + close hook) and re-stamps the rest. Delivered frames carry
   `MessageFrame::admission_generation` (overwritten on receipt, never taken from the wire).
4. **`SwimMembership::set_admission`**: a published generation sets a thread-safe flag; the driving thread tombstones every live member
   the snapshot denies, emits one **`Evicted`** event (new `MembershipEventKind`, distinct from Dead/Left), and `apply_update` refuses
   any Alive/Suspect update for a denied node (gossip, ack, join) until a later snapshot admits it. Eviction is local and **not gossiped**.
5. **Authority-only revocation**: `SwimMembership::Config::authority_only_revocation` (gossiped fingerprints counted and dropped, none
   offered) and `RevocationRegistry::set_authority_only` (merge ignored, `sample` empty). Default off = ADR-040 behaviour (positive control).
6. **Identity.** Keyed by `AdmissionKey`, a `using` alias of `NodeId`. Q1 (issue #40) will make the identity an Ed25519 public key with
   NodeId derived from it; this ADR does **not** depend on Q1's code. The alias is the single change point (`Entry` stores 64 bits today).

## 3. Claims, each with the test that breaks it

Tests are in `tests/admission_*_test.cpp`. "Control" = the same test with exactly one guarantee removed through
`AdmissionTestFaults` (or the default mode), asserting the OLD behaviour is observed. Results are in section 5.

| # | Claim | Test that breaks it | Control (guarantee removed) |
|---|---|---|---|
| C1 | `lookup` is a pure read of an immutable snapshot; an older snapshot already handed out is unchanged by a later publish; absent key is Deny | `admission_secure_transport_test` T0 | none needed (structural) |
| C2 | Generations are strictly increasing; equal/older/null/reentrant publishes are refused and counted, so a replay cannot re-admit | T0 | n/a |
| C3 | (b) By the time `publish(g)` returns, a live session of a now-denied peer is closed (session count 0, close hook called); its reconnect is refused; admitting it in a later generation reconnects | T1 | `skip_live_sweep`: session survives publish, no close |
| C4 | (a) A generation published between a handshake's answer and its completion stops the session; the denied peer gets no ServerHello | T2 (hook publishes inside the server engine's final `advance`) | `skip_handshake_recheck`: the session installs under the stale answer |
| C5 | (c) N frames queued inbound at the moment of denial: 0 delivered, discard counter == N exactly | T3 (N = 7) | `skip_inbound_gate` + `skip_live_sweep`: all N delivered |
| C6 | (d) Every delivered frame carries the generation it was admitted under and the peer identity; a sender cannot forge the stamp | T4 | `skip_inbound_gate`: stamp is 0 |
| C7 | LogExchangeOnly: only the control lane passes; data lane refused and counted; next generation admitting the peer lifts it; a log-only peer can still handshake; the session is not closed | T5 | `skip_inbound_gate`: data lane not restricted |
| C8 | Concurrent `publish` vs lookups vs live delivery: no frame delivered under a non-admitting generation, none delivered after `publish(g)` returned with an older generation, delivered + discarded == sent, generation monotonic | `admission_concurrency_test` (stress, 3 senders + publisher + reader) | `skip_inbound_gate`: P1 violations == every frame |
| C9 | Authority-only mode ignores a gossiped third-party revocation by an ordinary member (and does not re-gossip it); the default applies it | `admission_membership_eviction_test` T6 | default mode: revocation reaches the peer |
| C10 | The snapshot still denies and evicts in authority-only mode | T6 (last block) | n/a |
| C11 | Eviction emits exactly one `Evicted` event on every observing node, removes the node from view and placement, refuses re-admission by gossip/ack/join until a later snapshot admits it, then a fresh `Joined` | T7 | a generation that still admits the node evicts nothing |
| C12 | No relay path forwards a frame to or from a node the snapshot denies or restricts to log exchange; re-admission restores it | `admission_relay_test` (real sockets) | both admitted: 10 frames forwarded |

## 4. Deviations and findings (the existing code against the contract)

- **No inbound queue exists above the socket**, so guarantee (c) is implemented as a per-frame gate on arrival (a frame from a denied
  peer is discarded and counted whenever it reaches `deliver`, before or after the publish) plus the fence for frames already in
  flight. The counter increments when a queued frame reaches the transport, not at the instant `publish` returns. A frame already inside
  the application's receiver callback when it is published cannot be recalled; the fence guarantees it finishes before `publish`
  returns. Frames the receiver already posted into an actor mailbox are the application's and are outside this guarantee.
- **The pre-authentication handshake gate (`set_handshake_admission`) answers on a claimed NodeId.** The new check at completion is on
  the **authenticated** identity (`Result::peer_node_id`), which is the one that matters. The claimed-id check only refuses early.
- **Revocation gossip bypassed any authority** (`revocation_merge_` called straight into the registry). In default mode it still does
  (positive control); authority-only closes it at two layers.
- **A relay hook already existed** (`set_relay_forwarding(const NodeAuthority&)`); consulting the snapshot there needed no new code.
  There is no per-session relay state to purge, so "no relay forwards to or from an evicted node" is the per-frame check.
- **Eviction is not gossiped** by design (a node trusts only its own snapshot); a third node that is not told keeps the member.

## 5. Evidence

MSVC (VS 2026), Release, `build-q2`: the four `admission_*` test programs pass and each control observes the old behaviour (the full `ctest`
count is recorded in the implementation report). Not yet run: sanitizers, GCC/Clang (section 7).

## 6. Residual risks

- `publish` is synchronous and blocks behind a receive callback that blocks; it must not be called from a receive callback of a
  transport listening to the same authority (same-thread re-entry is detected and skips the barrier, counted as
  `reentrant_publishes`; a cross-thread cycle would deadlock). The AeroCoWorker design posts out of the callback.
- Two transports on one thread, each with its own authority, publishing concurrently through synchronous loopback can lock-order
  invert on their fences. That is a test-double shape (a real network separates the threads).
- With the handshake disabled (shared-cipher shape) sessions are not erased on deny (recreating one would restart its sequence spaces,
  nonce reuse, B12); the per-frame gate is the closure there.
- `close_hook` for TcpTransport: **superseded by section 8 (#47, N3).** `reset_peer_connection` redials the removed device; the recommended
  wiring is now `TcpTransport::set_admission(authority)` (refuse at HELLO, close, never redial, lifted by a later admitting snapshot) with the
  session layer's close hook empty. `close_peer` still suppresses reconnect with no way to lift it.
- `LogExchangeOnly` peers stay in membership and gossip over the control lane; they are not placement-eligible (not Admit).

- **Lifetime.** The authority must outlive every listener owner (they unregister in their destructors); a test that declared it after
  the transport hung in Debug on destruction. Documented in `admission.hpp`; the tests declare it first.

## 7. Owed

1. TSan (`-DQUARK_SANITIZE=thread`, `-j1`) and ASan/UBSan runs of the four `admission_*` test programs on Linux (this machine's toolchain is MSVC).
2. GCC 14 / Clang 22 compile of the new headers (`std::atomic<std::shared_ptr>`, designated initialisers on `AdmissionTestFaults`).
3. Wire-up demonstration over real mTLS (mbedTLS) rather than the mock handshake; the mock proves the state machine only.
4. When Q1 lands: change `AdmissionKey`, re-run T0-T8; the handshake completion already asks on `Result::peer_node_id`.
5. A bench: the fence adds one shared-lock acquire per frame on the secure path (not the actor hot path); not yet measured.
6. Signed, issuer-authenticated revocation (issue #35) is unchanged by this ADR; authority-only mode is the interim answer.

## 8. Red-team of the merged changes (#47), 2026-11-01

Status stays **Proposed**. Findings against this ADR and their fixes (the lease findings M2/M3/N2 are in ADR-071 section 10):

| Finding | Defect | Fix | Test (claim) | Control, observed |
|---|---|---|---|---|
| **M1** (major; also closes **N5**) | `TcpTransport` never compared `frame.from` with the connection's HELLO identity and `SecureTransport` erased a session when a handshake it only ANSWERED failed. Admission is asked about the claimed `from`, so a DENIED host (HELLO id 99) sending `Authenticate{from=1}` at 10/s cut 1->2 delivery from 100% to 9% | (a) `TcpTransport` drops and counts (`spoofed_origin_dropped()`) any locally delivered frame whose `from` differs from the connection's identified peer; a shape-B relay delivers its origin's frames only where the receiver opted in (`set_accept_relayed_frames`, default none; relayed frames are end-to-end sealed, ADR-048). (b) `SecureTransport` erases a session on a failed handshake only if THIS side initiated it (`PendingHandshake::initiated_here`) | `tcp_frame_origin_test` (C13, C14): no attacker 100%; attack with both fixes 100%, 0 samples without a session, 30 spoofed frames dropped; each fix alone also holds 100% (defence in depth); relay: 0/5 delivered and 5 counted without opt-in, 5/5 with it | both fixes off: **8%** delivery, session missing in 269/300 samples (red-team: 9%) |
| **N3** | the close hook this ADR recommended (`reset_peer_connection`) redialled the removed device: after a deny node 2 still held or reopened a connection to it | `TcpTransport::set_admission(authority)`: a denied peer is refused at HELLO, never dialled or queued for, its connection closed on the publish (peer-down reported so the session drops), reconnect not scheduled, and a later snapshot that does not deny it lifts all of that and dials it if its endpoint is known | `tcp_admission_redial_test` (C15): 3 s after a deny, 0/300 samples with a connection open, 0 redials, 0 frames delivered, 8 dials refused (two bare `TcpTransport`s); a later admitting snapshot restores traffic | the old close path (`reset_peer_connection`, no transport admission): connection open in 300/300 samples |
| (composition) | SWIM + SecureTransport + TcpTransport on one authority with a lease and a mid-run deny was untested | `admission_mobile_composition_test` (C16), see ADR-071 section 10 | eviction, session closed, no redial, no `Resumed`; re-admission lets the node back | old wiring: lease kept, connection open 130/150 |

**Why the origin check lives in the transport, not above it.** Admission and the handshake gate both read the claimed `from`; the transport is the
only layer that knows which socket delivered the frame. The relay opt-in exists because a shape-B relay (ADR-048) forwards a frame byte-for-byte
with the origin's `from`; without the opt-in every relayed frame would be dropped (four of the five relay test programs, all but the sealed-relay one, were updated to opt in at the
destination).

**Found while proving N3 (test double, not fixed).** With the mock handshake, a SECOND stale ClientHello queued behind a dead connection (the
5 s handshake timeout re-issues one) is answered twice, and the peer, holding no pending engine, treats the other's reply as an opener: an
unbounded renegotiation ping-pong (300 000+ completed renegotiations in 10 s, every frame `tamper_rejected`, a core pegged; 5 of 24 runs of
the first version of the N3 test, which ran over `SecureTransport`, and still 1 of 11 with a 1.5 s window). Not reachable with the Noise engine as far as read (a stray message fails to parse, and with the M1 fix an
answered failure no longer erases the session), but NOT verified with Noise. The root cause is that handshake frames queued per peer outlive the
connection they were meant for; purging queued `Authenticate` frames when their connection dies is the real fix and is not done. The N3 test
therefore runs over bare `TcpTransport`s (the session layer is covered once, in the composition test, which passed every run); the
reflection itself is not tested and its exact trigger was not pinned down (`handshakes_timed_out` was 0, so it is not the 5 s re-issue).

**Residuals (not fixed here).** *(Update 2026-11-01: N1 is closed on the pinned wire by ADR-072, test T5; it remains only on the HELLO wire.)* **N1**: a HELLO claim still displaces a live connection from the same source IP (DoS only; payloads stay sealed
under `SecureTransport`); closed only by the pinned accept path (#45, ADR-068 follow-up 1). #46's frame-identity concern is closed by the origin
check, its HELLO-claim concern is not. `TcpTransport::set_admission` answers on the CLAIMED HELLO id; the authenticated check remains the handshake's
(`Result::peer_node_id`). A denied peer that is only a relay target is not refused as a dial target by shape-B routing through another node.

## 9. Renegotiation ping-pong closed (#48) and unauthenticated failures (#46, session half), 2026-11-01

Root cause of the ping-pong (reproduced deterministically in `security_secure_transport_renegotiation_loop_test`, no timing): a client-role
node (lower NodeId) with NO engine in flight treated an inbound `Authenticate` frame as an OPENER and answered it as a server. Under the mock
handshake a ServerHello has the same shape as a ClientHello, so two nodes bounced handshakes indefinitely (20 000 completed renegotiations in
20 000 frames). Fixes, each sufficient alone (test T3):

1. **Role rule.** Only the higher NodeId is ever a server. A client-role node with no engine in flight drops an inbound Authenticate frame and
   counts it (`unexpected_authenticate()`). This matches the glare-free rule already stated in handshake.hpp.
2. **Bound.** A server-role node answers an opener for an already-live session at most once per `set_min_renegotiate_interval` (default 250 ms;
   `renegotiations_throttled()`); a failed forgery does not move the clock, only a completed handshake does.
3. **Purge.** `TcpTransport` drops the queued `Authenticate` frames of a peer when its established connection dies
   (`stale_authenticate_purged()`): a handshake frame is bound to the connection it was meant for. (Verified through the SecureTransport tests
   and the 48-test regression subset; the TCP purge itself has no isolated deterministic test, the queue is private and a connection that dies
   with frames still queued cannot be produced without a stalled peer.)

#46 (session half): a failure produced by bytes taken from an inbound Authenticate frame never erases a session, even for a renegotiation this
side initiated (`inbound_auth_failures_ignored()`); only a failure of this side's own step (empty input) does. Control fault
`erase_session_on_inbound_failure` restores the old behaviour: the session is erased (T4). Test-side consequence: tests that flooded a LOWER
NodeId with forged openers now flood a higher one; rotation tests that rekey immediately set the bound to 0.
