# ADR-006 — Large-Scale Cluster Topology (026)

- **Status:** Accepted (design selected; 026 to be drafted, then promoted after the one open Hard-gate experiment below passes)
- **Date:** 2026-07-15
- **Supersedes / extends:** none (new distribution seam work on top of 010/021/025)
- **Debate artifacts:** 3 designs, cross-examined, proven with compiled+executed C++ (g++ 14.2.0, clang++ 20.1.2, x86-64 Linux, TSC ~2.095 GHz host — below the 023 3–4 GHz reference, so absolute ns are host-local; scaling *shapes* are hardware-independent).

## Question

How does Quark's distribution layer scale to very large clusters (hundreds to thousands of nodes) **coordinator-free**, without abandoning the settled seams (010 Transport/Serializer/Membership, HRW/Rendezvous placement, in-house SWIM), fixing the four scale bottlenecks:

1. HRW placement is O(nodes)/lookup → a per-message cost at N≥1000.
2. Full membership at every node is O(N) memory + O(N) gossip.
3. One connection/peer is O(N) sockets/node, O(N²) cluster-wide.
4. One join/leave re-places/re-computes at every node (blast radius).

Constraints (gates): coordinator-free by default; deterministic + stable placement for stateful actors (invariant 4); HRW minimal-reassignment preserved; only the three seams extend the core; std-only C++23; **configurable via policy** with a flat/small cluster paying **nothing** (zero-cost-when-unused).

## The three designs (one line each)

- **D1 — FLAT-CACHED** (`bucketed HRW cache + bounded lazy mesh`): keep flat HRW + full SWIM; quantize the keyspace into a fixed `winner[]` bucket table (pinned `B = next_pow2(kBalanceC·max_nodes)`), maintained incrementally by a **single SWIM-thread writer** with release stores → wait-free, RMW-free O(1) routing. SWIM datagram control plane + LazyBounded data mesh cap sockets. Full O(N) membership kept, honest to ~2–5k nodes.
- **D2 — Cells** (`two-level HRW + gateway-relay + per-shard cache`): partition by a gossiped `zone` Label. `group(ActorId)` = HRW over O(G) group digests; `node(ActorId)` = HRW within the owning group's full membership (O(N/G)); remote senders **delegate the last hop to the destination group's gateway**. Per-node state O(N/G local + G digests) = **O(√N)**; connections full-mesh intra-group + gateway-relay cross-group = O(√N).
- **D3 — VirtualBins + Bounded Partial-View + DHT-Relay** (WINNER): decouple the three O(N) costs. Placement COMPUTE → fixed `bin_owner[B]` table (O(1) load, refilled once/epoch). SOCKETS → HyParView-style bounded partial view (active c·log N + passive) with Kademlia k-bucket **relay** to non-peer owners (≤⌈log₂N⌉ hops). GOSSIP → epidemic over the active view (O(log N) rounds). Roster kept O(N) *entries* (tiny, ~160 KB @ N=4096, delta-gossiped) because global HRW determinism genuinely needs the node SET. Snapshot published behind one `atomic<Snapshot*>` (acquire/release + QSBR reclaim).

All three express the resolution as the **same three configurable axes**, selected by 013 config (operational — they change speed/path, never *which* node owns an actor, so per the 013 boundary they are config, not per-actor 005 policy):
`topology = Flat | Partitioned | PartialView`, `connections = FullMesh | Gateway | LazyBounded/BoundedPartialView`, `placement_cache = Direct | Bucketed/VirtualBins | Resolved`.
Flat + FullMesh + Direct is the zero-cost-when-unused default that compiles to exact 010 single-level HRW.

## Evidence table (claim → survived red-team? → proven by executed C++? → measured number)

Kinds: `f`=fast/hot-path, `s`=safe, `c`=correct. Verdicts are the prover's executed results.

