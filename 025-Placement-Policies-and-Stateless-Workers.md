# 025 — Placement Policies and Stateless Workers

Two gaps the RFC left open, resolved together because they are the **same axis seen
from both ends**:

1. **Placement has one strategy** — uniform HRW over *all* live nodes (010). A
   developer cannot say "place this actor only where a GPU exists," "prefer the local
   node," "spread these apart," or "weight by node capacity." Placement should be a
   **policy the developer can tune to optimize** for locality, capability, capacity,
   or affinity — more ability, same zero-cost default.
2. **There is no stateless-worker pattern.** The core invariant is *at most one
   activation per actor* + stable `ActorId → node`. A stateless request processor
   (codec, validator, fan-out worker) wants the **opposite**: many activations, load
   balanced, no identity. Today it is unexpressible.

The unifying idea:

> **Placement determinism is the thing being traded.** Everything that is a function
> of the *gossiped membership + capability set* stays deterministic — every node
> computes the same mapping, no coordinator, invariant intact. Only a function of
> *live load* breaks determinism — and that is exactly what a **stateless** actor can
> afford, because it has no identity to pin. So capability/affinity placement extends
> the deterministic world; stateless workers step outside it deliberately.

> **Status: Accepted (x86-64)** — both blocking gates now `CORRECT`.
> [ADR-011](decisions/ADR-011-cluster-relay-and-placement-gate-verification.md) proved the
> **`Stateless<N>` pool relaxation** (exactly-once under concurrency — lost=dup=torn=0 at 10⁷
> msgs, TSan/ASan clean, shared-state control races; throughput 1.5–2.8× > N hand-rolled) and
> caught a real defect in the Weighted path (ADR-006's `weight·H` is non-proportional; the
> `CoV ≤ 0.2` balance threshold is a uniform-only quantization floor). Both are repaired in-spec
> (proportional log-WRH `w/(−ln H)`; load-per-weight balance target).
> [ADR-012](decisions/ADR-012-weighted-hrw-distribution-regate.md) proved the corrected
> form **sits at the multinomial floor** (within 1.9% of the ideal sampler at every N; churn
> exact — 0 bins between unchanged nodes) but returned **INCONCLUSIVE** on a mis-specified band
> (a p99 statistic vs a mean-level closed-form floor, unachievable even by the ideal sampler).
> [ADR-013](decisions/ADR-013-weighted-hrw-distribution-regate-2.md) then re-gated the Weighted
> distribution claim under a **like-for-like, preregistered** band (p99-vs-p99 against an
> independent `mt19937_64` multinomial, band fixed from the MC's own dispersion **before** any
> WRH number was observed) and ruled **CORRECT** — `R(N) ∈ [0.8765, 1.1051]` at every N, both
> mandatory controls firing, sanitizers clean with teeth. With the Stateless half (ADR-011 Gate C)
> and the Weighted-distribution half (ADR-013) both `CORRECT`, **025 is promoted to Accepted
> (x86-64)**.

## Part A — Node capabilities

### The capability model

Each node advertises a set of **static, typed capabilities** at startup — declared in
config (013), carried in the SWIM join payload and gossiped with membership (010,
021). Capabilities are **static for a node's lifetime**: changing them is a rejoin
(rolling restart, 021), never a live mutation. This is what keeps them usable in a
*deterministic* placement function — every node sees the same
`{NodeId → capabilities}` map from the same gossip state.

```cpp
// node config (013)
node.capabilities = {
    Label{"role", "ingest"},
    Label{"zone", "eu-west-1"},
    Flag{"gpu"},
    Scalar{"weight", 2.0},        // relative capacity (see Weighted)
};
```

- **Flags / labels** — booleans and key→value strings, matched by placement
  constraints.
- **`weight`** — a relative-capacity scalar for weighted placement.
- **Live load** (queue depth, CPU) is **not** a capability — it is an observability
  signal (009), consumed *only* by load-aware routing for stateless pools (Part C),
  never by the deterministic hash.

### Why static, gossiped capabilities (self-debate)

- **A coordinator holding node metadata** (etcd/Consul): strong consistency, but
  reintroduces the external service 010 deliberately avoids and a placement
  bottleneck. Rejected for the default; layerable behind the `Membership` seam.
