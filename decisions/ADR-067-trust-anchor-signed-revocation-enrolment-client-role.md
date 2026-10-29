# ADR-067 — Trust-anchor-signed revocation, ticketed enrolment, and the client identity role (020/021; #35, #36, #37)

Status: **Proposed** (2026-10-29). Design settled by direct discussion with the owner, then red-teamed
(round 1, 2026-10-29: 12 findings, all folded in below; see *Red-team round 1*). The tests listed under
*Evidence required* are outstanding. Amends 020 §1 and §"Rotation and revocation invariants", and 021 §1
and §3 "Join". Nothing here touches a hot path.

## Problem

Three issues from AeroCoWorker (ADR-001 §9, ADR-017) land on one trust boundary — the cluster CA:

* **#35** — revocation is not issuer-authenticated. Any admitted member can gossip a revocation for a
  *third party* (`revocation_registry.hpp`, "TRUST AND BOUNDS (C5)"); revocation is irreversible by
  design (union-only ratchet), so one compromised member can evict the anchor or every sibling.
* **#36** — Quark admits certificates but cannot issue one. There is no CSR / ticket / issue path, so an
  application must build the security-critical half of the trust boundary itself, on Quark's identity
  format.
* **#37** — every authenticated peer is implicitly a member. Investigation (issue comment, 2026-10-29)
  found that the data path already works without SWIM, but: (1) nothing *forbids* a device cert from
  sending a SWIM `Join` or being relayed for — "non-member" is a convention; (2) handshake role is
  `self < peer`, so a device with the higher NodeId never gets a session; (3) revocation of an open
  session relies on SWIM suspicion to close the socket, which a non-member never triggers.

Hard constraint from the consumer, adopted as a Quark rule: **the CA private key never enters a Quark
process in any configuration.** The application keeps it in a sign-only custodian with no export verb.

Found during design: `parse_cn_identity` (`mbedtls_handshake.hpp:99-116`) does not check that
`from_chars` consumed either numeric field, so `quark:1x:5<anything>` parses as cluster 1, node 5.

## Decision

### D1 — Quark verifies, never signs (the custody seam)

* A new `TrustAnchorVerifier` seam (std-only interface; mbedTLS adapter) verifies a signature against the
  cluster `TrustStore` — current roots plus the still-in-window previous roots (ADR-040 overlap). The
  std-only core contains **no** signing primitive.
* **Roots are ECDSA P-256; signatures are ECDSA-with-SHA-256, DER-encoded.** Only a CA root (CA:TRUE,
  keyCertSign when keyUsage is present) with a P-256 key is a revocation signer; any other certificate in
  the trust bundle is skipped by the verifier, so a record naming it never verifies (it still anchors TLS).
* **Revocation uses pre-signed records.** The application's custodian signs; the application calls
  `revoke(SignedRevocation)`; Quark verifies, applies locally, gossips. Revocation is an operator act and
  may be produced offline.
* **Issuance uses an asynchronous signer callback.** Issuance is inherently online (a joiner is waiting),
  so Quark validates the request (D4) and hands it to `IssueFn(IssueRequest, Completion)`, which returns
  immediately; the application completes later. The I/O thread never blocks on the custodian.
* **Quark verifies the returned certificate before sending it** — a buggy custodian cannot widen what a
  ticket authorised: chains to a trusted root; exactly one CN, byte-equal to the identity the anchor
  expects (prefix, cluster id, NodeId with the correct role bit, D3); subject public key equals the CSR's;
  `basicConstraints` CA=false or absent; no `keyCertSign` / `cRLSign`; validity within the configured
  issuance bound. Anything else is refused and audited; the ticket stays consumed.
* **Custodian contract** (stated, not enforceable by Quark): the custodian signs *typed requests* — a
  revocation record or a TBSCertificate it builds/inspects itself — over **raw bytes**, never a
  caller-supplied digest. A custodian that signs arbitrary digests voids D2's domain separation.
