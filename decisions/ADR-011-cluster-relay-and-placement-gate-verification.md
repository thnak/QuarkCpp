# ADR-011 — Cluster Relay & Placement Gate Verification (026 / 025 / 010)

Status: **Accepted (verification record)**
Date: 2026-07-15
Deciders: gate-verification judge (closing the ADR-006 / 010 / 025 / 026 Draft→Accepted gates)
Supersedes/relates: executes — does **not** redesign — the SETTLED cluster design of
[ADR-006](ADR-006-large-scale-cluster-topology.md) (VirtualBins O(1) placement,
bounded partial-view, DHT-relay) and specs
[010-Distribution.md](../010-Distribution.md),
[025-Placement-Policies-and-Stateless-Workers.md](../025-Placement-Policies-and-Stateless-Workers.md),
[026-Large-Scale-Cluster-Topology.md](../026-Large-Scale-Cluster-Topology.md);
resolves the "Cross-node **FIFO under relay** — unproven" Hard budget cell in
[023-Performance-Targets-and-Budgets.md](../023-Performance-Targets-and-Budgets.md)
(line 124); leaves the 010 cross-node **backpressure** design question open.

---

## Question

Three named GATE experiments were executed to prove or disprove the one unproven
claim behind each Draft spec. This record rules each gate (accepting a `CORRECT`
**only** when its mandatory positive control fired non-vacuously and sanitizers
were clean where relevant), then decides per-spec promotion for 010 / 025 / 026.

The bar for every gate: the positive control MUST fire (the discrimination is a
live detector, not a vacuous pass), attribution controls (null baselines) must be
clean, and the anti-cheat scale/observability conditions must hold. A gate whose
control cannot fire is **INCONCLUSIVE**, never `CORRECT`.

---

## Gates & rulings

### Gate A — FIFO-under-relay (`gate-026-fifo-under-variable-hop-relay`) → **CORRECT**

Faithful ADR-006 relay: VirtualBins placement (splitmix64 actor→bin→owner),
bounded partial view + greedy Kademlia-XOR relay to a non-peer owner, deterministic
per-digest **path pinning** (`next_hop` a pure fn of `(self, owner, epoch)`, epoch
carried per message), **drain-boundary** promotion (hot-peer owner promoted into
S's active view only after the old-path in-flight of that `(S,A)` stream has fully
quiesced, mid-stream under sustained load). Real cross-thread relay: sender, every
on-path node, and the owner are distinct worker threads with their own condvar FIFO
queues; forwarding crosses threads on a 32-core host. Arrival order recorded raw at
the owner — no dedup, no watermark, no receiver re-sort — so a reorder surfaces as
an inversion (AtMostOnce / pre-dedup).

- **inv_pinned = 0** across 100 trials × 10⁶ arrivals/cell under **both** g++14.2
  and clang20.1 (zero-tolerance met); also 0 under all TSan/ASan runs.
- **Control FIRED** (unpinned + immediate mid-stream promotion, identical harness,
  only pinning+drain removed): g++ 137 inversions, 88/100 valid-race trials;
  clang 168 inversions, 96/100 — both ≫ the 50% preregistered threshold, and only
  in trials with a non-empty pipe at the switch.
- **Null baselines** (switch disabled): inv_null_pinned = inv_null_control = 0 →
  inversions attributable solely to the path change.
- **Real path change**: pre-switch relay hop_count ∈ [2..6] (owner outside S's
  active view, ≥2 hops enforced), post-switch = 1, measured from the trace in
  100/100 trials both arms. In-flight ≥1 at the switch in 100/100 pinned trials
  (validRace) → genuine mid-stream promotion, not stop-the-world / empty-pipe.
- **Sanitizers**: TSan/ASan/UBSan clean on the pinned arm with real cross-thread
  happens-before edges; teeth proven — a deliberate-race control (`tsan_teeth.cpp`)
  was flagged by TSan, exit 99. The clean pass is not a no-op.