- **Dynamic per-request capability query**: a round-trip on every placement — fatal
  for the hot path.
- **Decision:** capabilities ride the membership gossip that already exists, are
  static per node lifetime, and are therefore available to every node's local
  placement computation with no lookup and no coordinator — the same property that
  makes HRW coordinator-free.

## Part B — Placement policies (deterministic, for stateful actors)

Placement (005) generalizes from a single strategy to a **strategy + optional
modifiers**, all resolved against the gossiped membership+capability set, all
**deterministic** (every node agrees), all zero-cost at runtime (compiled to the
metadata table, 005/008).

```cpp
class GpuDecoder : public Actor<GpuDecoder,
        Sequential,
        Placement<HashById,          // strategy (default)
                  Require<Gpu>,       // hard constraint
                  Prefer<SameZone>>>  // soft preference
{ /* … */ };
```

### Strategies

| Strategy | Meaning |
|---|---|
| **`HashById`** | Stable HRW hash of `ActorId` → node → shard (default, unchanged from 010). |
| `Explicit` | Caller-specified node/shard. |
| `Custom<F>` | User function `F(ActorId, MembershipView) → NodeId` — now given the **capability-annotated** membership view, so a developer can express arbitrary deterministic policy. |

### Modifiers (the developer's optimization levers)

Modifiers **narrow or bias** the candidate node set *before* the strategy chooses,
so the strategy still runs — HRW over the **eligible subset** is still deterministic
and still minimal-reassignment.

| Modifier | Effect | Determinism |
|---|---|---|
| `Require<Cap…>` | Hard constraint — eligible = nodes with all caps. Empty eligible set → placement error (007). | Deterministic |
| `Prefer<Cap…>` | Soft — rank preferred nodes first, fall back if none. | Deterministic |
| `Weighted` | Capacity-weighted HRW using each node's `weight` — bigger nodes host proportionally more. Uses the **proportional log-WRH** score `wₙ / (−ln H(bin, node))` (ADR-011 proved the plain `weightₙ·H` form non-proportional). Distribution matches the proportional-multinomial floor; re-weighting moves only ~the intended share delta (0 bins between *unchanged* nodes). | Deterministic |
| `LocalFirst` | Prefer the calling node if it is eligible — a latency optimization (no network hop). | Deterministic (per caller) |
| `Affinity<A>` | Co-locate with actor `A` (same node) — cache/locality for tightly-coupled actors. | Deterministic |
| `AntiAffinity<A>` | Place away from `A` — fault isolation / spread. | Deterministic |

Modifiers **compose** (`Require<Gpu>, Prefer<SameZone>, Weighted`). The compile-time
rule: modifiers filter/rank; exactly one strategy chooses from the survivors.

### The determinism invariant (load-bearing)

> A placement policy for a **stateful** actor MUST be a pure function of `(ActorId,
> gossiped membership+capability set)`. It may **not** read live load. This preserves
> core invariant 4 (stable `ActorId → node` for a fixed membership) — the whole
> reason placement needs no coordinator. Every modifier above obeys this; `Custom<F>`
> is handed only the membership view, never a load signal, and this is enforced by
> the type of `F`.

Membership change re-places affected actors exactly as today (010): the eligible set
shifts, HRW moves only the affected keys, fenced hand-off (021) carries live state.

## Part C — Stateless workers

A `Stateless` actor **deliberately opts out** of the single-activation world to
become a **load-balanced pool of activations**. This is the one place the RFC relaxes
core invariants, so the relaxation is spelled out exactly.

```cpp
class JsonValidator : public Actor<JsonValidator, Stateless<8>> {
    std::expected<Report, Error> handle(const Doc& d);   // no persistent state
};
```

`Stateless<N>` = **up to N activations per node**, routed **locally** (no network
hop), least-loaded / round-robin. It is both an execution policy and a placement
policy: there is no `ActorId → node` pin because there is no identity to pin.

### What it relaxes, and what it keeps

