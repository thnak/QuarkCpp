# 026 — Large-Scale Cluster Topology

How Quark scales to **very large clusters** (hundreds to thousands of nodes) without
a coordinator and without abandoning the settled distribution seams (010). The
single-node and small-cluster paths are unchanged; everything here is **opt-in
policy** that a flat cluster never pays for.

> **Design pinned by [ADR-006](decisions/ADR-006-large-scale-cluster-topology.md)** —
> the `VirtualBins + Bounded Partial-View + DHT-Relay` design (D3), which swept 9/9
> claims under executed C++ (GCC 14.2 + Clang 20.1) and upholds the distribution
> determinism invariant literally.
>
> **Status: Draft.** The steady placement/routing path is proven. **One Hard gate**
> blocks promotion: cross-node **FIFO under variable-hop relay** (see *Open
> questions*). The runner-up (D2 Cells) is retained as the opt-in tier beyond D3's
> honest ~10⁴-node wall.

## The bottlenecks that only bite at scale

Small-cluster distribution (010) is correct but has four costs that grow with N:

1. **HRW placement is O(nodes) per lookup** — at N ≥ 1000 this is a per-message cost.
2. **Full membership at every node** — O(N) memory + O(N) gossip dissemination.
3. **One connection per peer** — O(N) sockets/node, **O(N²) cluster-wide**.
4. **A single join/leave** re-places and recomputes at every node.

026 addresses each with a *configurable* mechanism, none of which a flat cluster
instantiates.

## Three configurable axes

Topology is not one decision but three orthogonal policies (selected via config, 013;
validated at startup, 008):

| Axis | Values | Default (small) | Default (N ≥ 512) | Opt-in (> ~10⁴) |
|---|---|---|---|---|
| `topology` | `Flat` · `Partitioned` · `PartialView` | **`Flat`** | **`PartialView`** | `Partitioned<GroupByLabel<zone>>` |
| `connections` | `FullMesh` · `Gateway` · `BoundedPartialView` | **`FullMesh`** | **`BoundedPartialView`** | `Gateway` |
| `placement_cache` | `Direct` · `VirtualBins` · `Resolved` | **`Direct`** | **`VirtualBins`** | `Resolved` (per-group) |

> **Zero-cost when unused.** `Flat / FullMesh / Direct` instantiates **none** of the
> bins, partial-view, or relay machinery — a small cluster pays nothing. The
> large-scale path is a compile+config selection, not a runtime branch on the hot
> path.

## VirtualBins — O(1) placement (the recommended large-N default)

Placement stops being an O(N) argmax and becomes an **O(1) table read**:

```cpp
bin      = splitmix64(actor_id) & (B - 1);       // B = next_pow2(16 * max_nodes)
owner    = bin_table[bin];                        // one load — 5–6 ns, N-independent
```

- The `bin_table` maps each of **B fixed virtual bins → owning node**, refilled
  **once per epoch** by per-bin HRW on the membership thread (a control-plane cost,
  never on the drain path).
- **B ≥ 16 × max_nodes** (validated) keeps bin balance tight (CoV ≤ 0.2, max/mean ≤
  1.5); a cluster provisioned near N = B collapses balance, hence the bound. **This
  threshold is for *uniform* (unweighted) placement** — ADR-011 proved it is a proportional
  **quantization floor** that *weighted* placement cannot meet at N = max_nodes with 16
  bins/node and min:max ≥ 8 weights. Under `Weighted` (025), the target is restated on
  load-per-weight terms (ρ = realized/weight, matched to the proportional-multinomial floor
  `√((W/(N·B))·Σ 1/wᵢ)`); a weighted deployment that wants tight absolute balance must size
  **B larger** (bins/node ≫ 16). See 025 and ADR-011.
- **Determinism is literal and content-addressed:** the bin table's identity is a
  **roster content-digest**, not a bare epoch counter — every node with the same
  membership *content* computes a byte-identical table (proven over 4096 bins × 10⁵
  ids). Two concurrent disjoint joins transiently yield different digests; that window
  is bounded by O(log N) gossip and covered for stateful actors by 021 handoff/fencing,
  not by placement.
- **Reclamation** of superseded bin tables is **QSBR** (sentinel-initialized per-worker
  beacons, reclaim at the min over registered workers, park/unpark reload,
  `alignas(64)`) — proven TSan/ASan clean with bounded RSS.

Bins **quantize** placement, so they accelerate only the **unconstrained `HashById`**
path. `Require`/`Prefer`/`Weighted`/`Affinity` (025) resolve against a
per-eligibility-class bin table or fall back to O(eligible) ≤ N; **`AntiAffinity` /
per-actor spread must bypass the cache** and compute exact per-actor HRW.

## Bounded Partial-View — O(log N) memory & sockets

