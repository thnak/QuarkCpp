# 010 — Distribution

Extends the single-node engine across a cluster **without dragging a networking or
serialization framework into the core**. The engine core has *zero* networking
dependencies; distribution is a layer built on three seams — `Transport`,
`Serializer`, and `Membership` — each with a std-only default implementation.

> If distribution is not configured, none of this compiles into the hot path. A
> single-node engine pays nothing for the cluster machinery.

## Placement across nodes

Placement (002) extends by one level:

```
ActorId → node → shard
```

Node selection uses **Rendezvous (Highest-Random-Weight) hashing**:

```
node(ActorId) = argmax over live nodes n of  hash(n.id, ActorId)
```

### Alternatives considered

- **Consistent-hashing ring with virtual nodes**: standard, but requires
  vnode bookkeeping and rebalancing logic, and disruption on membership change is
  larger and less uniform without many vnodes.
- **Decision: Rendezvous/HRW hashing.** No ring state to maintain, provably
  minimal reassignment on membership change (only the affected keys move), and
  every node computes placement independently from the membership set — no
  coordinator. Cost is O(nodes) per placement for a small cluster; at scale the
  **`VirtualBins` cache of [026](026-Large-Scale-Cluster-Topology.md) makes it O(1)
  (proven 5–6 ns, N-independent)**, and beyond ~10⁴ nodes the D2 `Partitioned` tier
  applies. The connections axis is likewise a policy: default one-per-peer is
  `FullMesh`; 026 adds `BoundedPartialView`/`Gateway` to break the O(N²)
  cluster-wide socket count.

Placement stays **stable** (core invariant 4): given the same membership, every
node maps an `ActorId` to the same node deterministically.

**Capability-constrained placement** rides this without breaking determinism. Nodes
advertise **static capabilities** (labels, flags, a capacity `weight`) in the SWIM
join payload, gossiped with membership. A placement policy (005) may restrict HRW to
the **eligible subset** (`Require<Gpu>`), bias it (`Prefer<SameZone>`, `LocalFirst`,
`Affinity`), or weight it (`Weighted`) — all still pure functions of the gossiped
membership+capability set, so every node agrees and the coordinator-free property
holds. Load-aware (non-deterministic) selection is confined to **stateless pools**,
which have no identity to pin. The full model is
[025-Placement-Policies-and-Stateless-Workers.md](025-Placement-Policies-and-Stateless-Workers.md).

## Membership

Cluster membership + failure detection uses an in-house **SWIM**-style protocol
over the `Transport`:

- Gossip-disseminated membership list.
- Randomized ping / indirect-ping failure detection with a suspicion timeout.
- Incarnation numbers to refute false suspicions.
- The smoothed round-trip time these pings already measure is **reused** as the
  transit estimate for cross-node deadline accounting (018) — no separate clock
  handshake.

The membership **view** a node publishes — the *live node set* HRW places over —
is the **non-`Dead`** set: both `Alive` and `Suspect` nodes are placement
candidates. Suspicion is provisional (a suspected node is probably still up and
still routable), so a node is removed from placement only when SWIM declares it
`Dead` after the suspicion timeout lapses with no refutation — **not** on mere
suspicion. Excluding on suspicion would migrate actors off a node on every
transient blip, the very thrash the stabilization window (021) exists to damp.
A `Dead` node is excluded; a refuted node (higher incarnation) stays.

### Alternatives considered

- **External coordinator (etcd / Consul / ZooKeeper)**: strong consistency, but a
  heavy operational + binary dependency and a single bottleneck for placement.
  Rejected for the default; can be layered behind the `Membership` seam if a
  deployment already runs one.
- **Decision: in-house SWIM.** Fully decentralized, no external service,
  implementable over the same `Transport`. Membership changes trigger
  re-placement; convergence is eventual and observable (009).

A membership change **re-places** affected actors: their `ActorId → node` mapping
moves. In-flight `ask`s to a departed node fail and escalate (007); state
migration is a persistence concern (012). The *orchestration* of a change — the
staged join FSM (placement recomputes only once a node can host work), fenced
hand-off of live actors, a **stabilization window** that damps flap-induced
thrash, and graceful drain vs. crash — is
[021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md).

## Transport seam

```cpp
struct Transport {
    virtual void send(NodeId to, MessageFrame frame) = 0;   // fire-and-forget
    virtual void on_receive(std::function<void(MessageFrame)>) = 0;
    // connection lifecycle, backpressure signalling…
};
```