| Invariant | Stateless behavior |
|---|---|
| At-most-one activation per actor | **RELAXED** — N activations, that is the point. |
| Stable `ActorId → node` placement | **RELAXED** — routed to *an* activation, not *the* one. |
| FIFO per (sender, receiver) | **RELAXED** — consecutive messages may hit different activations; **no ordering guarantee to a pool**. A developer choosing `Stateless` accepts this. |
| Persistence / effectively-once (012/017) | **N/A** — a stateless actor has no durable state; at-most/at-least-once only. |
| **Single-executor *per activation*** | **KEPT** — each pool activation is still sequential (or `Reentrant` if declared); the per-activation mailbox and drain are unchanged (001/002/003). |

So `Stateless` does not weaken the *engine* — it declares that *this actor* holds no
cross-message state and needs no identity, and in exchange gets fan-out. The
guarantee "one executor touches one activation's state at a time" still holds; there
is simply no *shared* state across the pool to protect.

### Routing and load-awareness

Because a stateless pool has **no identity**, its routing MAY read **live load** (the
thing Part B forbids for stateful actors) — this is safe precisely because there is
no stable-placement invariant to violate:

- **Local-first, least-loaded**: `tell`/`ask` to a `Stateless` actor picks the
  least-loaded local activation (queue depth from 009), spawning up to `N` on demand.
- **Cluster-wide variant** (`Stateless<N, ClusterWide>`): eligible-node selection
  (Part B modifiers apply — e.g. `Stateless<N, ClusterWide, Require<Gpu>>`) then
  least-loaded among eligible. Cross-node routing prefers local to avoid a round-trip;
  the load signal is gossiped and therefore approximate.

### Why a pool, not "just many actors with different keys"

A developer *could* emulate a pool by sending to `Worker/0…Worker/N-1` round-robin.
Rejected as the sanctioned pattern: it forces the developer to own N, the load
balancing, and the failure handling, and it pins each shard-key to a node (Part B),
defeating local fan-out. `Stateless<N>` makes the pool a **declared intent** the
engine sizes, routes, and heals — the same philosophy as the rest of 005.

### Failure and lifecycle

A stateless activation that faults is simply **discarded and the message
dead-lettered** (007) — there is no state to reconstruct, no `Restart` quiesce (015),
no fencing (017). The pool re-grows on demand. `IdleTimeout` shrinks it; the pool
size `N` is a **bounded resource** (022) and a natural fair-share unit.

### Broadcast is orthogonal to the pool (ADR-019)

`Stateless<N>` is fan-**to-one**: a `tell`/`ask` routes to *one* least-loaded pool activation.
The `Topic<M>` best-effort broadcast
([ADR-019](decisions/ADR-019-best-effort-broadcast-publish-primitive.md)) is the opposite axis
— fan-**to-many**. A stateless pool worker **MAY be a subscriber** on a topic, but `publish`
does **not** route to a single pool member: the two mechanisms are orthogonal and do not
compose into "broadcast picks one worker."

## Interaction

| Spec | Interaction |
|---|---|
| 001 | Core invariant 1 gains an explicit exception: single-activation is per **stateful** actor; `Stateless` is a declared, per-activation-sequential pool. |
| 002 | Local routing to the least-loaded pool activation reuses shard/worker machinery; each activation drains normally. |
| 005 | New policies: `Stateless<N>`; placement strategies extended with modifiers (`Require`/`Prefer`/`Weighted`/`LocalFirst`/`Affinity`/`AntiAffinity`). Validated at startup (conflicts: `Stateless` + `Placement<Explicit>`, `Stateless` + persistence, empty `Require` set at config time). |
| 006 | Sending to a `Stateless` actor is a send **to the pool** — no per-key FIFO, no identity; `ActorRef` to a stateless type resolves to the pool, not an activation. |
| 009 | Per-node pool depth + activation count are metrics and the **load signal** for routing; capability/eligibility is observable. |
| 010 | Membership gossip carries node capabilities; placement runs against the eligible subset; determinism invariant preserved. |
| 012 / 017 | `Stateless` actors are non-durable by construction — persistence and effectively-once do not apply (validated). |
| 013 | Node capabilities are node config. |
| 021 | A joining node advertises capabilities; a capability change is a rolling restart, not a live mutation. |
| 022 | Pool size `N` and per-node activation budgets are governed resources; stateless pools are a natural fair-share unit. |

## Self-debate

### Does capability placement reintroduce a coordinator? No.