All anti-cheat vacuity routes closed (dedup-masking, phantom-path-change,
stop-the-world, single-thread fake, receiver re-sort, asymmetric control,
empty-pipe, tiny-scale). **Verdict: CORRECT.**

### Gate B — Weighted-HRW-distribution (`gate-weighted-hrw-distribution-025-026`) → **WRONG**

Faithful ADR-006 VirtualBins: B = next_pow2(16·max_nodes) = 65536, per-bin argmax
weight-scaled rendezvous, heterogeneous weight tiers {1,3,8} (min:max 1:8, 3 tiers),
N swept {64,256,1024,4096}, p99 over 8 seeds, balance on dimensionless
ρ_i = realized_i / w_i, churn = |{bin: owner changed}|/B over the full table.

The literal ADR-006 line-104 form `weight_n·H(n,bin)` is **non-proportional**
(CoV ≈ 1.4, share error 8–28σ); per the "most charitable ADR-consistent choice"
rule the prover used the canonical proportional realization
`score = w_n / (−ln u)`, `u = H(bin,node) ∈ (0,1)` (noted in source).

- **Churn claim CORRECT, control FIRED**: log-WRH f_w tracks the intended share
  delta Δ within a few percent; bins trading between two *unchanged* nodes =
  **0.00000 exactly at every N** (perfect cross-node minimal disruption);
  f_w ≤ f_c/10. The mandatory control (modulo-slot, weight = contiguous replicas)
  reshuffled **97.5–99.9%** of bins on the identical Δ (≥0.5 gate, ~(N−1)/N target)
  → a weight change *can* force a near-global reshuffle, so WRH's small movement is
  a real, non-vacuous property.
- **Distribution claim FAILS**: the stated decision inequality
  (CoV(ρ) ≤ 0.2 **AND** max/mean ≤ 1.5) holds at N=64/256 but **fails** at
  **N=1024** (max/mean 2.06) and **N=4096** (CoV 0.359, max/mean 3.48), at p99,
  reproduced byte-identical across compilers, ASan/UBSan clean.
- Proportionality itself is sound (worst per-node share error 3–5σ), and the
  failure is shown to be the information-theoretic **quantization floor**: WRH CoV
  (0.051/0.095/0.178/0.359) matches an ideal proportional multinomial sampler
  (0.050/0.094/0.178/0.355) and the closed-form floor
  √((W/(N·B))·Σ 1/w_i) (0.044/0.087/0.174/0.349) to within 1–3% at every N. With
  min:max ≥ 8 weights at 16 bins/node (N = max_nodes), light nodes get ~4 bins, so
  CoV ≤ 0.2 is **unachievable by any proportional scheme** — the threshold was
  calibrated on *uniform* bins (026 line 58 / 023 line 123) and is ill-posed under
  weighting.

The judge accepts the ruling **as specified**: a stated Hard decision inequality
fails at p99 with clean, reproduced, sanitizer-clean evidence and a firing control.
Per the gate's own decision rule (`WRONG iff any DISTRIBUTION inequality fails`)
this is a genuine **WRONG**, not INCONCLUSIVE — the control fired and attribution is
clean, so the negative result is real, not vacuous. The load-bearing/novel half
(bounded churn, no global reshuffle under reweighting) is `CORRECT`; the compound
claim as written is not. **Verdict: WRONG.**

### Gate C — Stateless-pool-relaxation (`GATE-025-Stateless-pool-relaxation`) → **CORRECT**

Faithful 025 mechanism: pool of N single-executor activations, each owning an
intrusive Vyukov MPSC mailbox (per 001/002), local load-aware routing
(power-of-two-choices least-loaded; round-robin variant), **no** cross-message
shared state, genuine per-activation state (XOR accumulator + count, teardown
invariant checked), backpressure retries — never sheds. Unique 64-bit token per
message; exactly-once checked by **SET**, not count.

- **Safety**: lost = dup = torn = **0** by set, at M = 10⁷ × 10 runs, both
  compilers, N=16 (cross-socket, 32 distinct cores) and N=32/P=31 (oversubscribed),
  under verified saturation (maxdepth 2050 vs cap 2048, sustained backlog).
