# ADR-080 — Portable device client library for non-member sessions (#37 gap 4, #39)

Status: **Proposed**; slices S1, S2, S3a and S3b implemented and tested (S3c mTLS+HELLO conditional). Consumer:
AeroCoWorker device attach (ADR-017) and the solo-user relay. Follows ADR-040 (mTLS), ADR-068/072 (pinned key, pinned
wire) and ADR-074 D3 (client role).

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

- **S1 — done** (`tests/client_pinned_transport_test`): `ByteStream` + `PinnedClientTransport` over an in-memory
  blocking stream pair; two-way delivery with ids restored, wire bytes carry `from == to == 0`, a malformed frame
  closes the transport.
- **S2 — done** (`tests/adapters/pinned_client_library_check`, sodium builds): a client-role device composed with
  `SecureTransport` + the pinned engine against a real `TcpTransport` + `SecureTransport` member on loopback; the
  handshake completes, data flows both ways, and an un-admitted client-role device gets no session (control).
  Mutation control: removing the inbound id restoration makes the handshake fail, so the test exercises it.
- **S3a — done**: `include/quark/client/socket_byte_stream.hpp`, a real blocking socket through the PAL
  (`tcp_connect_blocking`, `set_blocking`, `shutdown_socket` added to both backends). `close()` shuts the socket
  down before closing it, so the transport's blocked reader wakes; `pinned_client_library_check` now uses it.
- **S3b — done (recipe, tested)**: reconnect/resume stays with the application by design. The check drops the dead
  stream/transport/SecureTransport and builds fresh ones, completes a new handshake and exchanges frames again;
  the app replays from its own log/sequence number. No hidden reconnect state in the library.
- S3c — **conditional, not implemented**: the mTLS+HELLO client path, only if AeroCoWorker's devices need mTLS
  rather than pinned keys (its ADR-044 Q1 uses pinned keys today).

## Residuals

- v1 is pinned-wire only; an mTLS device that must speak HELLO is out of scope until S3.
- The client does not rotate keys; pinned mode has no certificate rotation. A long-lived device re-handshakes on
  reconnect (the application's policy).
- The reader thread is one per client transport (one device connection); no shared reactor, by design.
- `SocketByteStream` is desktop-only by construction (PAL backends exist for Linux and Windows x86-64/arm64, ADR-059
  arm64 is cross-compiled only). A phone app supplies its own blocking socket behind `ByteStream`, as designed.
- S3c: a device that must speak the HELLO wire (mTLS) needs a HELLO-aware client transport; not implemented because
  AeroCoWorker's devices use pinned keys.