The temptation is a service that "knows" which nodes have what. But capabilities are
*static* and ride the *existing* gossip, so every node computes eligibility locally
from the same membership state — identical to how HRW is already coordinator-free. A
coordinator is only needed for *strong-consistency* placement, which the RFC already
declined (010).

### Load-aware placement for stateful actors — deliberately refused.

It is tempting to migrate a hot stateful actor to a less-loaded node. Refused: it
breaks the stable-placement invariant, forces a directory (to find where the actor
*actually* is), and fights the coordinator-free property. Load-awareness is confined
to `Stateless` pools, which have no identity to relocate. Stateful actors move only on
**membership change**, never on load.

### Is `Stateless` a hole in the safety story? No — it is a narrowing.

`Stateless` does not let two executors touch one activation's state; it declares there
is *no shared state* across activations. Each activation is still single-executor. The
data-race surface is unchanged; what changes is the *addressing* semantics (pool, not
identity) and the *ordering* guarantee (none), both of which the developer opts into
explicitly and the validator enforces (no persistence, no per-key FIFO expectation).

## Non-goals

- **Autoscaling the pool or the cluster.** `Stateless<N>` bounds a per-node pool;
  *when* to change `N` or add nodes is the external control loop over 009 (022), not
  the engine.
- **Perfect global load balancing.** Cluster-wide stateless routing is
  approximate (gossiped load), local-first to avoid round-trips.
- **Live-migrating stateful actors for load.** Out — stateful placement is stable and
  moves only on membership change.
- **Dynamic per-node capabilities.** Capabilities are static per node lifetime; a
  change is a rejoin.

## Open questions

- *(**Prove the `Stateless` relaxation**: **RESOLVED** — ADR-011 Gate C proved pool routing
  concurrency-safe (exactly-once, TSan/ASan clean, shared-state control races) and fan-out
  1.5–2.8× faster than N hand-rolled actors.)*
- **Pool sizing**: static `N` vs. adaptive-to-load (adaptive interacts with 022 and
  the oscillation concern raised in 024).
- **Load-signal freshness**: gossiped load for cluster-wide routing is stale by one
  gossip round; how stale before routing decisions are actively bad?
- **Affinity across re-placement**: if `Affinity<A>`'s target `A` moves on a
  membership change, does the affine actor follow atomically, and at what cost?
- **Capability constraint feasibility at startup**: `Require<Cap>` with no eligible
  node in the initial cluster — hard startup error (Strict) vs. deferred-placement
  until a capable node joins (Relaxed)?
- **Weighted-HRW distribution — the one gate blocking promotion (ADR-011).** Gate B measured
  it and found: **churn is bounded and correct** (log-WRH tracks the intended share delta, 0
  bins move between *unchanged* nodes; a modulo control reshuffles 97.5–99.9%), and the
  distribution is **proportional** (share error 3–5σ) — but the *absolute* balance inequality
  `CoV ≤ 0.2 ∧ max/mean ≤ 1.5` **fails at N ∈ {1024, 4096}**, because it is a proportional
  **quantization floor** (16 bins/node → a light node gets ~4 bins), not a defect of the
  scheme. Two repairs are applied above: (1) the **proportional log-WRH formula** `w/(−ln H)`
  replaces ADR-006's non-proportional `weight·H`; (2) the **weighted-balance target is restated
  on load-per-weight** (ρ = realized/weight vs the multinomial floor), with the uniform
  `CoV ≤ 0.2` scoped to unweighted placement — a weighted deployment wanting tight absolute
  balance sizes B larger. **Remaining step:** re-run Gate B against the corrected formula +
  restated target; a CORRECT verdict promotes 025 to Accepted (x86-64).

## Interaction with large-scale placement (026)

The O(1) `VirtualBins` cache of
[026-Large-Scale-Cluster-Topology.md](026-Large-Scale-Cluster-Topology.md)
accelerates **only the unconstrained `HashById`** path. Constrained/optimizing
placement composes with it as follows: `Require`/`Prefer`/`Weighted`/`Affinity`
resolve against a **per-eligibility-class bin table** or fall back to O(eligible) ≤ N;
**`AntiAffinity` and per-actor spread must bypass the bin cache** and compute exact
per-actor HRW, because bins *quantize* placement and would collapse a deliberate
spread.