* **C5 made structural:** startup is refused if the node's own `IdentityMaterial` public key equals any
  trusted-root public key (the "CA cert as node cert" shortcut would put the CA key in-process).

### D2 — Signed revocation record (Quark-defined, not CRL/OCSP)

```
"QUARK-REVOKE-v1\0"  16 B   domain separation; ASCII, cannot begin a DER SEQUENCE (0x30)
cluster_id           u64    a record for another cluster is refused
fingerprint          32 B   SHA-256 of the revoked leaf (RevocationRegistry's existing key)
not_after            u64    the revoked leaf's notAfter, unix seconds — the prune horizon
issued_at            u64    unix seconds; audit only, NEVER a validity gate
anchor_key_id        32 B   SHA-256 of the signing root's SubjectPublicKeyInfo
signature            ≤72 B  DER ECDSA-SHA256 over the 104 preceding bytes
```

* Fixed-length, fixed-layout signed portion: no length ambiguity, no reinterpretation.
* Accepted iff `cluster_id` matches, `anchor_key_id` names a currently trusted root, and the signature
  verifies. Otherwise dropped and counted (`signed_rejected()`); turning counters into audit records is the
  application's job.
* **Identity is the fingerprint, never the record bytes.** ECDSA is malleable (`(r, n−s)` also verifies),
  so dedup and gossip key on `fingerprint`; a record whose fingerprint is already present is **not
  re-verified**, so re-gossip of known records costs no signature check. Verifies of never-seen
  fingerprints are bounded per merge and by a token bucket (`SignedRevocationLimits`); a record over budget
  is deferred, not lost (`signed_deferred()`): every node gossips its whole held set round-robin, so the
  record arrives again. Expired records are refused before any verify (`signed_expired()`). This bounds
  verify CPU. The budget is node-wide, not per sender, so a member flooding forged records keeps genuine
  NEW records deferred for as long as the flood lasts; per-sender budgeting is a follow-up (honest nodes
  re-gossip only verified records, so a bad signature identifies its immediate sender).
* **Replay is harmless**: the registry is a union-only ratchet. No nonce — deliberately.
* **Pruning:** an entry is removed once `now > not_after + skew_bound` — an expired leaf already fails
  the handshake, so pruning cannot re-admit it. Signed entries have their **own cap**, sized by the
  operator for the fleet; the existing `max_entries` cap keeps bounding the unsigned/local set only. A
  full signed cap refuses loudly (`revoke()` returns `errc::overloaded`; gossip counts `signed_cap_refused()`),
  never silently, and the cap is checked before any verify is spent.
* **Distinct cluster ids** are required for clusters sharing a CA (otherwise records replay across them);
  stated in 021 §1.
* **Root rotation:** a record verifies only while its signing root is trusted. Rotation therefore
  includes **re-signing the outstanding signed set** under the new root: Quark exports it
  (`signed_revocations()`), the application has the custodian re-sign, and calls `revoke()` with the new
  records before the old root's window closes. A node joining after the window learns only re-signed
  records — the rotation runbook makes this a required step.
* **Without a verifier** the registry stays in legacy unsigned mode (local `revoke_locally()` plus bare-
  fingerprint gossip, the pre-067 behaviour); signed records are ignored and `revoke(SignedRevocation)`
  **returns an error** (`errc::unavailable`) instead of silently revoking on one node.
  `SecurityMode::Strict` with the handshake enabled refuses to start in that configuration
  (`validate_revocation_trust`). `enable_signed()` is one-way: it drops all previously gossiped unsigned
  entries and from then on refuses unsigned gossip (counted in `rejected()`). A peer pinned to SWIM wire
  v1 carries no signed section, so Strict also refuses a node whose SWIM is pinned to wire v1. **Mixed versions:** during a rolling upgrade old nodes gossip
  bare fingerprints (refused by new nodes) and new nodes gossip records (unknown to old nodes), so
  revocation propagation is not guaranteed until the upgrade completes; documented in the 021 rolling
  upgrade section, and operators revoke locally on every node meanwhile.
