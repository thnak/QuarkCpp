# ADR-080 — Portable device client library for non-member sessions (#37 gap 4, #39)

Status: **Proposed**. Consumer: AeroCoWorker device attach (ADR-017) and the solo-user relay. Follows ADR-040 (mTLS),
ADR-068/072 (pinned key, pinned wire) and ADR-074 D3 (client role).

## Problem

#37 established that a device must attach to one home node with the same authenticated identity but without joining
SWIM: no membership entry, no gossip, no placement. ADR-074 D3 added the client role (an identity CN / pinned-key role
whose NodeId has bit 63 set) and made the client side of a member/client pair the TLS client, so the device always
initiates. What is left is gap 4: how the device actually speaks the wire.

Reimplementing the handshake in the application would duplicate ADR-040/068 (TLS-in-Authenticate, exporter keys, AEAD,
frames) — the very thing #37 says must not happen. The alternative, a written wire spec only, leaves that duplication
with the application.

## Decision

Ship **`quark::client`**, a small, portable, blocking client library. It reuses the existing protocol code and adds no
new wire format:

1. **`client::ByteStream`** — a three-method seam (`read`/`write`/`close`) with blocking semantics. The application
   supplies the socket for its platform. The library never starts a PAL `IoContext`/reactor, so it runs on Android/iOS
   without a PAL backend. A PAL-backed desktop implementation is a follow-up, behind the same seam.
2. **`client::PinnedClientTransport`** — a single-peer `Transport` over a `ByteStream`. It implements exactly the
   ADR-072 pinned wire from the dialling side: no HELLO; a frame's `from`/`to` are zeroed on the wire and restored
   from the connection's fixed peer on receipt (the AEAD AAD still binds the real ids); the first frame is an
   `Authenticate` opener. There is no claimed identity on the wire, so the transport cannot deliver a frame from any
   other peer.
3. **Composition, not a new stack.** The application wraps this transport in `SecureTransport` with a
   `PinnedHandshakeEngineFactory` (or its own `HandshakeEngineFactory`) and a client-role `NodeId`. The first
   `SecureTransport::send()` opens the Noise IK handshake; inbound frames are delivered to the same `SecureTransport`
   by the reader thread. ADR-074 D3's `local_is_client` makes the client initiate regardless of NodeId order.
4. **Non-goals (v1).** No SWIM, gossip, placement or capability advertisement (that is the point). No relay-fallback
   (ADR-048 probes use the HELLO, which the pinned wire does not carry). No reconnect/backoff and no resume-from-seq:
   the application owns its event log and redials/replays. No mTLS+HELLO device path in v1 (the client role exists in
   `SecureTransport`; a HELLO-capable client transport is a later slice). No in-library socket.

## Security properties

- **No identity before admission.** The transport sends no HELLO and no NodeId; every byte before the handshake is
  part of the single `Authenticate` opener, exactly as ADR-072 requires.
- **Peer fixed by construction.** The client dials one endpoint and attributes every inbound frame to that configured
  peer. There is no `from` field to spoof: an on-path frame is still authenticated by the session AEAD before delivery.
- **Client role.** The device's NodeId has bit 63 set; `SecureTransport` therefore never initiates as a member/server
  and never accepts a member's renegotiation. The member side already refuses client ids in SWIM and relay (ADR-074).
- **Revocation/admission.** The pinned engine checks the key at every handshake; a denied or revoked device fails the
  handshake and the application closes the byte stream. Open-session revocation is ADR-074 D3's active close.

## Proof plan

- S1 (this slice): `ByteStream` + `PinnedClientTransport` over an in-memory blocking stream pair; two-way delivery
  with ids restored, wire bytes carry `from == to == 0`, a malformed frame closes the transport.
- S2: compose with `SecureTransport` + a pinned factory over the same seam and complete a client-role handshake and
  data exchange against a `TcpTransport`-backed member on loopback (both real).
- S3: a PAL-backed `SocketByteStream` (desktop/`linux_x86_64`, `windows_x86_64`, `windows_arm64`), reconnect/resume
  hooks, and the mTLS+HELLO client path if AeroCoWorker needs it.

## Residuals

- v1 is pinned-wire only; an mTLS device that must speak HELLO is out of scope until S3.
- The client does not rotate keys; pinned mode has no certificate rotation. A long-lived device re-handshakes on
  reconnect (the application's policy).
- The reader thread is one per client transport (one device connection); no shared reactor, by design.
