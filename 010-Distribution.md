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
Certificate rotation and compromise-driven revocation of an already-admitted
node's session are handled separately, without a re-placement or a
membership-epoch change — see that same file's "Certificate rotation and
revocation on a live cluster".

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

`SecureTransport` (020, ADR-040)'s per-peer session state — the directional
cipher pair, sequence counter, replay high-water mark, key `generation`, and
the peer's retained certificate fingerprint — is a `PeerSession`, folded into
this transport's existing per-peer lock/map, not a parallel one. This carries
a load-bearing ordering invariant, with its own adversarial test (a
`yield()`-hook-injected concurrent-sender + concurrent-rekey stress, per
CLAUDE.md's "a load-bearing invariant without a test … is not done"):
**sequence-number assignment, AEAD seal, and the hand-off to the wire
transport for one peer's session must be ordered atomically with respect to
other same-session senders.** Assigning `seq` without serializing it through
to wire submission permits wire-arrival order to diverge from `seq` order
under concurrent same-session senders, causing genuine, non-replayed frames
to be spuriously rejected by the strict-monotonic replay guard. The lock this
requires is scoped to one session's own mutex, never shared cluster-wide (see
020-Security.md's "Rotation and revocation invariants") — one peer's crypto
work never blocks another peer's session.

### Alternatives considered

- **gRPC / HTTP2**: rich but pulls protobuf + a large runtime; overkill for
  intra-cluster actor traffic. Optional adapter only.
- **Decision:** minimal length-prefixed framing over TCP in the default transport;
  QUIC/RDMA/io_uring transports can be swapped in behind the seam.

### Sibling seams

Not every byte a node moves is a `Transport` frame. `VoiceChannel`
([028-Voice-Datagram-Channel.md](028-Voice-Datagram-Channel.md)) — best-effort,
unordered, unreliable UDP for real-time voice/media — is a **NEW, PARALLEL seam**,
not a `MessageFrame`/`FrameKind` variant and not a `Transport` decorator. It shares
**no** code path with `Transport`/`MessageFrame`, and it is never reachable through
the actor mailbox/`GovernanceCore` (003) — a deployment that doesn't use voice pays
nothing for it. Do not assume voice traffic is "just another frame kind" flowing
through this seam; it deliberately opts out of the ordering guarantee this section
exists to provide.

What it **does** share: the node's I/O reactor. `TcpTransport::event_loop()` is the
**one sanctioned extension point** for a sibling seam that needs to share the
node's already-open `pal::IoContext` instead of standing up a second thread/loop/
fd-set — `VoiceChannel` is its first (and, by design, only intended) consumer. No
other public surface should be added to `TcpTransport` for *reactor sharing*; a
future sibling seam should extend `event_loop()`'s usage, not grow a second
accessor.

A second, narrower exception exists for a different purpose: `TcpTransport::
reset_peer_connection(NodeId)` (020, ADR-040) lets `SecureTransport` force-drop
the live socket to a peer after a failed certificate renegotiation, WITHOUT
declaring the peer dead — unlike `close_peer()` (021's permanent-death
teardown, which suppresses reconnect), it leaves reconnect-eligibility
untouched, so the reconnect/backoff machinery
[021-Cluster-Formation-and-Lifecycle.md](021-Cluster-Formation-and-Lifecycle.md)
already specifies immediately redials, and a fresh mTLS handshake carries a
new generation. This is the one sanctioned exception to "no other public
surface," scoped narrowly to that one purpose — it is not a general-purpose
connection-control API, and a future sibling seam should not extend it for
anything else.

### Capability gossip is control-plane, not data-plane (025, ADR-045)

Node capability dissemination (`CapabilityRegistry`, capability_registry.hpp) lives
entirely inside `SwimMembership::tick()`/`send_control()`/`handle_control()` on the
protocol thread — the SAME `FrameKind::Control` frames that already carry membership
`updates` and ADR-040 `revocations` (`ControlMsg.capabilities`), never a
`Transport::send`-visible data-plane frame and never reachable from
mailbox/dispatch/activation headers. A deployment that never calls
`set_capability_gossip` pays nothing for it; placement policies that DO read a
`CapabilityView` measured a read-cost delta within noise (-4% to -5%) of the
pre-existing std-only test double (ADR-045).

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

**Principal propagation across the network (ADR-044).** `MessageFrame` carries a
`Principal` (020 §3), stamped from the sender's ambient context at the wire edge.
`DistributedRouter::deliver` must thread this field through `inbound_thunk` into
`LocalRouter::deliver_from_wire` — an earlier integration gap silently dropped it
here, which ADR-044's evidence run found and fixed. A non-anonymous principal is
re-established as the receiving actor's ambient `current_context().principal` at
all four real claim/dispatch sites (`020` §3 names them); an anonymous one (the
default absent a security config) costs nothing extra on the receive path.

## Cross-node reply-stream credit-return (ADR-018)

An `ask` that returns a **stream** across nodes runs the 024 credit-ring backward
(callee = producer, caller = consumer; ADR-018). Reply-direction credit is returned to
the remote callee by an **edge-triggered** `CreditReturn{stream_id, tail}` frame carrying
the **absolute** caller `tail`, applied `shadow_tail = max(shadow_tail, tail)`:

- **Monotone max-merge** makes the return **reorder- and duplicate-safe** — a stale or
  replayed `CreditReturn` can never retract credit (an *additive* delta could). A low-rate
  **tail heartbeat** carries the same absolute tail so a dropped *final* `CreditReturn`
  never wedges the callee with a full window.
- `stream_id` is a **process-monotonic nonce** (not a reused index), so the transport's
  `stream_id → ring*` map has **no ABA**. The receive path **gen-gates before any write**
  to ring memory: a frame for a torn-down or reincarnated stream is dropped, never applied.

This is the reply-direction dual of, and **composes with**, the cross-node backpressure
open question below (a remote full mailbox is a *producer stall* via the credit window,
not head-of-line blocking on the shared connection).

## Cross-node broadcast fan-out (Draft — ADR-019)

A `Topic<M>` best-effort broadcast
([ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)) that spans nodes
fans out over this seam by **coalescing remote subscribers by node**: the publisher emits
**one fire-and-forget frame per distinct subscriber-node**, not one per remote subscriber, so
amplification is bounded by **#nodes, not #remote-subs**. A `Suspect`/`Dead` or otherwise
unreachable node is **dropped without stalling the publisher** — this composes with SWIM
membership (above) and the stabilization window (021) exactly as an ordinary cross-node `tell`
does.

The bound is on *fan-out*, not on publisher work: the **synchronous per-node ADR-016 encode**
is publisher CPU, so **publisher latency rises linearly in the number of distinct alive
nodes**. That linear cost *is* the bounded amplification (one encode per node), **not** a
stall — GATE 1 (publisher never blocks) still holds. Per-node coalescing also makes loss
**coarse**: one dropped frame loses the publish for every subscriber on that node.

**Status: Draft.** Amplification + dead-node no-stall are proven only on an in-process
simulated transport (x86-TSO, ADR-019 GATE 7). Promotion is gated on a **real-transport**
re-proof and the **ADR-011 path-pinned relay-tree FIFO** re-gate (the relay-tree variant is in
026).

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
- *(Split-brain policy under network partition: resolved for `EffectivelyOnce` actors —
  HRW gives deterministic placement, but two partitions may still each activate the "same"
  actor; the store accepts only the higher fencing token at commit, so the zombie
  activation's commit (and its transactional outbox) is rejected, producing no state and no
  output. See the partition worked example in `017-Delivery-Guarantees.md`. Actors that don't
  opt into `EffectivelyOnce` accept this as the documented CP/AP tradeoff, not a gap.)*
- Node-identity / certificate revocation propagation in a coordinator-free cluster
  — gossip a revocation list over SWIM vs. short-lived certs (020). *(Same open item as
  020-Security.md's "Certificate/identity revocation" — not yet resolved in either spec.)*
- Whether shard-granularity or actor-granularity is the unit of re-placement on
  membership change. *(Addressed by 026: the **virtual bin** is the re-placement unit
  at scale — a join/leave moves ~1/N bins, quantized and cache-friendly.)*