* Out of scope: revoking the anchor itself. That is trust-root rotation (ADR-040), consistent with the
  consumer's rule that an old identity cannot authorise its own replacement.

### D3 — Identity role: in the certificate AND in the NodeId

* Two CN forms: `quark:<cluster>:<node>` = **member** (unchanged), `quark-client:<cluster>:<node>` =
  **client**. A distinct prefix, so an un-upgraded parser **rejects** a client cert instead of reading it
  as a member.
* **Disjoint id spaces by construction: NodeId bit 63 is the client bit** (`kClientNodeBit`). The parser
  requires it **set** for `quark-client:` and **clear** for `quark:`; anything else is refused. A client
  can therefore never hold a member's id, whatever the application's allocator does — this is the
  red-team's safety finding #1, closed without re-keying any table.
* `parse_cn_identity` is made strict: both numeric fields consumed exactly, exactly one CN in the Subject.
  The post-handshake check compares role (implied by the id) as well as node and cluster.
* **Role is known before the handshake** because it is a bit of the claimed NodeId. That fixes:
  * *Handshake role:* `local_is_client(self, peer)` = `peer` is a client → we are TLS **server**; `self`
    is a client → we are TLS **client**; both members → `self < peer` (unchanged). Renegotiation follows
    the same rule, so a member never starts a handshake or renegotiation toward a client; it only answers.
  * *Relay:* the member-only form `set_relay_forwarding(const NodeAuthority&)` refuses any frame whose
    origin or destination id has the client bit, whatever the authority answers. #37 requires a device
    behind NAT to reach its home node through a relay, so that is an **explicit per-pair opt-in**
    through the predicate form `set_relay_forwarding(fn(origin, dest))`, which sees the bit (e.g. admit
    exactly `(client, its home)`). Frames stay sealed end to end; the relay holds no session.
  * *Placement:* `admitted_nodes`/`place_admitted` never return a client id, whatever the authority
    answers (defence in depth behind SWIM).
* **Enforcement:** a client session's Control frames never reach SWIM (dropped and counted, never
  decoded); a client never enters any membership view, gossip, or `placement_view()`. Data frames and the
  existing `set_principal_policy` binding are unchanged.
* **Revocation close:** a revoked client session is **erased and its connection actively closed** on the
  sweep tick (no SWIM to fall through); a relayed client, which has no direct socket, is left holding
  nothing. A client whose *initial* handshake fails (e.g. a revoked certificate on reconnect) is also
  disconnected rather than left retrying on an open socket. The 020 invariant "close within one sweep
  tick" now holds for both roles.
* **Role rule, all pairs:** member↔client — the client dials and is the TLS client; same-role pairs —
  the lower id. Both ends always compute complementary answers (two clients included).
* **Operational notes:** (1) handshake admission (`set_handshake_admission*`) is consulted for clients
  too — an admission allowlist derived from membership refuses every client, so admit clients by the
  id bit (or a client roster) there; (2) a member replies to a *relayed* client only if it routes that
  client via the relay too (`set_relay_via(client, {relay})`); (3) a client never runs
  `SwimMembership` — it is the member side that keeps it out.