A node no longer holds the full membership or a socket per peer:

- **Active view** (~`c·log(max_nodes)` peers) — direct SecureTransport connections.
- **Passive view** — a larger backup set for repair, no sockets held.
- Peers reachable only indirectly are probed via **SWIM indirect (k-relay) probing**
  (021); an optional **connectionless-datagram SWIM control plane** is a distinct
  opt-in that must re-supply 018 RTT, keepalive, and **per-datagram AEAD** (020) — not
  free.
- Cluster sockets drop from O(N²) to **O(N log N)**.

## DHT-Relay — reaching a non-peer

A cross-node send whose target isn't in the active view routes via a **maintained
Kademlia** table (kept **separate** from the gossip view, with empty-bucket fallback):

- Delivery completes within **≤⌈log₂N⌉ hops** (proven 100% within the bound;
  unmaintained views dead-end 46–95%).
- **Hot-peer promotion** upgrades a repeated destination to a direct connection, so
  steady traffic pays the relay tax **once**, not per message.
- Cold or uniform all-to-all traffic pays up to ⌈log₂N⌉ extra RTTs (12 @ N=4096) —
  such workloads should select `FullMesh` or a larger `relay_cap`.

## Cross-node FIFO under relay (PROVEN — was the promotion gate)

FIFO per (sender, receiver) is a core guarantee (006). Under a fixed `FullMesh`
connection it holds trivially ("over one connection"). Under relay topologies a
message stream can change path mid-flight (a hop is promoted, a bucket repairs),
which could reorder. The discipline:

- **Deterministic per-digest path pinning** — a stream's path is a function of the
  roster digest, so it changes only at a digest transition;
- **Promotion only at a drain boundary** — a path change is applied between messages,
  never mid-stream.

**Proven** by [ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md)
(the named monotonic-sequence-through-a-mid-stream-path-change experiment). A faithful
ADR-006 relay (VirtualBins placement, bounded partial view, greedy Kademlia-XOR relay,
epoch-carried per-digest path pinning, drain-boundary promotion) delivered **0 inversions
across 100 trials × 10⁶ arrivals/cell** under both GCC 14.2 and Clang 20.1 (zero-tolerance),
with real cross-thread hops (hop-vector 2–6 → 1 at the switch, in-flight ≥ 1 at the switch
in 100/100 trials). The **mandatory unpinned control inverted 88–96%** of valid-race trials
(137/168 inversions); null baselines (switch disabled) were 0, so inversions are attributable
solely to the path change. TSan/ASan/UBSan clean (a deliberate-race control tripped TSan —
teeth proven). This was the **Draft→Accepted Hard gate**; it is met, and 026 is **Accepted
(x86-64)**.

## The D2 Partitioned tier (beyond ~10⁴ nodes)

D3's roster stays **O(N)** (~160 KB @ N=4096) because global determinism needs the
node *set*. That is honest to ~10⁴ nodes. Beyond it, `Partitioned<GroupByLabel<zone>>`
(the proven D2 design) trades **global determinism for per-group determinism**:
`ActorId → group → node`, per-group full membership + sparse inter-group summary
gossip, `Gateway` connections across groups. The caveat is explicit and load-bearing:
**a remote node cannot locally compute `ActorId → node`** across groups — it resolves
to the group and delegates to a gateway. This is invariant 4 held at *group*
granularity, the deliberate price of O(√N) memory.

## Honest bounds (record, don't hide)

| Quantity | D3 bound |
|---|---|
| Placement lookup | **O(1)**, N-independent (5–6 ns) |
| Per-node sockets | O(log N) |
| Gossip convergence | O(log N) rounds |
| Roster memory | **O(N) entries** (~160 KB @ 4096) — the wall |
| Non-peer send | ≤⌈log₂N⌉ relay RTTs |

## Configuration (013)

The 026 knobs are **operational** — they change *speed and path*, never *which node
owns an actor*:

```cpp
cfg.cluster({
    .topology       = Topology::PartialView,   // Flat | Partitioned | PartialView
    .connections    = Conn::BoundedPartialView, // FullMesh | Gateway | BoundedPartialView
    .placement_cache= Cache::VirtualBins,       // Direct | VirtualBins | Resolved
    .max_nodes      = 4096,                      // sizes B = next_pow2(16*max_nodes)
    .active_view    = 0,                         // 0 = c*log(max_nodes)
    .relay_cap      = 0,                         // 0 = ceil(log2 max_nodes)
    .group_label    = "zone",                    // Partitioned only
    .gateways_per_group = 3,                     // Partitioned only
});
```

Validation (008): `B ≥ 16·max_nodes`; `active_view ≈ c·log(max_nodes)`; seeds present
iff distribution is enabled; and **config may not override an invariant** — no
non-deterministic placement for stateful actors, no FIFO removal.

