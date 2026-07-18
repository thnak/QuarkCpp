# 021 — Cluster Formation & Lifecycle

010 specs the **steady-state** distributed engine — HRW placement, SWIM
membership, the `Transport`/`Serializer`/`Membership` seams — and 020 specs the
**security boundary** that gates it. Both assume a cluster that already *exists*
and whose members already *trust* each other. Neither says how the cluster comes
into being, how nodes physically connect, where that trust is rooted, or how the
cluster safely changes shape while running. This spec fills that gap: the
**operational lifecycle** of a multi-node deployment — bootstrap, join, connect,
scale up, scale down, and rolling upgrade.

> A single-node engine has no cluster to form. Like 010 and 020, **none of this
> compiles into a single-node build** — it is a layer over the distribution seams,
> not a tax on the core.

## Scope — the seam between three specs

To avoid overlap, the boundaries are explicit:

| Concern | Owned by |
|---|---|
| *Where* an actor lives; failure detection; wire framing | **010** (HRW, SWIM, `Transport`) |
| *Whether* a node/principal is allowed; crypto handshake; identity validation | **020** (`NodeAuthority`, `SecureTransport`) |
| *How* the cluster forms, connects, and reshapes over time | **021** (this spec) |

021 is the **control-plane lifecycle**. It provisions the trust root that 020
validates against, drives the connection mechanics that realize 010's "one
connection per peer," and orchestrates the membership transitions that 010's HRW
reacts to. It defines **no new crypto** (020) and **no new placement math** (010).

## The three questions this answers

The design pressure named three things — *source of trust*, *connections*, and
*scaling the system*. They map to the three lifecycle phases:

```
   FORM                 CONNECT                RESHAPE
   ────                 ───────                ───────
 trust root      →   dial / dedup / keepalive   →   join · drain · rebalance
 (source of          (one long-lived, muxed          (elastic, damped,
  trust)              connection per peer)             fenced hand-off)
```

---

## 1. Source of trust — bootstrapping cluster identity

020 says every node has a cryptographic identity and admission chains to a **trust
anchor**, but a trust anchor is not free-standing: *something* must issue and
vouch for identities, and the very first node has no one to ask. This is the
bootstrap chicken-and-egg, and it is a provisioning decision, not a crypto one.

### The models (self-debate)