- **Real build sanitizers CLEAN** both compilers (TSan/ASan/UBSan, exit 0).
- **Control FIRED**: the mandatory shared plain non-atomic field RMW'd by ≥2
  activations on the drain path → TSan reports a data race on `g_shared_plain`
  at `handle_one:189`, exit 99, on **both** g++ and clang; measured concurrent
  overlap up to 272 s ⇒ activations genuinely run on distinct cores ⇒ the clean
  real pass is non-vacuous.
- **Second control (parallel scaling)**: a global-lock dispatch variant stays flat
  (0.83× at N=16) while the real per-mailbox pool rises to **5.68×** (≥0.7·8) —
  kills the global-lock-cheat route; safety and throughput come from the same
  binary.
- **Throughput** (msg/s/core, equal core budget, pool lost=0): pool **beats**
  hand-rolled separately-addressed actors **1.52–2.79×** (g++ N=16 1.53×,
  clang N=16 1.52×, g++ N=32 2.79×). Oracle proven non-vacuous: injected
  drop-one/dup-one → lost=1,dup=1 at proc=M (a count-only oracle would pass).

All anti-cheat routes closed (serialized producers, global-lock, count-only oracle,
lost==dup cancellation, no-real-state, strawman baseline, win-by-shedding,
non-saturating load, unwired sanitizer, synchronized control, tiny-N).
**Verdict: CORRECT.**

---

## Evidence table

| Gate | Control fired? (non-vacuous) | Sanitizers | Verdict | Headline number |
|---|---|---|---|---|
| A — FIFO-under-relay (026/010) | **YES** — unpinned control 137/168 inversions, 88–96% of valid-race trials; nulls 0; path changed 100/100; validRace 100/100 | TSan/ASan/UBSan clean, teeth proven | **CORRECT** | **inv_pinned = 0** / 100 trials × 10⁶ arrivals/cell, both g++14 & clang20 |
| B — Weighted-HRW-distribution (025/026) | **YES** — modulo control reshuffled 97.5–99.9% on identical Δ; churn between unchanged nodes = 0 | ASan/UBSan clean (TSan N/A, no concurrency) | **WRONG** | Distribution **CoV 0.359 / max/mean 3.48 at N=4096** (and max/mean 2.06 at N=1024) FAIL; churn CORRECT |
| C — Stateless-pool-relaxation (025) | **YES** — shared-plain control races on `g_shared_plain` both compilers, exit 99; scaling control flat vs 5.68× | TSan/ASan/UBSan clean | **CORRECT** | lost=dup=torn=**0** @ M=10⁷; pool beats hand-rolled **1.52–2.79×** msg/s/core |

---

## Per-spec promotion decisions

### 026-Large-Scale-Cluster-Topology → **PROMOTE — Accepted (x86-64)**

Rule: *026 promotes iff FIFO-under-relay is CORRECT (control inverted).*
Gate A is CORRECT with the mandatory control firing (inverted 137/168 times, 88–96%
of trials). The one unproven claim behind 026 — that path-pinning + drain-boundary
promotion preserves per-`(S,A)` FIFO across a mid-stream variable-hop relay path
change — is proven with zero-tolerance and clean attribution. **026 moves
Draft → Accepted (x86-64).**

> Note: 026's *distribution/balance* Hard threshold (CoV ≤ 0.2, max/mean ≤ 1.5)
> is separately shown WRONG for weighted placement (Gate B). 026's FIFO promotion
> stands; its balance threshold text must be repaired (see residual risks) but that
> is a threshold-restatement, not a topology-design defect, and does not gate the
> FIFO promotion.

### 025-Placement-Policies-and-Stateless-Workers → **HOLD — stays Draft**

