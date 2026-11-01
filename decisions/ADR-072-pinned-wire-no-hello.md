# ADR-072: Pinned wire — no HELLO, identity from the handshake, no NodeId in frame headers (#45, #46)

Status: Proposed. Follows ADR-068 (Noise IK engine). Consumer: AeroCoWorker RFC 002 §6e (B2), ADR-044 §4.6 Q1; unblocks dialing a
LAN-discovered endpoint by address (ADR-070 F3, #42).

## Problem
ADR-068 delivered the engine but not "no identity before admission" on the shipped transports: `TcpTransport` sent a cleartext `QRK1` HELLO with
the NodeId, `SecureTransport` put `from`/`to` in every frame header and took the responder's expected peer from the claimed `from`, and a HELLO
claim could displace a live connection (#46, N1).

## Decision
`TcpTransport::set_pinned_wire(true)` (cold setup) selects the pinned wire:
1. **No HELLO either way.** A dialler knows who it dialled and confirms at connect. An accepted connection is anonymous: it may send exactly ONE
   `Authenticate` frame (<= 1024 B, `from == to == 0`); anything else closes it silently (`anonymous_frames_refused()`).
2. **Identified by the handshake.** `SecureTransport` runs the responder engine with no expected peer; only after Msg1 authenticates and the key is
   admitted does it install a session under the AUTHENTICATED NodeId, call `Transport::identify_inbound(token, peer)`, then send Msg2 on the bound
   connection. A failure sends nothing and calls `reject_inbound(token)` (close). New seam: `MessageFrame::inbound_token`, two defaulted virtuals.
3. **No ids in headers.** A direct frame (`from == self`, `to == connection peer`) goes out with both ids 0 and has them restored from the
   connection's identity on receipt (so the AEAD AAD, which binds the real ids, is unchanged). Relay frames keep explicit ids (shape B, ADR-048).
4. **Authenticated supersede (#46 HELLO half).** A connection identified by the handshake always replaces the one held for that peer (a phone on a
   new network), without firing peer-down for the session the same handshake just swapped in place. The address binding and the id election apply only to
   unauthenticated HELLO claims, which no longer exist on this wire. A HELLO sent to a pinned listener is garbage and is refused with zero bytes.

## Proof (`tests/adapters/pinned_tcp_check.cpp`, real sockets, recording proxy; each with a control)
T1 two-way delivery, b never dials. T2 capture: no NodeId, key or cluster id anywhere on the wire (CONTROL, HELLO wire: both NodeIds visible).
T3 a silent prober, random bytes, a wrong-key Msg1 and a replayed HELLO all get ZERO bytes (CONTROL: the HELLO wire sends 14 bytes unprompted).
T4 an un-admitted initiator gets no session and no byte back (CONTROL: admitted, it is served). T5 a HELLO claiming a's NodeId does not displace a's
live connection (CONTROL: on the HELLO wire it does). T6 an authenticated reconnect resumes delivery with one session.

## Residuals
- Frame headers still carry `target` (actor type/key), `msg_type`, `principal` and `deadline` in the clear (AAD-bound, not sealed). Sealing them is a
  further step and changes routing; not done here.
- Anonymous handshake work is bounded by TcpTransport's pending-inbound caps (per IP and total) and the pending-HELLO timeout, not by a NodeId.
- Relay fallback probes (ADR-048) use the HELLO and are incompatible with the pinned wire.
- Linux: GCC 15 + ASan/UBSan pass for the #48 tests; `pinned_tcp_check` was run on MSVC only (no libsodium on the Linux box); TSan not run.