Default implementation: **plain TCP** with length-prefixed frames, one multiplexed
connection per peer, over a **per-OS event loop supplied by the Platform
Abstraction Layer** (019) — epoll/`io_uring` (Linux), `kqueue` (macOS/BSD), IOCP
(Windows). The transport logic is written against the PAL's readiness/completion
interface, not against a specific OS API. No asio, no gRPC.

The **mechanics** that make "one connection per peer" hold — lazy establishment on
first cross-node send, deterministic dial deduplication (lower `NodeId` wins),
SWIM-ping-reused keepalive, and jittered reconnect/teardown — are specified in
[021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md),
along with the `Discovery` seam a fresh node uses to find a first contact before
gossip can take over.

For a cluster crossing an untrusted network, this transport is wrapped by the
mutually-authenticated, encrypted `SecureTransport` of
[020-Security.md](020-Security.md) — the handshake is paid once per peer (one
connection per peer), and node identity established there also gates SWIM
admission and HRW placement, so an unauthenticated node can never be placed onto.

### Alternatives considered

- **gRPC / HTTP2**: rich but pulls protobuf + a large runtime; overkill for
  intra-cluster actor traffic. Optional adapter only.
- **Decision:** minimal length-prefixed framing over TCP in the default transport;
  QUIC/RDMA/io_uring transports can be swapped in behind the seam.

## Serialization seam

Cross-node `tell`/`ask` must turn a message into bytes. The mechanism is the
**single serialization story defined in `016-Serialization.md`** — one
reflection-free `describe`/`QUARK_SERIALIZE` per type, shared with persistence
(012). The core does **not** mandate a serialization library:

```cpp
template<class M> struct Serializer;   // seam; default drives 016's codec
```

- Wire uses 016's **canonical tagged encoding**, with a **transparent tagless fast
  path** negotiated per type at connect: peers exchange schema fingerprints, and
  identical fingerprints unlock a near-memcpy packed encoding; a mismatch (rolling
  upgrade) falls back to the tagged form automatically (see 016).
- Optional adapters (protobuf, FlatBuffers, Cap'n Proto) plug in behind
  `Serializer` for teams that already use them.

Only **remotely-sent** message types need a `Serializer`; a purely local,
never-persisted actor's messages never require one, checked at Validation (008).

## Delivery semantics across the network

| Property | Local (006) | Cross-node |
|---|---|---|
| Ordering | FIFO per (sender, receiver) | FIFO per (sender, receiver) over one connection. Under 026 relay topologies, preserved by deterministic per-digest **path pinning** + drain-boundary promotion — **proven** ([ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md): 0 inversions / 100 trials × 10⁶ arrivals, unpinned control inverts 88–96%). |
| Duplication | At-most-once | Per-actor `Delivery` level (017): at-most-once, at-least-once (retry + dedup by `MessageId`), or effectively-once |
| Failure | Dead-letter / `ask` error | Peer down → `ask` fails & escalates (007); `tell` dead-lettered locally |

Exactly-once is **not** offered as a transport property; where it matters it is
built from at-least-once + idempotent handlers + fenced persistence — the full
mechanism and its partition proof are in `017-Delivery-Guarantees.md`.

## Dependencies

Std + the Platform Abstraction Layer's socket/event-loop backend (epoll·io_uring /
kqueue / IOCP) for the default transport. Everything heavier (gRPC, protobuf,
external coordinators) is an opt-in adapter behind a seam and never linked into a
single-node build.

## Status

**Accepted (x86-64, core)** — the placement + cross-node **FIFO data path** is proven
(HRW/VirtualBins by ADR-006; FIFO-under-relay by ADR-011). The one remaining item that keeps
010 from *full* Accepted is the **cross-node backpressure** design question below — it is a
data-plane flow-control design, not a defect in the proven placement/FIFO core.

## Open questions

- **Cross-node backpressure (the named residual for full acceptance)**: how a remote full
  mailbox (006) signals the sender without head-of-line blocking the shared connection. Needs
  its own gate before 010's backpressure path promotes.
- Split-brain policy under network partition — HRW gives deterministic placement,
  but two partitions may each activate the "same" actor; reconcile via persistence
  fencing tokens (012)?
- Node-identity / certificate revocation propagation in a coordinator-free cluster
  — gossip a revocation list over SWIM vs. short-lived certs (020).
- Whether shard-granularity or actor-granularity is the unit of re-placement on
  membership change. *(Addressed by 026: the **virtual bin** is the re-placement unit
  at scale — a join/leave moves ~1/N bins, quantized and cache-friendly.)*