* **Cost:** client NodeIds live in the upper half of the 64-bit space; the application allocator must set
  the bit (Quark refuses an `IssueRequest` result that does not match the ticket's role).

### D4 — Ticketed enrolment, owned by Quark

* `Enrollment` is a transport-agnostic state machine (the `lease.hpp` pattern): ticket table, request
  validation, rate limits, audit, `IssueFn` completion. Not on any hot path.
* **Join code** = `{anchor endpoint, anchor NodeId, SHA-256 of the CA root cert, ticket_id,
  ticket_secret}`. `ticket_secret` is **≥ 128 bits from the PAL CSPRNG**.
* **Wire:** the joiner opens a fresh connection on the anchor's existing listener and sends an **enrol
  hello** (no NodeId). Inside **server-authenticated TLS** the joiner requires the server leaf to chain to
  the pinned CA **and** to be a **member**-role cert (`quark:` prefix) for exactly the join code's
  `cluster_id` and anchor NodeId — a client cert or any other member cannot impersonate the anchor. Only
  then does it send `ticket_id`, a CSR, and `HMAC-SHA256(ticket_secret, CSR)`; the secret never travels.
  The anchor replies with the certificate chain or a refusal code.
* **Isolation and bounds (C6):** the enrolment socket leaves the member pending-inbound pool at the enrol
  hello and counts against a **separate, small `max_concurrent_enrolments` cap** (and a per-IP cap); an
  enrol hello is refused outright while **no unexpired ticket exists**. Enrolment TLS and verification
  run **off the transport I/O thread** (a bounded cold-path worker), so a flood of unauthenticated
  ECDHE never stalls member traffic (020's crypto-isolation invariant).
* **Check order (fixed):** ticket exists, unexpired, under its attempt limit → constant-time HMAC verify →
  CSR proof-of-possession → **consume** (atomic, single use) → `IssueFn`. A wrong HMAC or bad CSR counts
  an attempt but **does not consume** the ticket; a consumed ticket stays consumed even if the custodian
  then fails.
* **The anchor never honours a requested name.** The CSR contributes only its public key and
  proof-of-possession. The application assigns the NodeId (it owns the roster) through `IssueRequest`;
  the role and cluster come from the ticket; Quark verifies the result (D1, D3).
* **Tickets:** created by the application (`issue_ticket(role, ttl)`), held in memory on the anchor,
  short TTL, per-ticket attempt limit, per-IP rate limit. An anchor restart loses outstanding tickets —
  fail closed; the operator re-issues a join code. Durable tickets are a non-goal.
* **Audit:** every attempt, accepted or refused, emits an audit record (ticket id, source address,
  assigned NodeId if any, role, refusal reason).
* Re-issue / rotation of an enrolled identity reuses ADR-040 rotation; enrolment only covers first issue.

## Claims (each must be proven by a test)

* **C1** A member that is not the anchor cannot cause a third party to be revoked.
* **C2** A revocation signature can never be accepted as a certificate signature or vice versa.
* **C3** A client certificate never yields a SWIM member, gossip entry, placement target, a session under
  a member's NodeId, or a relay hop the application did not explicitly opt that pair into — including on
  an un-upgraded node and via a forged Control frame.
* **C4** A ticket authorises at most one certificate, of exactly its role and cluster, even under
  concurrent presentation, a wrong-HMAC attacker, and custodian failure; only the real anchor can redeem it.
* **C5** No CA private-key material is reachable from Quark code in any accepted configuration.
* **C6** Enrolment traffic cannot exhaust the anchor's member-connection capacity or stall its I/O thread.
* **C7** A signed revocation is always enforced: never refused by a full cap silently, never lost across
  root rotation when the runbook is followed, never dropped silently for lack of a verifier.

## Red-team round 1 (2026-10-29) — disposition

Held: C1; C2 (by the fixed-length P-256/SHA-256 record more than the prefix); C3 on un-upgraded nodes;
forged Control frames (`handle_control` already binds `m.from` to the authenticated sender); the CSR
binding. Found and folded in:

| # | Finding | Severity | Resolution |
|---|---|---|---|
| 1 | Shared NodeId space: a client cert with a member's id takes the member's slot while it is disconnected | breaks safety | D3 client bit 63, parser-enforced |
| 2 | Handshake role decided before the peer role is known → client/client glare livelock | liveness | D3: role read from the id bit |
| 3 | Enrolment either consumes member pending slots or escapes all caps; crypto on the I/O thread | availability | D4 separate cap, ticket-gated hello, off-thread crypto |
| 4 | Unpruned `max_entries` cap fills with device revocations and refuses a signed one | weakens safety | D2 `not_after` prune, separate signed cap, loud refusal |
| 5 | Fail-closed default silently localises `revoke()`; mixed-version propagation gap | weakens | D2 `revoke()` error, Strict refuses start, documented upgrade gap |
| 6 | Any CA-chained cert impersonates the anchor to a joiner → ticket capture, escalation | weakens | D4 anchor NodeId + member role pinned, ≥128-bit secret, fixed check order |
| 7 | Returned-cert check omits CA flags → sub-CA | weakens | D1 full returned-cert check; `verify_cb` rejects CA leaf at depth 0 |
| 8 | Relay/admission role-blind | weakens | D3 client bit refused by relay and allowlists |
| 9 | Revocations lost on root rotation | weakens | D2 re-sign on rotation (runbook step) |
| 10 | Custodian contract, alg pinning, malleability, shared-CA cluster ids | minor | D1/D2 pinned |
| 11 | C5 not structural (CA key used as node identity) | minor | D1 startup refusal |
| 12 | Strict parse must cover the cluster field and single CN | cosmetic | D3 |

## Evidence required before Accepted

* `security_cn_parse_strict_test` — trailing bytes in either field, multiple CNs, role/bit mismatch
  both ways, `quark-client:` refused by an old-style member-only parse; CA leaf at depth 0 refused.
* `security_signed_revocation_test` — C1/C2 matrix (unsigned, wrong signer, tampered, wrong cluster,
  unknown root; positive control anchor-signed accepted); malleated signature dedups by fingerprint;
  replay idempotence; prune after `not_after`; signed cap refusal is loud; `revoke()` without a verifier
  errors; re-signed set survives rotation and a fresh node refuses the revoked leaf.
* `security_client_role_test` — C3: client connects and exchanges frames, absent from every view and
  gossip, Control frames dropped, relay refused; a client cert carrying a member id is refused at parse;
  high-NodeId client and rotation-driven renegotiation re-establish without glare; active close on
  revocation; positive control: the same key enrolled as a member appears.
* `enrollment_test` — C4/C6: valid ticket → cert and join; none / expired / reused / wrong-HMAC (ticket
  survives) / requested name ignored; concurrent same-ticket presentation yields one issuance; joiner
  aborts against a client-role or wrong-id server cert before sending `ticket_id`; returned cert with
  CA:TRUE refused; enrol flood leaves member reconnects within bound; audit record per attempt.
* `enrollment_no_key_test` — C5: links a stub `IssueFn`; asserts it is the only signing path; startup
  refused when own identity key equals a root key.
* All under Release + ASan/UBSan + TSan (enrolment worker and sweep run concurrently with the I/O thread).

## Implementation status

* **D3 client role — done** (`security_client_role_test`, `security_cn_parse_strict_test`, mbedTLS check).
* **D2 signed revocation — done**: `signed_revocation.hpp` (record, `TrustAnchorVerifier` seam),
  `RevocationRegistry` signed mode, `mbedtls_trust_anchor.hpp` (`MbedtlsTrustAnchorVerifier`,
  `anchor_key_id_of_cert`), `validate_revocation_trust` (Strict startup check)
  (`security_signed_revocation_test`; real ECDSA P-256 in the mbedTLS check).
  **SWIM wire encoding (settled):** signed records ride as opaque blobs in a section appended *after*
  the ADR-053 version trailer — every v2 decoder stops at the trailer, so un-upgraded nodes keep
  decoding membership and simply ignore the section (no version bump). Bounded before allocation: at
  most 64 blobs of at most 256 B per frame; `Config::max_signed_revocation_gossip` (default 8, about
  1.4 KB) caps what a node attaches. Successive frames walk the held set round-robin, so every record is gossiped
  however large the set. A malformed section fails the frame, like any malformed field.
  The application still owns `prune_expired` scheduling and the rotation re-sign step.
* **D4 enrolment — not started.**

## Not decided here

* **Device-side client implementation** (#37 gap 4): a written client wire-protocol spec vs. a small
  portable client library without the PAL reactor. Scoping decision with the consumer.
* Issuance availability: the anchor is a single point of availability for enrolment (not for safety).
  Electing an issuing anchor among replicas (028 leases) is a possible follow-up.
