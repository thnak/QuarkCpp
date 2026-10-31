# 020 — Security

**Cross-cutting resolution.** A single-process actor engine has almost no attack
surface; a **distributed** one has a large one, because placement (010) means
*any node may host any actor and its state*, messages cross an untrusted network,
and durable state sits on disk. Security therefore touches the transport (010),
membership (010), the message context (004), the send verbs (006), persistence
(012), configuration/secrets (013), the PAL (019), and observability (009). This
spec defines the trust model and the seams that enforce it; like every other
subsystem, the mechanisms are **seams with a self-contained default**, and the
one place that principle bends — real cryptography — is called out explicitly.

## The core principle: security is a boundary concern

> An in-process `tell`/`ask` between two local actors crosses no trust boundary
> and pays **nothing** — no encryption, no authz check, no copy. Security cost is
> incurred only where a message or byte crosses a boundary: **node↔node** (010)
> and **ingress** (an external client entering the cluster).

This mirrors the rest of the engine: a single-node deployment that configures no
cluster and no ingress compiles none of this in. Security is a layer over the
distribution seams, not a tax on the hot path.

## Threat model and trust boundaries

Stating what is defended is as important as the mechanism. The default posture is
**"trusted cluster, hostile network and disk"** — the standard datacenter model.

| Boundary | Threat | Default defense |
|---|---|---|
| **Node ↔ node** link | Eavesdrop, tamper, replay, inject | Mutually-authenticated, encrypted transport (below) |
| **Cluster admission** (010 SWIM) | Rogue node joins, receives re-placed actors + state | Node identity + join authorization; unauthenticated peers cannot gossip or be placed onto |
| **Ingress** (external client → actor) | Unauthenticated/unauthorized message to an actor | Principal authentication at the edge + per-actor authorization (below) |
| **Data at rest** (012 WAL/snapshots) | Disk theft, backup exfiltration | Optional envelope encryption via the store/keyring seam |
| **Secrets** (013 credentials, keys) | Plaintext in config/logs/core dumps | `SecretSource` seam; never in `EngineConfig` literals, never logged (009) |
| **Insider actor** (a compromised actor type) | Sends messages it shouldn't | Authorization is per (principal, target, message), not per-node — an authenticated node is not automatically authorized for every actor |