### D1 — FLAT-CACHED

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 O(1) route | yes | **CORRECT** | route p50 2.98/3.51/3.82/5.81 ns @ N=64/256/1024/4096; p99 ≤18 ns; **0 lock-RMW** (objdump, both compilers); Direct-HRW p50 253→11287 ns (~1900× slower @4096) |
| F2 ≤2 ms repair, deferred refill | yes | **WRONG** | on_join 2.95 ms / on_leave 4.06 ms mean @ N=4096,B=262144 (>2 ms Hard-adjacent); 365/1000 leaves forced a synchronous O(N) salt-scan (deferred-refill guarantee violated); multi-class 556 ms @20 classes, bulk-100 = 671 ms |
| S1 wait-free RMW-free routing under churn | yes | **CORRECT** | TSan+ASan/UBSan clean both compilers; ~1.8 B reads, 0 races/UAF; uint16 idx never overflowed over 80,392 incarnations (EBR recycle); (max_nodes+1)th admission rejected |
| S2 socket cap + SWIM detection | yes | **INCONCLUSIVE** | socket-cap PROVEN (open FDs ≤64 over 674,712 dials, re-dial delivers); SWIM indirect-detection is only a structural model, not a network proof → no weight |
| C1 determinism | yes | **CORRECT** | 8 nodes byte-identical `winner[]`; B-pinning removes live-N divergence |
| C2 minimal-reassignment @ bucket granularity | yes | **CORRECT** | moved·N = 1.000 (join)/1.001 (leave); 0 wrong-bucket moves; grow-N control 1.597 (not the 0.5·N rebucket) |
| C3 balance CoV≤0.2 ∧ max/mean≤1.5 | yes | **WRONG** | @ kBalanceC=64: CoV p99 ≤0.152 (holds) but **max/mean p99 1.45/1.53/1.58/1.61 > 1.5** for N≥256; needs kBalanceC≥128 (another 2× memory) |

Net: **4 proven, 2 disproven (F2, C3), 1 inconclusive.**

### D2 — Cells

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 warm O(1) / cold O(√N) | yes | **CORRECT** | warm-hit flat 8.4 ns (g++) / 15–16 ns (clang) across G=8..64, ≤30 ns, N-independent; cold p99 553→3600 ns (√-scaling); SetAssoc fixes DirectMapped thrash (7.6 ns vs 1005 ns) |
| F2 0 cross-core RMW read path | yes | **CORRECT** | asm: 0 lock-prefixed/xchg/cmpxchg/xadd, 0 mfence, both compilers; g_topo acquire = plain mov |
| F3 O(√N) connections | yes | **CORRECT** | @ N=4096,G=64,K=3: 63 regular / 252 gateway vs 4095 full-mesh; intra-group join churns 0 connections elsewhere |
| F4 O(√N) gossip fan-out (default) | yes | **CORRECT** | Uniform default fan-out = N/G (16/32/64); DynamicAggWeight = N (documented non-containment opt-in) |
| S1 RCU race/UAF-free | yes | **CORRECT** | TSan/ASan clean both compilers; freed==published (no leak/early-free); negative control (immediate free) trips ASan UAF in resolve() |
| S2 single-owner under gossip delay | yes | **CORRECT** | Uniform: 0 level-1 mismatches / 1e6 keys; group-birth window: 30,185 misroutes → 30,185 gateway wrong-group NACKs, **0 silent double-owners** |
| C1 determinism, G=1≡flat | yes | **CORRECT** | 0 disagreements over 1e6 keys × 32 resolvers; G=1 byte-identical to flat HRW |
| C2 contained minimal-reassignment | yes | **CORRECT** | Uniform: moved 947/1e6 (~1/N), 0 cross-group, 0 other-group node change |
| C3 group birth/death + l1_gen cache | yes | **CORRECT** | cross-group moved ~1/G (30,389/1e6); surviving-group level-2 change 0; l1_gen cache 0 stale hits vs 663 for the withdrawn per-group-epoch scheme |

Net: **9 proven, 0 disproven.** Refines invariant 4 to **group granularity** (remote nodes cannot locally compute `ActorId→node`) — its own top-listed risk.

