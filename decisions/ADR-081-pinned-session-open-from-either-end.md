# ADR-081 — Either end of an admitted pinned pair may open the session (#83)

Status: **Proposed**. Follows ADR-040 (handshake driving), ADR-068/072 (pinned handshake, pinned wire),
ADR-074 D3 / #63 (role rules: the client initiates against a member, otherwise the lower id).

## Problem

`SecureTransport::ensure_handshake` returns immediately unless `local_is_client(self, peer)` is true
(`secure_transport.hpp:832`), and the pinned anonymous-opener path refuses an opener that the role rule does not
make the initiator (`:896`). So the higher-id end of a two-member pinned pair — which holds the peer's key and
address — cannot bring the pair up, although the lower-id end can. The same asymmetry applies to a member that
wants to open toward a client.

The two checks exist for good reasons and must not simply be removed:

- glare-freedom: both ends derive the same initiator from the ids with no wire negotiation;
- ADR-074 D3: a member must never initiate toward a client (and the pinned client's id has bit 63 set, so the old
  "lower id initiates" rule refused every client — #63).

## Decision

Add a **role-agnostic session request** that is not a handshake message and carries no identity:

- `SecureTransport::request_session(peer)`: if `local_is_client(self_, peer)` it is exactly today's
  `ensure_handshake`; otherwise it asks the inner transport to open a **bare connection** to `peer` (no frame).
- `Transport::request_session(peer)` (virtual, default no-op) opens the connection. `TcpTransport` dials the
  configured endpoint; the pinned transport dials its configured peer. For a transport that already has a live
  connection this is a no-op.
- When the *accepting* transport has an inbound connection and this end is the designated initiator, it now
  initiates: the pinned listener invokes a `connect_hook`, and `SecureTransport` installs that hook as
  "call `ensure_handshake(peer)`". The peer's id comes from the pinned configuration (single-peer link); the
  connection itself is unauthenticated until Msg1, exactly as a normal dial is.

The responder-answers-nothing rule is unchanged: the accepting side still sends nothing to an unknown opener
until it has decided to initiate, and what it then sends is Msg1 — the same bytes it already sends when it dials
itself. A bare connection reveals only address and timing to a host that already had to reach the listener.

## Glare

If both ends request at the same time, each may dial and each may also accept; the existing pinned session
install and the transport's per-peer dedup pick one live connection, and a simultaneous second handshake is a
renegotiation (ADR-040 S4), not a second session. The test asserts one session and one live connection.

## Test

`tests/adapters/pinned_session_open_check.cpp` (sodium build): a two-member pinned pair whose **higher** id calls
`send()` first must form a session and exchange frames both ways (this is the #83 reproduction). Controls:
- removing the bare connection (today's code) reproduces "never connects" — the test times out instead of passing;
- an unadmitted connector produces no bytes from the accepting side and no session (responder-answers-nothing);
- simultaneous requests from both ends yield exactly one session.
