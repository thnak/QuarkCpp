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
delivery-layer dedup of 017).

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
- Production secure transport is a **thin adapter** over one vetted library —
  default recommendation **mbedTLS or BoringSSL** (portable across
  Linux/Windows/macOS + x86-64/ARM64, small footprint) — behind the
  `SecureTransport` seam. OpenSSL/schannel/Security.framework are alternatives.
- The engine core still links **zero** crypto in a single-node build; the adapter
  is linked only when a secure cluster is configured.

This is the intellectually honest position: minimal-dependency is a principle, not
a suicide pact, and "don't roll your own crypto" outranks it.

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

- **Key rotation** for node identities and data-encryption keys without a cluster
  restart — online rotation vs. drain-and-restart. Interacts with membership (010)
  and quiescence (015).
- **Certificate/identity revocation** propagation latency in a decentralized
  (no-coordinator) cluster — gossip a revocation list over SWIM, or require a
  short-lived-cert model (SPIFFE-style) that sidesteps revocation?
- **Capability model concretely** — ref attenuation, revocation, and non-forgeable
  serialization of an `ActorRef`-as-capability across the wire (016).
- **Ingress authentication** shape — is there a first-class gateway/edge actor, or
  is ingress entirely a host-app concern that hands the engine an already-authenticated
  principal?
- **DoS / rate-limiting** — resolved as the availability sibling of this spec in
  [022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md)
  (rate limiting shares this spec's ingress boundary + principal key).