Rule: *025 promotes iff BOTH Weighted-HRW-distribution AND Stateless-pool-relaxation
are CORRECT (controls fired).*
Gate C (Stateless-pool) is CORRECT, but Gate B (Weighted-HRW-distribution) is
**WRONG** — a stated Hard distribution inequality fails at p99 for N ∈ {1024, 4096}.
The AND is not satisfied. **025 is HELD at Draft.** The single missing step to
promotion is not a new measurement — the measurement exists and is decisive — it is
a **spec repair + re-gate**: restate 025's weighted-placement balance target on
quantization-adjusted / load-per-weight terms (or mandate a larger B, e.g. bins/node
≫ 16, for heterogeneous weights) so the threshold is well-posed, then re-run Gate B
against the corrected inequality. Until then 025's stateless-pool half is proven but
cannot carry the spec alone.

### 010-Distribution → **PROMOTE-CORE — Accepted (x86-64, core)**

Rule: *010 promotes iff FIFO-under-relay is CORRECT — but if the open cross-node
BACKPRESSURE design question is unresolved, recommend "Accepted (x86-64, core)" for
010's placement + FIFO data path and flag backpressure as the residual.*
Gate A is CORRECT, so 010's **placement + FIFO data path** is promoted to
**Accepted (x86-64, core)**. The cross-node **backpressure** design question remains
open and unproven by any gate here, so 010 does **not** receive full Accepted; the
FIFO/placement core is Accepted and backpressure is carried as the named residual.

---

## 023 budget-cell update

Gate A is `CORRECT`, so the Cross-node FIFO-under-relay cell in
`023-Performance-Targets-and-Budgets.md` (line 124) moves from **unproven** to the
measured result. New cell content:

> | Cross-node **FIFO under relay** | correct | **correct (Hard gate)** | **correct — 0 inversions / 100 trials × 10⁶ arrivals per cell, both g++14.2 & clang20.1 (ADR-011, gate-026-fifo-under-variable-hop-relay); control fired 88–96% (137/168 inversions), null baselines 0, real hop-vector change 2–6→1 in 100/100, TSan/ASan/UBSan clean** |

---

## Residual risks

1. **[010 — named residual] Cross-node backpressure is unproven.** No gate here
   exercised flow-control / credit propagation across the relay; only the FIFO
   data-path ordering was proven. This is why 010 is Accepted (x86-64, **core**),
   not full Accepted. Must be resolved by its own gate before 010's backpressure
   path can promote.
2. **[025 blocker] Weighted balance threshold is ill-posed.** CoV ≤ 0.2 /
   max/mean ≤ 1.5 was calibrated on *uniform* bins (026 line 58, 023 line 123) and
   is unachievable by *any* proportional scheme at N = max_nodes with 16 bins/node
   and min:max ≥ 8 weights (proven a quantization floor). 025 stays Draft until the
   threshold is restated on quantization-adjusted / load-per-weight terms or B is
   sized larger for weighted deployments, and Gate B is re-run. This also implicates
   the **023 line 123** balance cell and **026 line 58** — they hold for uniform
   placement but not under weighting; both should be scoped/annotated accordingly.
3. **[ADR-006 text defect] Line 104 `weight_n·H(n,bin)` is non-proportional**
   (CoV ≈ 1.4, share error 8–28σ). An implementer building to the literal formula
   gets badly skewed placement. ADR-006 should be corrected to the proportional
   log-WRH realization `score = w_n / (−ln H(bin,node))`. The *proportionality* of
   the corrected form and the *bounded-churn / zero-between-unchanged-nodes*
   minimal-disruption signature are proven CORRECT and safe to rely on.
4. **[scope] All three gates are x86-64 only** (32-core host). The FIFO-under-relay
   happens-before edges (mutex/condvar handoff, atomic epoch/in-flight) and the
   stateless-pool MPSC publish are proven on TSO; weak-memory (ARM) behavior is
   behind the PAL and must be re-validated before any non-x86 promotion. Promotions
   here are explicitly **(x86-64)**.
5. **[hardware-bounded proof] Stateless-pool** true N = cores with P = cores−1 on
   all-distinct cores is physically impossible on the reference box; the clean
   cross-core config is N=16+P=16 (32 distinct cores), with N=32/P=31 oversubscribed.
   Per-core throughput still beats hand-rolled, but a non-oversubscribed full-core
   proof is bounded by the reference hardware.