### D3 — VirtualBins + Bounded Partial-View + DHT-Relay (WINNER)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 O(1) N-independent resolve | yes | **CORRECT** | resolve() mean **flat 5.3–6.1 ns** across N (both compilers); O(N) HRW grows 1682→140,880 ns; **4980–24,021× faster**; cache-pressure p99 ≤38 ns (< conceded 100 ns bound) |
| F2 0 alloc / 0 cross-core RMW in layer | yes | **CORRECT** | scope A (resolve+next_hop+beacon): 0.00 RMW/msg, allocs M-invariant (thread bootstrap only); scope B: 0.999 RMW/msg = the single pre-existing 006 ring push, outside 023's drain-path gate |
| F3 ≤⌈log₂N⌉ hops, 100% completion | yes | **CORRECT** | maintained Kademlia: **100%** completion, max hops 4/6/7, p99 4/5/6 @ N=256/1024/4096; unmaintained HyParView view dead-ends 46–95% (proves maintenance earns the bound); Zipfian promotion → 0-hop for hot peers |
| F4 O(log N) gossip convergence | yes | **CORRECT** | rounds-to-100% p99 5/7/8/9 (~log₂N), msgs/node/round = 4 (constant in N) |
| S1 QSBR race/leak/UAF-free (fixed) | yes | **CORRECT** | sentinel-init + registered-worker min + park/unpark reload: TSan/ASan+UBSan clean both compilers; published==freed, peak live=2 (bounded RSS); fixes both original EBR bugs |
| S2 O(N log N) cluster / sub-linear per-node | yes | **CORRECT** | cluster/(N·log₂N) = 1.70/1.54/1.41 (bounded, non-growing); cluster/fullmesh 0.107→0.0083 (gap widens, never N²); per-node O(log N) (honest weakening from "constant") |
| C1 content-addressed determinism | yes | **CORRECT** | same content-digest ⇒ byte-identical owner over 4096 bins + 1e5 ids despite different slot order; divergent rosters carry distinct digests |
| C2 minimal-reassignment on owner | yes | **CORRECT** | moved-owner fraction tracks 1/N (0.018% @ N=4096), 0 spurious moves; CV 0.24–0.28, 0 idle nodes @ B=next_pow2(16N) |
| C3 relay preserves mapping | yes | **CORRECT** | bin_owner == per-bin HRW for 1e5 ids; relay 0 misdelivery over 2e4 sends |

Net: **9 proven, 0 disproven.** Upholds placement invariant 4 **literally** (every node computes the same `ActorId→owner` locally, content-addressed). One untested residual: cross-node FIFO under variable-hop relay (see below).

## Decision

**Winner: D3 — VirtualBins + Bounded Partial-View + DHT-Relay.** Recommended as the opt-in large-N default (N ≳ 512).

Reasoning, in the judge's ranking order:

1. **Safety gate.** No design has a *safe*-kind claim marked WRONG. D1 has a *correct*-kind claim WRONG (C3) but with a stated cheap fix (kBalanceC≥128, 2× memory) so it is not disqualified — but it is not a clean survivor either. D2 and D3 are clean (0 disproven).
2. **Proven beats claimed.** D2 and D3 tie at **9 proven / 0 disproven**; D1 is **4 proven / 2 disproven (F2, C3) / 1 inconclusive** and drops behind despite the fastest raw route (2.98 ns) — its ≤2 ms repair budget (F2) and its load-balance bound (C3) were both disproven by executed C++.
3. **Best measured hot-path numbers among safe survivors.** D3's placement lookup is **unconditionally O(1), 5.3–6.1 ns mean, N-independent, with no cache-miss cliff** (p99 ≤38 ns even under a >L1 working set). D2's hot path is a per-shard cache: the common warm hit is 8.4 ns (g++) / 15–16 ns (clang), but a cold miss recomputes at **O(√N) → p99 up to 3600 ns**. D3 is both faster in the common case and free of the tail cliff. **D3 wins criterion 3.**
4. **Invariant fidelity.** D3 upholds the distribution invariant literally — *every node computes the same `ActorId→owner` from the same membership content* (C1, content-addressed). D2, by contrast, **bends invariant 4 to group granularity**: remote nodes provably cannot compute `ActorId→node` (they hold only O(√N) digests) and must delegate to a gateway. D2 keeps the mapping single-valued and deterministic (proven S2/C1), but literal per-node computability is the price of its O(√N) memory — it is D2's own top-listed risk, and criterion 4 ("a design that bends a core invariant does not win") is a hard gate. **D3 wins criterion 4.**

Criteria 3 and 4 both point to D3.