## Interaction

| Spec | Interaction |
|---|---|
| 010 | Replaces the O(N)-per-placement / "cached ordering" hand-wave; adds the connections axis. |
| 021 | Bulk join is **batched admission** (one bin-table sweep for M joiners) — serial per-event repair blows the budget; stabilization window ties to roster-digest transitions (the FIFO path-change boundary). |
| 025 | The bin cache accelerates only unconstrained `HashById`; constrained/affinity placement bypasses or uses per-class tables. |
| 020 | 026 topologies keep SWIM over authenticated `SecureTransport`; relays/gateways are same-trust-anchor peers but concentrate traffic → the DoS/hotspot target (provision + rate-limit, 022); datagram SWIM needs per-datagram AEAD. |
| 009 | Per-node control-plane metrics: cache hit/miss + cold-recompute, roster-digest gauge, gossip rounds, relay-hop histogram + ttl dead-letters, open-socket gauge vs bound. |
| 013 | The knobs above. |
| 023 | Placement-lookup budget (Hard p99 ≤50 ns, Goal ≤20 ns, N-independent); membership-repair budget (≤2 ms single join/leave); the FIFO-under-relay Hard gate; bin-balance budget. |

## Self-debate

### Why not just cache the flat HRW ordering (D1)?

D1 kept flat HRW + full membership with an incremental-repair cache. It posted the
*fastest raw route* (2.98 ns) but **missed two budgets**: membership repair (2.95–4.06
ms vs ≤2 ms) and load balance (max/mean 1.53–1.61 > 1.5 even at high balance
constants), plus an inconclusive safety claim. Fast common case, but it does not hold
the tail or the repair budget at scale — and it still carries O(N) memory and
O(N²) connections. Rejected as the large-N default; its lazy-establishment +
idle-teardown ideas fold into `FullMesh` for small clusters.

### Why bins over per-shard recompute-caches (D2's hot path)?

D2's warm cache hit (8.4 ns) is close, but its **cold miss recomputes at O(√N) → p99
up to 3600 ns** — a tail cliff in the common working-set-churn case. VirtualBins is
**unconditionally O(1)** with no cliff. D2's memory win is real and kept for the
>10⁴-node tier, but not on the hot path at moderate scale.

### Why keep it coordinator-free?

A coordinator (etcd/Consul) would make placement a lookup and simplify membership —
but reintroduces the external dependency and bottleneck 010 declined, and a
content-addressed bin table already gives every node the same answer with no service.
A coordinator remains layerable behind the `Membership` seam for deployments that
already run one.

## Non-goals

- **Not a coordinator** — determinism comes from content-addressed gossip, not a
  registry.
- **Not beyond ~10⁴ nodes on D3 alone** — that regime is the D2 Partitioned tier,
  explicitly opt-in with its group-granularity caveat.
- **Not autoscaling** — *when* to add nodes is the external control loop (022); 026 is
  *how the cluster is shaped* once they exist.
- **Not a new wire format** — 010/016 own the bytes; 026 owns placement/topology.

## Open questions

- *(Cross-node FIFO under variable-hop relay: **RESOLVED** — proven by ADR-011, the
  Draft→Accepted gate is met; see "Cross-node FIFO under relay" above.)*
- **Weighted-HRW + virtual bins** — **RESOLVED** (ADR-013, via 025's re-gate). The
  proportional log-WRH form `score = w/(−ln H)` on load-per-weight ρ was re-gated **CORRECT**
  under a like-for-like preregistered p99-vs-p99 band against an independent multinomial:
  `R(N) ∈ [0.8765, 1.1051]` at every N (sits at the multinomial floor), re-weight churn exact
  (0 bins move between *unchanged* nodes, non-vacuous), both mandatory controls fired (modulo
  reshard ≥0.98; non-proportional `w·H` misses 4–26×). The two earlier follow-ups are closed:
  (1) the `weight·H` formula is the log-WRH form (ADR-006 line 104 carries the correction note);
  (2) the `CoV ≤ 0.2` threshold is a uniform-only floor and the weighted target is the
  load-per-weight ρ band that ADR-013 measured against. A weighted deployment wanting tight
  *absolute* balance still sizes B larger (bins/node ≫ 16).
- **Churn-window dead-ends** — stale k-buckets can dead-end → ttl → dead-letter until
  convergence; bounded by the 021 stabilization window + passive-view repair, not
  eliminated. How large a window is acceptable?
- **Datagram SWIM control plane** — the connectionless opt-in must re-supply RTT,
  keepalive, and per-datagram AEAD; is the failure-detector latency win worth that
  cost vs. reusing the authenticated connection?
- **The ~10⁴ roster wall** — is a hybrid (D3 within a region, D2 across regions) worth
  specifying, or is the clean `Partitioned` cutover enough?