- **Shared cluster secret (PSK).** Every node is provisioned with one pre-shared
  cluster key; join = prove knowledge of it (020's handshake). *Simplest, zero
  infrastructure, works air-gapped.* But there is **no per-node identity or
  revocation** — one leaked secret compromises the whole cluster, and rotation is a
  fleet-wide event. Fine for dev, small trusted clusters, or a bootstrap step.
- **Cluster CA.** A signing authority issues **short-lived per-node certificates**
  chaining to a cluster root; the root public key is the trust anchor. *Real
  per-node identity, revocation-by-expiry, no fleet-wide secret.* Cost: the CA
  signing key is now **the** thing to protect (the true "source of trust"), and it
  is an operational component to run.
- **External identity plane (SPIFFE / cloud IAM / mesh).** Delegate identity to the
  platform the cluster already runs on. *Zero bespoke trust infrastructure,
  workload identity for free.* Cost: a hard dependency on that environment; not
  portable to bare metal or air-gapped.

### Decision

The root of trust is **explicit configuration, never implicit or derived**, and
its shape is a provisioning choice behind 020's existing `NodeAuthority` seam —
021 does **not** add a competing seam, it defines what feeds it:

- **Default (dev / small trusted cluster): shared cluster secret**, resolved from
  the `SecretSource` (020, 013) by *reference*, never a literal. Consistent with
  020's honesty rule, using it on a real multi-node cluster is **allowed but loud**
  (startup warning, 009) and **rejected under `SecurityMode::Strict`** (013).
- **Recommended for production: a cluster CA issuing short-lived certs**, or an
  external SPIFFE/cloud identity plane — both `NodeAuthority` adapters (020).
- The trust root is provisioned **out of band, before the process starts** (a
  mounted secret, a cert file, a workload-identity socket). The engine consumes it;
  it never generates or self-signs a root implicitly, because an implicitly
  self-created root is a root no operator chose to trust.

### Cluster identity — preventing accidental merges

Beyond *node* identity, the cluster carries a **cluster id** and a monotonic
**membership epoch**:

- A joining node presents the expected **cluster id** in its handshake; a mismatch
  is rejected. This stops a decommissioned node, or a node pointed at the wrong
  `Discovery` result, from being absorbed into the wrong cluster and corrupting
  placement (a hazard 010's coordinator-free HRW cannot otherwise catch).
- The **epoch** advances on each admitted membership change, giving a total order
  on membership snapshots so a node rejoining after a partition can tell *stale*
  from *current* rather than resurrecting a departed view.

---

## 2. Connections — realizing "one connection per peer"

010 states the invariant — **one multiplexed connection per peer** — but not the
mechanics that make it true under concurrent dialing, failure, and churn. Those
mechanics live here.

### Discovery — the seam that starts everything

SWIM (010) gossips membership once a node is *in*, but a fresh node knows no one.
It needs an initial contact set:

```cpp
struct Discovery {
    virtual std::vector<Endpoint> contacts() = 0;   // where might peers be?
};
```

- **Default: a static seed list** from `EngineConfig` (013 already carries
  `cluster.seeds`) — std-only, works everywhere.
- **Adapters** (behind the seam): DNS SRV, Kubernetes endpoints, cloud provider
  tags, multicast. Environment-specific, so never in the core.
- Seeds are **contact points, not coordinators**: a joiner reaches *any one* live
  seed to pull the membership snapshot, then SWIM takes over. Once joined, a node
  has **no standing dependency** on the seeds — they may die without affecting the
  running cluster. This is the property that keeps the system coordinator-free
  (010) while still bootstrappable.

### Establishment: lazy, not full mesh (self-debate)

- **Eager full mesh.** Every node dials every other at join → *predictable
  latency, no first-message stall*, but **O(N²)** idle connections and a connection
  storm at every membership change. Wasteful when traffic is sparse.
- **Lazy on-demand.** Dial a peer only when the first cross-node message routes to
  it → *few connections, cheap*, but a first-message latency spike, and SWIM needs
  *some* connectivity regardless.
- **Decision: lazy data-plane, gossip control-plane.** SWIM's control traffic is
  already fanout gossip (a subset of peers), not a mesh. **Data connections are
  established on first cross-node send** to a peer, then kept **long-lived and
  multiplexed** (010's one-per-peer). This scales with the *communication graph*,
  not with N², and matches real actor traffic, which is rarely all-to-all.

### Dial deduplication — the concurrency hazard

If A and B send to each other simultaneously, both dial and two connections form,
violating the one-per-peer invariant. Resolved deterministically, no negotiation
round-trip:

> When two connections exist between the same pair, the one **initiated by the
> lower `NodeId`** is kept and the other is closed. Both ends compute the same
> winner from ids alone.

The AEAD handshake (020) rides the surviving connection; the loser is dropped
before any application frame flows, so dedup costs nothing steady-state.

### Liveness and teardown

- **Keepalive reuses SWIM.** The direct/indirect pings SWIM already sends (010)
  double as connection liveness — no separate heartbeat timer. Their smoothed RTT
  is *already* the transit estimate 018 uses for cross-node deadlines.
- **Reconnect with jittered exponential backoff**, capped. Jitter prevents a
  thundering reconnect herd when a peer or switch flaps.
- **Teardown on death.** When SWIM declares a peer dead (suspicion timeout, 010),
  its connection is closed and its buffered outbound frames are dead-lettered
  locally (010); in-flight `ask`s to it fail and escalate (007).

---

## 3. Scaling the system — safe, damped elasticity

HRW's headline property (010) is **minimal reassignment**: adding node M moves
only ~1/N of actors onto M, removing one moves only its actors elsewhere. But
"minimal" is not "free" or "instantaneous" — done naively, a membership change is
a migration storm with a double-activation window. 021 makes scale events *safe
and cheap* so an external autoscaler can trigger them freely.

### Join — a staged finite-state machine

A new node does not appear in placement the instant its process starts:

```
 DISCOVER ─▶ CONNECT ─▶ AUTHENTICATE ─▶ SYNC ─▶ WARM ─▶ ANNOUNCE ─▶ MEMBER
 (Discovery) (dial     (020 handshake,  (pull   (RTT   (gossip     (HRW now
             a seed)   cluster-id check) member- baseline join;      places
                                          ship    for 018) epoch++)   onto it)
                                          snap)
```

- Placement (010) **recomputes only at ANNOUNCE**, once the node can actually host
  work — never against a half-connected node. HRW's determinism means every node
  reaches the same new mapping independently. At scale this recompute is the
  `VirtualBins` refill (026) on the membership thread — a control-plane cost, off the
  drain path.
- **Bulk join is batched (ADR-006).** M joiners are admitted in **one bin-table
  sweep** (one store per changed bin), not M serial repairs — serial back-to-back
  per-event repair blows the ≤2 ms membership budget (a measured 671 ms for a
  100-node bulk join). The **stabilization window** ties to **roster-digest
  transitions**, which are also 026's FIFO path-change boundary — so a burst of joins
  settles to one digest, one path change, one re-place.
- **WARM** primes the 018 transit estimate and any node-scoped resources (004)
  before traffic arrives, so the first re-placed actors don't eat a cold-start
  latency cliff.

### Hand-off — moving live actors without corruption

When placement moves actor `X` from old node O to new node N:

1. O stops accepting *new* messages for X and lets its mailbox **drain** (015
   quiescence); in-flight `ask`s complete or time out (018).
2. **Persistent X** (012): N activates from the store, guarded by a **fencing
   token** (012, 017) — O's activation, if it lingers across a partition, is fenced
   out and cannot corrupt state. This is exactly the split-brain reconciliation 017
   proves; 021 adds nothing to the guarantee, it *invokes* it on every planned move.
3. **In-memory X**: either transferred as a state message or re-activated cold on
   N, per the actor's policy. No store, so no fencing needed — worst case is lost
   volatile state, which an in-memory actor already tolerates on node loss (010).

### Anti-flap — damping churn (self-debate)

- **React to every membership event immediately.** Fast convergence, but a flapping
  node (up/down/up) triggers repeated re-placement — actors thrash between nodes,
  each move paying drain + reload. Churn amplifies cost.
- **Decision: a stabilization window.** A membership change must **hold for a
  configurable settle interval** before it drives re-placement of *healthy*
  actors. A node that flaps within the window causes no reshuffle. Genuine failures
  (sustained beyond the window) re-place as normal. This trades a little
  convergence latency for large protection against thrash — the right trade for a
  system where each move has real cost.

### Scale down — graceful leave vs. crash

- **Graceful drain (planned).** A leaving node announces intent → is removed from
  placement candidacy (no *new* actors land on it) → drains mailboxes and hands off
  its actors (above) → departs membership. In-flight work completes; nothing is
  dead-lettered that didn't have to be.
- **Crash (unplanned).** SWIM detects (010), placement re-computes, actors
  re-activate elsewhere from the store (persistent) or cold (in-memory). The loss
  window is whatever the actor's `Delivery` (017) and `PersistMode` (012) allow —
  021 changes none of that, it just distinguishes the *planned* path that avoids it.

### Rolling upgrade

A version bump is **graceful-leave → replace binary → rejoin**, one node at a
time. Two properties already in the RFC make a mixed-version cluster safe during
the roll:

- **Wire compatibility** — 016's schema-fingerprint negotiation falls back to the
  canonical tagged encoding when two peers differ, so new and old nodes interoperate
  without a flag day.
- **Membership-protocol compatibility** — SWIM's on-wire format is versioned and
  back-compatible within a supported window (a support-policy open question).

The stabilization window above also means the brief per-node absence during its
restart does not thrash placement across the rest of the cluster.

---

## Non-goals

- **Not a process orchestrator.** Deciding *which machines exist* and starting the
  processes is Kubernetes/Nomad/systemd's job; Quark manages **actors within the
  nodes it is given** and integrates with the orchestrator through the `Discovery`
  seam. It does not launch or schedule OS processes.
- **Not an autoscaling policy.** 021 makes join/leave *safe and cheap*; **deciding
  when** to add or remove a node is an external control loop reacting to 009 metrics
  (queue depth, CPU, latency). The engine provides the safe mechanism, not the
  capacity policy.
- **Not strong-consistency membership.** Membership stays SWIM-eventual (010) by
  design. A linearizable config store (Raft/etcd) for membership or a quorum gate is
  an optional `Membership` adapter, not the default — consistent with 010's
  coordinator-free posture.
- **No new cryptography or placement math.** Handshake and identity validation are
  020; the HRW mapping is 010. This spec orchestrates them.

## Interaction

- **010** — drives SWIM membership transitions and triggers HRW re-placement;
  realizes the one-connection-per-peer invariant (dial dedup, lazy establishment);
  reuses SWIM ping RTT for keepalive and 018 transit.
- **020** — provisions the trust root that `NodeAuthority` validates; the join FSM's
  AUTHENTICATE step *is* the `SecureTransport` handshake; cluster-id check rides it.
- **012 / 017** — planned hand-off invokes fencing-token reload; crash recovery uses
  the same durable path. Split-brain safety is 017's proof, exercised on every move.
- **016** — schema-fingerprint fallback is what makes a mixed-version cluster safe
  during a rolling upgrade.
- **015** — graceful drain of a departing/handoff actor uses the quiescence
  primitive (`quiesce(Drain)`).
- **013** — `cluster.seeds`, cluster id, trust-root *references*, stabilization
  window, and graceful-leave timeout are operational config (not policy); validated
  at 008.
- **009** — join/leave/rebalance events, churn rate, and connection state are
  observable; admission failures also flow to 020's `AuditSink`.
- **018 / 004** — WARM primes the transit estimate and node-scoped resources before
  traffic arrives.
- **019** — connections are PAL sockets on the per-OS event loop; no OS API leaks
  above the PAL.

## Dependencies

Std only for the core lifecycle logic and the `Discovery` seam. Discovery adapters
(DNS/K8s/cloud) and a linearizable membership store are optional, behind their
seams, never linked into a default build. The crypto for the AUTHENTICATE step is
020's vetted-library adapter — 021 adds no crypto dependency of its own. Nothing
here is compiled into a single-node engine.

## Open questions

- **Bulk join / cold start.** Bringing up N nodes at once (a fresh cluster, or a
  large scale-out): does each join settle independently, or is there a batched
  admission so the epoch and placement don't churn N times in a row?
- **Hand-off bandwidth control.** A large scale-out moves many persistent actors at
  once; the state-transfer/reload rate is bounded by a governance throttle
  ([022-Resource-Governance-and-Overload-Control.md](022-Resource-Governance-and-Overload-Control.md))
  so a rebalance doesn't saturate the network or the store — open question is the
  *default* rate and how it adapts to observed cluster load (009).
- **Stabilization window vs. failover latency.** The anti-flap settle interval
  directly delays legitimate failover; is one global value right, or should it be
  per-actor-type (a critical actor wants fast failover, a cache tolerates delay)?
- **Quorum / minimum cluster size.** Should admission or continued operation require
  a minimum live-node count to blunt split-brain, and how does that interact with
  the deliberately coordinator-free default (010)?
- **Seed liveness during long partitions.** If every configured seed is
  simultaneously unreachable, a *new* node cannot join even though the cluster is
  healthy — is a healthy member promoted to an ad-hoc contact, or is that purely a
  `Discovery`-adapter concern?