**D2 is a strong second and is retained in the spec as the opt-in higher tier.** Its genuine wins — O(√N) *memory* (vs D3's O(N) roster) and blast-radius **containment** (computation/connection/gossip scoped to the changed group) — are exactly what matters *beyond* D3's honest wall. D3 keeps an O(N) roster because global HRW determinism requires the node SET; that roster (~160 KB @ N=4096, delta-gossiped) is fine for the stated target but becomes the ceiling around 10⁴–10⁵ nodes. D2's Partitioned topology trades global determinism for per-group determinism to break the O(N) memory floor and is the correct next tier. The two are complementary points on the `topology` axis, not competitors.

**D1's bucketed cache is subsumed:** D3's `VirtualBins` IS a fixed bin table with the same O(1) load, and D3 additionally solves the connection-explosion bottleneck (bounded partial view + relay) that D1 leaves as a LazyBounded mesh needing a re-architected control plane. D1's `Direct`/`FullMesh` remains the flat-small default (zero-cost-when-unused).

### The single tie-breaking experiment before 026 promotes Draft → Accepted

D3's one untested claim is **cross-node FIFO under variable-hop relay** — 010 grants FIFO only "over one connection," and multi-hop relay has no single end-to-end connection. D3's stated fix (next_hop is a deterministic pure function of `(self, owner, roster-digest)` so the path is pinned within a digest; hot-peer promotion takes effect only at a per-destination drain/quiesce boundary; path changes only at a digest bump, which already carries a 021 stabilization boundary) is plausible but **not proven by executed C++**. Required experiment: a sender emits a monotonically-numbered stream to one owner while a promotion / k-bucket update changes the path mid-stream; assert the owner's mailbox observes a strictly increasing sequence (no inversion at the path-change boundary), contrasted against a control with unpinned paths that must show an inversion. This is the Hard gate (per 023: no Draft→Accepted with an unproven Hard property).

## Spec-update recommendations

### NEW `026-Large-Scale-Cluster-Topology.md`
- Define the three configurable axes and their variants; make **D3 (`topology=PartialView`, `connections=BoundedPartialView`, `placement_cache=VirtualBins`) the recommended large-N default (N≳512)**, `Flat/FullMesh/Direct` the small default, and **D2 (`topology=Partitioned<GroupByLabel<"zone">>`) the opt-in tier beyond ~10⁴ nodes** with its group-granularity determinism caveat stated loudly.
- Specify VirtualBins: `bin = splitmix64(ActorId.hash()) & (B-1)`, `B = next_pow2(16·max_nodes)` (validated at 008; ≥16 bins/node — proven CV 0.24–0.28, 0 idle nodes), refilled once per membership epoch off the hot path by per-bin HRW `argmax_n w_n/(−ln H(n,bin))`.

  > **Correction (ADR-011).** The original text here read `argmax_n weight_n·H(n,bin)`, which
  > gate verification proved **non-proportional** (CoV ≈ 1.4, per-node share error 8–28σ). The
  > proportional realization is **log-WRH**: `score = w_n / (−ln H(n,bin))`, `H ∈ (0,1)` — proven
  > proportional (share error 3–5σ, matching the multinomial floor) with bounded churn (0 bins move
  > between *unchanged* nodes on a re-weight). Use the log-WRH form. The absolute balance threshold
  > `CoV ≤ 0.2 ∧ max/mean ≤ 1.5` is a *uniform-only* quantization floor and does not bind weighted
  > placement (see 025 / 026 / ADR-011).
- Specify content-addressed snapshot identity: a **roster content-digest** `hash(sorted {NodeId,weight})`, NOT a bare epoch counter (a counter is not a consensus sequence number coordinator-free); frames carry the digest; a relay re-resolves/bounces on digest mismatch.
- Specify the QSBR reclaim precisely (the proven fix): beacons sentinel-init to `UINT32_MAX`, reclaim = min over **registered** workers via a bitmap, park publishes the sentinel and drops the cached snapshot pointer, unpark reloads `g_topology` (acquire) before any deref, per-worker beacon `alignas(64)`.
- Specify maintained Kademlia routing (per-bucket liveness refresh + α-parallel probing, **separate from the SWIM/HyParView gossip view**, with empty-bucket fallback) — the prover showed the unmaintained view dead-ends 46–95%.
- State the FIFO discipline (deterministic path pinning per roster-digest; promotion only at drain/quiesce boundary) and mark the FIFO experiment above as the Draft→Accepted Hard gate.
- Record the honest bounds: placement O(1) N-independent; per-node sockets **O(log N)** (not "constant"); gossip O(log N) rounds; roster O(N) *entries*; relay adds ≤⌈log₂N⌉ RTTs for cold/uniform traffic.

### `010-Distribution.md`
- Lines 33–34 ("Cost is O(nodes) per placement … a cached ordering handles large clusters"): replace the hand-wave with a pointer to 026's `VirtualBins` bin table (proven O(1), 5–6 ns, N-independent) and the D2 Partitioned tier.
- Add a `connections` seam note: the default one-connection-per-peer is the `FullMesh` policy; 026 adds `BoundedPartialView`/`Gateway` to break O(N²) at scale.
- **Delivery-semantics table (line 140):** annotate cross-node FIFO — "over one connection" holds for FullMesh; under 026 relay topologies FIFO is preserved by deterministic per-digest path pinning (026), pending the FIFO Hard-gate experiment.
- Open questions (155–165): mark the "cached ordering" and "shard vs actor granularity of re-placement" items as addressed by 026 (bin/virtual-bin granularity).

### `021-Cluster-Formation-and-Lifecycle.md`
- Bulk-join / cold-start open question (line 300): 026 requires **batched admission** — process M joiners in one bin-table sweep (one store per changed bin), because per-event repair is O(B) single-writer and D1's evidence showed serial back-to-back repair blows the budget (bulk-100 = 671 ms). Applies to VirtualBins refill and D2 gateway re-election.
- Stabilization window (215): a roster-digest bump is the path-change boundary for 026 FIFO pinning — tie the stabilization settle to digest transitions.
- Keepalive/dial (95–100, 158): the default SWIM-over-Transport keepalive is retained; note that `BoundedPartialView` relies on SWIM **indirect** (k-relay) probing to detect peers held via no direct socket, and that a connectionless-datagram SWIM control plane is an explicit opt-in that must re-supply 018 RTT, keepalive, and per-datagram auth (D1's S2 attack — do NOT present it as free).

### `025-Placement-Policies-and-Stateless-Workers.md`
- Part B: state that the O(1) `VirtualBins`/`Bucketed` cache accelerates the **default unconstrained `HashById`** path; `Require/Prefer/Weighted/Affinity` constrain the eligible subset, so a constrained actor type resolves against a per-eligibility-class bin table (bounded #classes, extra memory/repair) or falls back to O(eligible)≤N.
- `AntiAffinity` / per-actor spread must **bypass** the bin cache (bins quantize placement — two actors sharing a bin co-locate) and compute exact per-actor HRW.
- Weighted-HRW open question (251–253): **elevate to a Hard gate** — D2/D3 both flagged that re-weighting may move >1/N keys under virtual bins/level-1; C2 tests join/leave, not re-weight. Re-weighting minimal-reassignment must be measured before `Weighted` combines with any 026 cache at scale.

### `020-Security.md`
- Confirm the default 026 topologies keep SWIM over the authenticated `SecureTransport` (per-connection handshake, per-message AEAD, lines 93–101), so admission still gates HRW/bin placement.
- Add: **relay nodes and D2 gateways forward application frames** — they remain ordinary `SecureTransport`-authenticated peers chaining to the same trust anchor (no new crypto), but they concentrate cross-partition traffic and are the natural DoS/hotspot target; provision/rate-limit accordingly.
- Add: the opt-in connectionless/datagram SWIM control plane requires a **per-datagram AEAD seal** under the 020 session key (the settled model only specs per-connection handshake auth) or the failure detector is spoofable.

### `009-Observability.md`
- Add per-shard/per-node metrics for 026: placement-cache hit/miss and cold-recompute count; membership-epoch / roster-digest gauge; gossip convergence rounds; relay-hop histogram and dead-letter-on-ttl-exhaustion count; open-socket gauge vs the configured bound; per-partition (D2) digest staleness and gateway wrong-group-reject count.
- These reuse the per-shard plain-counter model (no hot-path RMW); the bin-table refill runs on the membership thread, so its cost is a control-plane metric, not a drain-path one.

### `013-Configuration.md`
- Policy-vs-config table (line 17): add the 026 knobs to the **configuration** column (operational — they change speed/path, never *which* node owns an actor): `cluster.topology`, `cluster.connections`, `cluster.placement_cache`, `cluster.max_nodes`, `cluster.active_view/passive_view`, `cluster.relay_cap`, `cluster.group_label`, `cluster.gateways_per_group (K)`.
- Example:
  `cfg.cluster({.topology=PartialView{.active=16,.passive=64}, .connections=BoundedPartialView{.relay_cap=…}, .placement_cache=VirtualBins{.buckets=next_pow2(16*max_nodes)}, .max_nodes=4096})`.
- Validation (008/line 83): `B ≥ 16·max_nodes`; `active_view ≈ c·log(max_nodes)`; `seeds present iff distribution enabled`; reject `topology=Flat` combined with any large-scale cache/connection machinery being *required* (zero-cost-when-unused: Flat/FullMesh/Direct must instantiate none of it).
- Rule reaffirmed: config may not override an invariant — it cannot make placement non-deterministic or remove FIFO.

### `023-Performance-Targets-and-Budgets.md`
- Add a **placement-lookup latency budget** (currently unbudgeted): Hard ≤ 50 ns p99, Goal ≤ 20 ns p99, N-independent (D3 measured 5–6 ns mean / ≤38 ns p99 under cache pressure; D1 2.98–5.81 ns p50).
- Bind the **0-cross-core-RMW gate (line 80/115)** to the 026 placement read path (D3 F2, D2 F2, D1 S1 all proven 0 RMW on resolve/route; the one ring-push RMW stays outside the drain-path gate).
- Add a **membership-repair budget**: single join/leave incremental repair ≤ 2 ms on the reference core for the default single-table path (D1 missed this at 2.95/4.06 ms on a 2.095 GHz host — remeasure on the 3–4 GHz reference and require batched admission for bulk join).
- Add the **cross-node FIFO-under-relay** correctness experiment as a Hard gate for 026 promotion (the tie-breaking experiment above).
- Add a **balance budget** for bin/bucket tables: CoV ≤ 0.2 **and** max/mean ≤ 1.5 at N≤max_nodes (D1's C3 showed kBalanceC=64 misses max/mean; size the constant to pass the p99, not the mean).

## Residual risks (winner D3)

- **Cross-node FIFO under relay is claimed but unproven** — the single Hard gate before 026 Draft→Accepted (experiment above).
- **Roster stays O(N) entries** (~160 KB @ N=4096). Honest to ~10⁴; beyond that the operator must opt into D2's Partitioned tier (which trades global for per-group determinism).
- **Relay latency tax**: cold/uniform-all-to-all traffic pays up to ⌈log₂N⌉ extra RTTs (12 @ N=4096); genuinely all-to-all workloads should select `FullMesh` or a larger `relay_cap`. Hot-peer promotion only helps repeated destinations.
- **Churn-window loss**: stale k-buckets can dead-end → ttl → dead-letter until convergence; bounded by the 021 stabilization window + passive-view repair, not eliminated.
- **Content-digest convergence window**: two concurrent disjoint joins produce different rosters/digests transiently (inherent to coordinator-free gossip, bounded by F4); single-activation of stateful actors across the window is enforced by 021 handoff/fencing, not by placement.
- **Weighted-HRW + virtual bins re-weighting** may move >1/N bins (untested — 025 open question elevated to a gate).
- **B must be validated at startup** (`B ≥ 16·max_nodes`); a cluster provisioned near N=B collapses bin balance.

## Consequences

- 026 is drafted with D3 as the recommended large-N default, D2 Partitioned as the opt-in higher tier, D1 Direct/FullMesh as the flat-small default — all on the same three config axes, all zero-cost-when-unused.
- Seven specs get the concrete edits above; the FIFO experiment and the remeasured repair/balance budgets on the 3–4 GHz reference gate promotion.
- No coordinator introduced; the three seams remain the only extension points; std-only C++23; HRW minimal-reassignment and coordinator-free determinism preserved.