**Explicit non-goals of the default model** (see Non-goals): defending one
co-located actor from another *in the same process* (they share an address space —
that is a language/OS memory-safety concern, not this engine's), side-channel /
timing-attack hardening of handler code, and DoS/rate-limiting (an availability
concern, specced separately in
[022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md)).

## Layers

Security is not one feature; it is five independent layers, each its own seam so a
deployment adopts exactly what its threat model needs:

```
  ingress client ─┐
                  ▼
        [ 3. Authorization ]  principal → may (target, message)?     ← per message
  node ═══[ 2. Transport security ]═══ node    mTLS-class, per connection
              │
        [ 1. Node identity + admission ]  who may join the cluster    ← per node
              │
        [ 4. Secrets ]  keys/creds feeding 1–2 and 5   [ 5. At-rest ]  disk (012)
```

## 1. Node identity and cluster admission

Every node has a **cryptographic identity** (a key pair; the public half is its
`NodeId` or is bound to it). SWIM membership (010) is extended so that:

- A joining node must complete a **mutual authentication handshake** (layer 2)
  before it can gossip membership or be selected by HRW placement. An
  unauthenticated peer is invisible to placement — it can never be assigned an
  actor, so it can never pull actor state.
- Admission is governed by a **trust anchor**: the default is a shared cluster CA
  (a node is trusted iff its certificate chains to the cluster root) or a
  pre-shared cluster key; a `NodeAuthority` seam lets deployments substitute SPIFFE
  IDs, a PKI, or an allowlist. **How that anchor is provisioned and bootstrapped**
  (the first-node chicken-and-egg, shared-secret vs. cluster-CA vs. external identity
  plane, and the cluster-id/epoch guard against accidental merges) is the *source of
  trust* section of
  [021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md);
  this spec validates against the anchor, 021 establishes it.

This closes the sharpest distributed hole: because placement is *any-node-hosts-any-actor*
(010), cluster admission **is** data authorization at the node granularity. A
rogue node that cannot authenticate cannot be placed onto and therefore cannot
observe state.

## 2. Transport security seam

The `Transport` (010) gains a security wrapper providing **confidentiality,
integrity, mutual authentication, and replay protection** on every node↔node
connection.

```cpp
struct SecureTransport : Transport {                 // decorates the 010 transport
    // handshake establishes peer identity + session keys once per connection;
    // frames are sealed (AEAD) thereafter.
};
```

Because 010 already multiplexes **one connection per peer**, the handshake cost is
paid **once per peer**, then amortized across all traffic — the per-message cost is
one AEAD seal/open, not a handshake. Replay is prevented by per-session sequence
numbers inside the AEAD associated data (composing with, not replacing, the
delivery-layer dedup of 017). The sequence space is **per lane** (data vs
control, by `FrameKind`), not one per session: the transport may reorder
*across* lanes (ADR-046 S4 sends control ahead of queued data) while keeping
each lane FIFO, so one shared counter would reject overtaken data frames as
replays (issue #16). The lane is bound into both the AAD and the nonce, so a
frame cannot be moved between lanes and the lanes never share a nonce.

### The honest exception: crypto is not self-implemented

Every other subsystem in this RFC ships a std-only default and treats heavy
libraries as optional adapters. **Cryptography is the one deliberate exception.**
Rolling an in-house TLS or AEAD stack is a well-known way to ship subtle,
catastrophic vulnerabilities. Therefore:

- There is **no std-only default that performs real cryptography.** The seam's
  built-in implementation is a **plaintext transport** suitable only for a trusted
  single-host or loopback/dev deployment, and it is **named as such** — enabling
  it in a multi-node config emits a loud startup warning (009) and is rejected
  under a `SecurityMode::Strict` config (013).
- Production secure transport is a **thin adapter** over **mbedTLS**
  (`QUARK_WITH_MBEDTLS`, mirroring `QUARK_WITH_SQLITE`/`QUARK_WITH_ROCKSDB`'s
  opt-in-adapter posture — ADR-040), chosen over BoringSSL for this project's
  Windows/MSVC-primary toolchain: BoringSSL has no stable ABI, ships no
  official MSVC support, and its build path is Bazel/GN/Go-only, none of which
  fit a CMake-first, MSVC-primary project. OpenSSL/schannel/Security.framework
  remain viable alternative adapters behind the same seam.
- mTLS is used **once per connection, purely as an authenticated key exchange**
  — a TLS 1.3 mutual handshake, with directional traffic secrets pulled via
  RFC 8446 §7.5's exporter (or mbedTLS's export callback) — **never as the
  record layer** for application traffic. The existing AEAD/sequence-number/
  replay envelope in `secure_transport.hpp` carries every application frame
  after the handshake completes; the handshake's only job is authenticating
  the peer and deriving that envelope's per-session directional keys.
- Per-peer session state (the directional cipher pair, sequence counter,
  replay high-water mark, key `generation`, and the peer's retained
  certificate fingerprint) is a `PeerSession`, folded into the transport's
  existing per-peer lock/map — see
  [010-Distribution.md](010-Distribution.md)'s Transport seam section for the
  load-bearing ordering invariant this requires.
- The engine core still links **zero** crypto in a single-node/default build;
  the mbedTLS adapter is linked only when `QUARK_WITH_MBEDTLS=ON` — proven by
  a dedicated build-isolation test that scans the default build's core
  library for `mbedtls_`-prefixed symbols (ADR-040).
- **A second adapter, libsodium (`QUARK_WITH_SODIUM`, issue #40, ADR-068),** carries the
  pinned-key mode below under the same posture: opt-in, never linked into the core
  (the isolation test also scans for `sodium_`/`crypto_` symbols), and the
  cryptographic *primitives* (X25519, ChaCha20-Poly1305, BLAKE2b) are libsodium's.
  What Quark writes is only the Noise state machine over them, proven against the
  official test vector, byte for byte. That is a narrower step than "implement
  crypto" but it is not zero: it is the one place this repo writes protocol glue
  over a primitive library, it is called out in ADR-068, and it wants an
  independent audit before it guards anything valuable.

This is the intellectually honest position: minimal-dependency is a principle, not
a suicide pact, and "don't roll your own crypto" outranks it.

### Pinned-key mode (Noise IK, #40)

A second handshake mode beside the CA chain, for clusters with **no CA**: peers authenticate each other by **public keys
pinned by the application** (AeroCoWorker's serverless LAN group pins device keys at an in-person pairing). Decision record
and evidence: [ADR-068](decisions/ADR-068-pinned-key-noise-ik.md). It sits behind the unchanged `HandshakeEngine` seam
(`handshake.hpp`) as `PinnedHandshakeEngine`; `SecureTransport` drives it exactly as it drives the TLS engine, and the per-lane
counter/replay envelope carries every frame afterwards.

**Why not TLS with pinned certificates.** An independent red-team showed that TLS cannot hide a device's identity from an
*active* prober on the LAN: a TLS server answers a ClientHello with its ServerHello before it knows who is asking, and
its certificate (or a stable fingerprint of it) is the identity. A pattern in which the responder's static key is known to the
initiator in advance and the responder stays silent until the first message authenticates is what the requirement needs, and
that is Noise IK.

- **Selection and no downgrade.** The mode is chosen per cluster by which `HandshakeEngineFactory` is installed
  (`TrustMode::CaChain` = mbedTLS, `TrustMode::PinnedKey` = Noise IK). There is nothing to negotiate. The mode byte is part of
  the Noise **prologue** (`"quark-pinned-key/v1" || mode`), so a CA-mode peer and a pinned-mode peer never complete a handshake
  in either direction (checked with real mbedTLS engines: `pinned_crossmode_check`). The prologue separates Noise peers of
  different modes; a TLS peer's bytes are not Noise at all and are refused on their own, so the cross-mode guarantee rests on
  both and the test exercises both.
- **Identity is the full public key.** A peer is its **Ed25519 public key** (32 bytes), never a hash or a truncation; the trust
  decision compares the full key. Its **NodeId is derived**, never self-claimed: the first 8 bytes (little-endian) of SHA-256 of
  the public key. Msg1 carries the NodeId and the responder refuses one that is not the derived one, so nobody can claim
  another key's NodeId. Because the id is 64 bits, the application's key book refuses to admit a second key whose derived id
  collides with an admitted one (`PinnedKeyBook::admit`); NodeId `0` is "unspecified" and never a valid derived id. **Sign-bit
  malleability:** Ed25519 keys A and -A (top bit of the last byte flipped) convert to the same X25519 key, so the holder of A's
  secret can authenticate as -A. Trust compares exact Ed25519 bytes, which is safe only if the book never holds both, so
  `PinnedKeyBook::admit` refuses a second key with an already-admitted key's X25519 image; an application implementing its own
  `PinnedTrust` must do the same. The identity that is really authenticated is the X25519 image of the key.
- **Noise static key = the converted Ed25519 key.** libsodium `crypto_sign_ed25519_pk_to_curve25519` / `_sk_to_curve25519`. The
  map is one-way (the sign bit is lost), so Msg1 carries the Ed25519 key in its encrypted payload and the responder checks that
  converting it yields exactly the static key that authenticated the message. **Key-reuse caveat, accepted:** one key does both
  Ed25519 signatures (the application's) and X25519 DH (the handshake's). The two uses are domain-separated (the handshake never
  signs; the application never DHes; the prologue carries a Quark label), libsodium supports the conversion, and published
  analyses of combined use find no break under that separation. It is accepted because a second per-device key would have to be
  generated, paired, pinned and rotated too, and the consumer's pairing flow pins one key. ADR-068 lists it for audit.
- **Suite: `Noise_IK_25519_ChaChaPoly_BLAKE2b`** (Noise revision 34). Msg1 `-> e, es, s, ss` (154 bytes: 96 overhead + a 58-byte
  payload: u16 version, u64 scope/cluster id, u64 initiator NodeId, u64 timestamp, 32-byte Ed25519 key); Msg2 `<- e, ee, se`
  (58 bytes: 48 overhead + u16 version, u64 responder NodeId). Both are fixed length. The initiator knows (pins) the responder's key
  in advance.
- **No cleartext HELLO, no identity before admission (the specified property).** In this mode the TCP `QRK1` HELLO is **not
  sent**: the information it carries (the version for the ADR-053 window, the NodeId for dial dedup) travels inside the encrypted
  handshake payloads. A device reveals no stable identifier, scope id or NodeId to anyone who has not proven it is admitted. Msg1
  and Msg2 are fixed length and carry no identifier in the clear, so two sessions by one initiator share no bytes beyond that
  (the one non-random bit is the top bit of each ephemeral X25519 public key, always 0: a classifier can recognise the protocol,
  not a device). **STATUS, honestly: delivered by the handshake bytes and by `PinnedHandshakeEngine`, NOT yet by the shipped
  transports.** `TcpTransport` still emits its HELLO and `SecureTransport` still puts `from`/`to` (the derived NodeIds) in the
  clear in every frame header, including the Authenticate frames, and takes the responder's `expected_peer` from that claimed
  `from`. Over those transports a pinned-mode node therefore still discloses its NodeId to a passive observer and to a prober that
  sends nothing. Closing that is a transport change (a pinned accept path with no HELLO, identification by the handshake, sealed
  headers), recorded as the follow-up in ADR-068; until then the property holds for byte-level users of the engine only.
- **Silent refusal (engine contract: message-oriented).** The responder engine takes exactly one whole 154-byte Msg1 per
  `advance()`; a byte-stream caller reassembles it (a partial write is the caller's business and gets silence). If Msg1 does not decrypt (a prober without the responder's key), is the wrong length, names the wrong
  cluster or an out-of-window version, carries a NodeId that is not the derived one, comes from a key the application does not
  admit (or one in the revocation set), or has a stale or replayed timestamp, the responder sends **nothing** and the connection
  is closed. An initiator likewise sends nothing to a key the application does not currently admit. What an attacker can still
  learn: that something accepts TCP on the port, and, by timing, when the connection was closed. **Timing residual (unmeasured):**
  a Msg1 that authenticates under the responder's static key (built by anyone who holds the responder's public key: a revoked
  member, a reader of the membership log) is refused only after more work (a second X25519, an AEAD, a key conversion, a book
  lookup) than random bytes are, so the delay before the close can confirm that "the device with this public key is here". The
  engine does not equalise it. A refused initiator hears
  nothing, so its parked handshake ends by the handshake timeout, not by a message.
- **Replay rule (WireGuard's idea).** Msg1's timestamp must be within `max_clock_skew_ns` of the responder's clock (default 120 s;
  LAN devices are not clock-synchronised) and strictly greater than the latest timestamp already accepted from that initiator key,
  kept in a bounded per-responder record (`PinnedReplayGuard`, only admitted keys reach it). The initiator draws timestamps
  strictly increasing even across a clock step back. **Residuals (checked as documented limits in `pinned_handshake_check`):** (a) the record
  lives in memory, so after a responder restart (or an eviction) a captured Msg1 inside the skew window is accepted **once**. The
  replayer cannot derive any key, but `SecureTransport` installs a session for the initiator on the replayer's connection, so the
  responder believes that peer is up and its traffic to it is lost until the real initiator redials. (b) An initiator's timestamp
  counter is also in memory: after an initiator restart or a clock step back its handshakes are refused until its clock passes the
  latest timestamp the responder accepted from it (at most `max_clock_skew_ns` ahead of the responder clock; indefinitely if the
  skew bound is disabled). (c) Two Msg1s from one initiator arriving out of order refuse the older.
- **The trust decision is the application's.** `PinnedTrust::admitted(cluster, ed25519_public_key) -> bool` (`core/pinned_key.hpp`):
  std-only types in the signature, `noexcept`, thread-safe, non-blocking, **consulted fresh at every handshake** by both ends, plus
  `PinnedKeyDirectory::key_of(cluster, node)` giving the initiator the key it pins. `PinnedKeyBook` is a ready-made mutable
  allowlist implementing both. Un-admit makes the *next* handshake fail; re-admit makes it succeed.
- **Session keys.** Noise `Split()` yields the two directional keys; each becomes a Quark envelope key as
  `HMAC-BLAKE2b(split_key, "quark-pinned/v1 c2s" | "...s2c" || handshake_hash)`, a label domain disjoint from TLS mode's
  `"quark c2s"`/`"quark s2c"` exporter labels and bound to that very handshake. They key `SodiumAeadChaCha20Poly1305` (the
  libsodium implementation of the `Aead` seam) inside the unchanged envelope; a pinned-mode build needs libsodium only.
  Forward secrecy: the transport keys depend on `ee` and `se` with fresh ephemerals, so a later compromise of both long-term keys
  cannot open a captured session. **Documented limit:** Msg1 is encrypted to the responder's *static* key, so a later compromise of
  the responder's key reveals which keys dialled it and when (not any session traffic).
- **`peer_fingerprint` in `HandshakeEngine::Result`** carries the peer's full Ed25519 key in this mode, so the revocation set and
  `sweep_revocations` (ADR-040) can name a pinned peer by it.
- **Out of scope (stated, not forgotten).** Re-checking sessions that are *already open* when trust changes is **#41**
  (application-driven admission and eviction). Discovery is #42, an Android PAL #43, several clusters in one process #44.
  Also not done in this change, and recorded in ADR-068: making the **TCP framing itself identifier-free**. Today the frame header
  that `TcpTransport`/`SecureTransport` carry still holds `from`/`to` NodeIds in the clear and `TcpTransport` still emits its HELLO;
  the pinned handshake bytes are identifier-free and a prober gets nothing, but wiring a pinned-mode connection state machine into
  `TcpTransport` (no HELLO, identification by the handshake, sealed headers) is the follow-up.

### Rotation and revocation invariants (ADR-040)

Node identity and cluster trust roots are **hot-reloadable**, not fixed at
process start: `OverlappedMaterial<T>` (current + a grace-windowed previous,
operator-controlled TTL) lets a node rotate its own certificate, or the
cluster's trusted CA roots, without a coordinated restart — a fresh handshake
always signs with the new `current` material; `previous` exists only so an
already-open session that predates the rotation is not disrupted before its
grace window elapses.

Compromise is handled separately, and with **no grace window**: a
`RevocationRegistry` of compromised-key fingerprints gossips over the *same*
channel 021 already uses for membership convergence (no separate, slower
propagation path) and is enforced two ways — at handshake time, against a new
connection attempt, **and** on a bounded sweep tick, against sessions that are
**already open**. Both enforcement paths are required: checking only at
handshake time leaves an already-open session usable by a since-revoked peer
indefinitely.

Two invariants follow, load-bearing (each carries its own test):

- **A revoked peer fingerprint must close any live session to that peer
  within one sweep-tick interval, independent of whether that session
  predates the revocation.** Enforcing revocation only against *new*
  connection attempts and not already-open sessions was a fatal gap found and
  closed during this ADR's design debate.
- **AEAD seal/open for an established session never executes while holding a
  lock shared with any other peer's session.** Any per-session serialization
  added for ordering correctness (010-Distribution.md's Transport seam
  section) must be scoped to that one session's own mutex, never a
  cluster-wide one — otherwise one peer's crypto work stalls every other
  peer's traffic.

### Capability gossip decode discipline (ADR-045)

Node capability entries (`ControlMsg.capabilities`, cluster.hpp) piggyback on the
same control-frame channel as membership `updates` and the revocation gossip above,
and are decoded with the same adversarial-frame discipline: every field — node id,
incarnation, `local_seq`, and the per-entry blob — routes through the bounds-checked
`SwimByteReader` accessors, and a defensive `kMaxDecodedCapabilityBytes` total-
allocation cap rejects an oversized-but-well-formed frame BEFORE allocating, wholly
independent of what any individual entry's declared blob length claims. Proven via
20k fuzzed inputs under ASan+UBSan with zero crashes and zero over-cap/partial
accepts (ADR-045 S3).

**Residual, not newly introduced, risk:** capability content itself is not
cryptographically authenticated beyond the cluster's existing mTLS/session trust
boundary — a compromised or buggy peer already admitted to the cluster can still
advertise a false capability (e.g. a false `Flag{"gpu"}`). This is the identical
trust model membership and revocation gossip already operate under; no new gap is
introduced by capability gossip, and no new mitigation is added for it either.

## 3. Authorization and principal propagation

Authentication establishes *who* (a **principal** — a service identity, a user, a
node). Authorization decides *whether that principal may send message `M` to
target actor `A`*. Two design choices:

**Where the check runs.** At the **boundary, before the handler** — an
authorization interceptor sits between deserialization and dispatch, so handler
code never contains `if (allowed)` boilerplate and a denied message is dropped to
a security dead-letter (below) before it can mutate state.

**How principal reaches the handler.** The principal rides in the **`MessageContext`
(004)** alongside the deadline, trace id, and stop token — it is another ambient
per-message value. And crucially it **propagates down the causal chain the same
way deadlines do (018)**: a message a handler sends inherits the current
principal by default, so an end-to-end call chain carries the originator's
identity without threading it through every signature.

```cpp
// available in a handler, no plumbing:
const quark::Principal& who = ctx.principal();
```

Unlike a deadline, a principal may be **attenuated but not amplified**: a handler
may drop to a lesser principal for a downstream send (least privilege / delegation)
but cannot forge a *stronger* one — the runtime stamps outbound principals from the
inbound context, and elevation requires an explicit, audited capability grant, not
a field assignment. (Deadlines inherit *tighter*; principals inherit *weaker*. The
symmetry is deliberate: both monotonically shrink authority/budget down the chain.)

**Policy model.** The default is an **ACL/policy seam** — `Authorizer::allow(principal,
ActorId, type_key(M)) -> bool` — pluggable to RBAC, OPA/Rego, or a capability
system. The RFC's preferred long-term direction is **capability-based**: an
`ActorRef<A>` (006) is already an unforgeable-by-construction handle, so a
capability model where *possessing a ref is the authorization* fits the actor
model natively (noted as a future direction, not the v1 default).

**Principal propagation mechanism (ADR-044, Accepted — "Flag-Gated Envelope
Pool").** A wire-arrived, non-anonymous `Principal` is carried from
`DistributedRouter::deliver` through `inbound_thunk` (which forwards
`MessageFrame::principal`) into `LocalRouter::deliver_from_wire`, which routes
the descriptor through a second, wire-scale pool — `EnvelopePool`
(`quark::detail::EnvelopePool`/`DescriptorEnvelope`) — instead of the plain
`MessagePool` whenever `principal.anonymous() == false`. It is resolved into
`current_context().principal` (or the per-frame `ReFrame::ctx.principal` under
`Reentrant`/`MaxConcurrency<N>`) via one flag-gated pooled-struct dereference
at **all four** claim/dispatch sites: plain `Sequential` drain, governed-
`Sequential` drain, `Reentrant` admit, and supervised-restart redelivery (007).
A purely local send with an anonymous ambient principal (the default) never
touches `EnvelopePool` and pays zero extra cost — this is the "a purely
intra-process tell pays nothing" invariant above, now backed by measured
evidence (ADR-044 F1) rather than assumed. `Descriptor` itself is untouched by
this mechanism — it stays at its real, ten-times-proven 56-byte layout; the
mechanism spends one previously-unused bit of the existing 12-bit `flags`
subfield instead of growing the descriptor. See ADR-044 for the full design
debate, the two competing mechanisms it rejected, and the evidence table.

Two residual risks are carried forward, not yet closed: no TSan-capable
toolchain was available to prove the concurrency-safety claims for real (ASan +
a coroutine-resume stress substitute came back clean, which is not the same
guarantee); and `EnvelopePool`'s mutex-guarded acquire is a measured 9–34%
marginal cost on trivial, control-frame-sized messages (heartbeats, bare acks)
— real but small in absolute terms (~15–20 ns), and within budget for
realistic payload sizes.

## 4. Secrets

013 deferred secrets to "the host's secret source"; this pins the seam. Secrets
(cluster keys, TLS certs/keys, store credentials) are **never** literals in
`EngineConfig` and **never** reach a log or metric (009).

```cpp
struct SecretSource {
    virtual std::expected<Secret, error> get(std::string_view name) = 0;
    // Secret zeroizes its buffer on destruction; not copyable to a std::string.
};
```

- Defaults/adapters: environment (`QUARK_SECRET_*`), a file/mounted-secret
  reader, and OS keystores behind the same seam — **macOS Keychain, Windows
  DPAPI/CNG, Linux kernel keyring / files** (this OS split is a `SecretSource`
  adapter concern, not a PAL one — the PAL stays about compute/IO, 019).
- The `EngineConfig` **surface** (013) holds secret **references** (names), not values;
  resolution happens at startup through the `SecretSource`, and resolved material lives in
  a zeroizing `Secret` type, never in the config struct. (Implementation note: the
  frozen-core `EngineConfig` stays trivially-copyable — the `SecurityMode` posture is a
  plain enum field; the string-valued secret *references* live in a `SecurityConfig`
  companion on the same 013 config surface, so the frozen aggregate keeps its
  trivially-copyable/aggregate-init contract. The invariant is *references, not values,
  resolved at startup* — wherever on the config surface the reference sits.)

## 5. Data at rest

Persistence (012) records (WAL + snapshots) may hold sensitive state. Encryption
at rest is **optional envelope encryption**: a per-actor or per-shard data key,
wrapped by a key-encryption key from a `Keyring` seam (a KMS, an OS keystore, or a
static key from the `SecretSource`). It composes cleanly with 016's encoding — the
canonical tagged bytes are produced first (so schema evolution/migration is
unchanged), then sealed before hitting the disk record. Fencing tokens (012, 017)
are integrity-relevant and are covered by the record's AEAD tag so a stale token
cannot be spliced onto fresh ciphertext.

## Randomness — a PAL concern

Every layer above needs a **cryptographically secure RNG** (nonces, session keys,
tokens). This is the one security primitive that *is* an OS service and therefore
belongs to the PAL (019): `getrandom` (Linux), `BCryptGenRandom` (Windows),
`arc4random_buf` / `getentropy` (macOS/BSD). The PAL exposes one canonical CSPRNG
so no subsystem reaches for a non-cryptographic `std::mt19937` by accident. (Note:
this is distinct from the *deterministic seeded* PRNG the simulation backend uses
for test reproducibility — sim runs with security disabled or with a stub CSPRNG.)

## Audit

Security-relevant events — authentication failures, authorization denials, cluster
admission/rejection, secret-resolution failures — flow to an **`AuditSink`** seam,
a sibling of 009's `MetricsSink`/`TraceSink`/`DeadLetterSink`. Denied messages go
to a dedicated **security dead-letter** stream (distinct from the ordinary
dead-letter of 007, so a authz denial is not confused with a poison message). The
default sink is structured records to stderr/file; SIEM/OTLP export is an adapter.

## Self-debate

- **Ship a std-only default TLS to honor the minimal-dependency principle?**
  Rejected, firmly. In-house crypto is the classic source of severe CVEs; the
  principle "self-contained default" is subordinate to "never roll your own
  crypto." The default is *plaintext, explicitly labeled dev-only*, and secure
  transport is a thin adapter over a vetted library. Being loud about this beats a
  false sense of security.
- **Authorize inside the handler vs. at the boundary?** Boundary. Putting the
  check before dispatch keeps handler code clean, prevents a denied message from
  ever touching state, and lets the denial be audited uniformly. A handler *may*
  still do fine-grained business checks, but coarse authz is not its job.
- **Principal inherits down the chain (like a deadline) or must be re-supplied per
  send?** Inherit — otherwise every handler re-plumbs identity and someone forgets,
  creating a confused-deputy hole. But inheritance is **attenuating only**: you may
  delegate *less* authority downstream, never forge more. This mirrors 018's
  deadline inheritance (monotonically non-increasing) and is the security analogue
  of it.
- **Capabilities vs. ACLs?** Capabilities fit the actor model beautifully (a ref
  *is* a permission) and are the stated long-term direction, but a v1 that assumes
  every existing `ActorRef` is a capability would force a large model change (ref
  attenuation, revocation, non-forgeable serialization across the wire). Ship the
  ACL/`Authorizer` seam now; evolve toward capabilities without breaking it.
- **Encrypt intra-node messages too, for defense in depth?** Rejected: same-process
  actors share an address space; encrypting a message one actor hands another
  defends nothing an attacker with process memory access couldn't already read. It
  would tax the hot path for zero real gain. Security stops at the process boundary.

## Non-goals

- **Actor-vs-actor isolation within one process.** Co-located actors share memory;
  the engine gives sequential-execution safety (001), not a sandbox. Untrusted
  code isolation is an OS/language problem (separate processes/nodes).
- **DoS / rate limiting / quotas.** An availability concern, not a
  confidentiality/integrity one — specced separately as the *availability sibling* of
  this document in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md)
  (rate limiting shares this spec's ingress boundary and principal key; the coarse
  volumetric gate runs *ahead* of authz). Note the further boundary 022 draws:
  **application-layer** governance is the engine's; **network/volumetric DoS is the
  kernel/load-balancer's**, not the engine's.
- **Side-channel / constant-time guarantees** in application handler code.
- **A bespoke cryptographic protocol.** We adopt vetted primitives (TLS-class
  handshake, AEAD), never invent them.

## Interaction

- **010** — `SecureTransport` decorates the transport; node identity gates SWIM
  admission and HRW placement. This is where the layer physically lives.
- **026** — large-scale topologies keep SWIM over the authenticated `SecureTransport`,
  so bin/HRW placement is still admission-gated. Relays and D2 gateways forward
  application frames as **same-trust-anchor peers** (no new crypto) but **concentrate
  cross-partition traffic** — they are the natural DoS/hotspot target and must be
  provisioned + rate-limited (022). The opt-in datagram SWIM control plane needs a
  **per-datagram AEAD seal** under the session key, or the failure detector is
  spoofable.
- **004 / 006** — `MessageContext::principal()` is the ambient identity; the send
  verbs stamp outbound principals (attenuating) from the inbound context.
- **018** — principal propagation reuses the causal-chain inheritance machinery
  that deadlines use; the two are the ambient values that ride every hop.
- **012 / 016** — at-rest encryption seals 016's canonical bytes after encoding, so
  evolution/migration is unaffected; fencing tokens fall under the AEAD tag.
- **013** — `EngineConfig` carries secret *references* and a `SecurityMode`
  (`Off`/`Strict`); Strict rejects plaintext transport on a multi-node cluster at
  Validation (008).
- **009** — authn/authz outcomes and admission events go to the `AuditSink`;
  denials to a distinct security dead-letter stream.
- **019** — the CSPRNG is a PAL primitive; the simulation backend (014) stubs it.
- **008** — Validation enforces the config-level invariants (e.g. secure transport
  required when distribution + `Strict`), consistent with the `std::expected<…,
  ValidationReport>` model.

## Dependencies

Std for the core seams (`Authorizer`, `SecretSource`, `AuditSink`, `Keyring`
interfaces) — these add **nothing** to a single-node build. The **only** heavy
dependency in this spec is the vetted crypto library behind `SecureTransport` /
at-rest encryption (recommended mbedTLS or BoringSSL), linked **only** when a
secure cluster or at-rest encryption is configured. The CSPRNG comes from the PAL
(019). No security machinery is compiled into a single-node, no-ingress engine.

## Open questions

- **Key rotation** for data-encryption keys without a cluster restart — online
  rotation vs. drain-and-restart. Interacts with quiescence (015). (Node
  *identity* rotation — the transport mTLS certificate — is resolved below,
  ADR-040.)
- **Certificate/identity revocation.** Resolved (ADR-040): **both, layered**.
  Normal-case identity rotation is `OverlappedMaterial<T>`-based (current +
  grace-windowed previous, operator-controlled TTL, no forced short-lived
  certs). Compromise-case revocation is a gossiped `RevocationRegistry`
  (retained peer fingerprints) riding 021's existing membership-convergence
  channel, enforced against already-open sessions on a bounded sweep tick (see
  "Rotation and revocation invariants" above) — not just at handshake time.
  Residual: under a network partition, a node holding a stolen key can still
  authenticate to peers on the far side until the partition heals and gossip
  catches up — a measured, not eliminated, propagation-latency window.
- **Capability model concretely** — ref attenuation, revocation, and non-forgeable
  serialization of an `ActorRef`-as-capability across the wire (016).
- **Ingress authentication** shape — is there a first-class gateway/edge actor, or
  is ingress entirely a host-app concern that hands the engine an already-authenticated
  principal?
- **DoS / rate-limiting** — resolved as the availability sibling of this spec in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md)
  (rate limiting shares this spec's ingress boundary + principal key).
