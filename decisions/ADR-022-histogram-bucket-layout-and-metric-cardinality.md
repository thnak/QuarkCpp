# ADR-022: Histogram Bucket Layout and Cardinality Control for Per-Shard Metrics

## Status

Accepted

## Question

009-Observability.md leaves two open questions (see its "Open questions" section):

1. Should histogram bucket layout be one fixed, universal exponential scheme
   shared by every histogram in the engine, or should each metric declare its
   own bucket boundaries?
2. Should metrics be aggregated per-actor-**type** (one set of
   counters/histograms shared by every activation of that type on a shard) or
   per-actor-**instance** (a separate set per activation)?

The design must specify the exact mechanism for both axes, how they interact
(does per-instance cardinality multiply a fixed or a configurable bucket
layout?), and how worst-case memory is bounded without a runtime
container/allocation on the hot path.

## Designs debated

1. **FGT — Universal base-2 HDR buckets, per-(shard × type-index) cardinality.**
   Reuses the shipped 64-bucket `Histogram` verbatim, unmodified, for every
   metric. Cardinality is per-type only (no per-instance axis at all): one
   `TypeHistogramBlock` per registered type, dense-indexed by `type_index`,
   pre-sized once at `Engine::build()`.

2. **Per-metric configurable buckets, per-actor-type cardinality grid.**
   Each metric slot gets its own `{shift, step_shift, total_buckets}`
   `HistogramLayout` (closed-form bucket math, one lookup + one shift + one
   clamp). Cardinality is still per-type only — a `(shard × type_index)` grid
   of `ActorMetricCell`, one allocation at build().

3. **Per-metric buckets with opt-in per-instance cardinality (bounded
   critical-arena).** Each metric has its own `Spec::boundaries()`. Cardinality
   is two-level and *composes*: a per-type block **always** exists (default for
   every actor); an actor type may additionally opt in, via a new CRTP policy
   `PerInstanceMetrics<N>`, to a bounded per-instance level backed by a single
   engine-wide `critical_arena` — a flat, pre-sized array partitioned by
   prefix-sum offsets per opted-in type, with per-(shard,type) fixed-capacity
   freelists handing out instance slots once at activation creation (not per
   message). Per-instance cardinality never changes bucket layout — it
   multiplies the *same* `Spec::boundaries()` into more physical block
   instances, only for types that ask for it, bounded by `critical_arena_reserve`.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| F1 hot-path record cost ≈ baseline | FGT | Yes (revised, mandatory `type_index` param) | **WRONG** (fast) | clang++ delta +2.55–2.65ns/op vs claimed ≤2ns; scales with touched working set (256 types, 1.36MB > L1) |
| F2 rollup cost tracks live count, not cap | FGT | Yes | **CORRECT** | 160 snapshot() calls = 8×2×10 exactly (not 20,480 = cap-sized) |
| S1 race-free across type_index under steal churn | FGT | Yes | **CORRECT** | 0 TSan/ASan/UBSan reports, 2.9–3.5M records |
| S2 array sized to live count, never reallocated | FGT | Yes (revised: sized to live count + reserve, not to `max_types` cap) | **CORRECT** | RSS ≈ 212KB vs 10,880KB cap-bound; base ptr stable across 3 dynamic `add_type()` calls |
| S3 cache-line padding prevents false-sharing collapse | FGT | Yes (revised: platform-conditional) | **CORRECT** | unpadded adjacent-cell storm: 63–72% throughput drop (matches ADR-008 F3's 74%); padded: 1.5–2.9% drop |
| C1 no per-instance drill-down (accepted trade-off) | FGT | Yes | **CORRECT** | 5 instances of same type → single merged Histogram, count=100,000=5×20,000 exactly |
| C2 merge is exactly associative/commutative | FGT | Yes | **CORRECT** | 10 partition schemes × 1M values, bucket-exact match to reference, both compilers |
| F1 bucket_index_of ≈ bucket_of cost | Per-type grid | Yes | **CORRECT** | inlined delta 0.0003–0.15ns/op |
| F2 grid addressing costs ≈ baseline | Per-type grid | Yes | **WRONG** (fast) | clang++: baseline 375.98 Mmsg/s vs design 287.27 Mmsg/s (−30%); matched-cardinality control still −89% on clang |
| S1 zero heap alloc after build() | Per-type grid | Yes | **CORRECT** | 1 alloc at build(), 0 on spawn/record (incl. aligned-new) |
| S2 TSan-clean cross-worker handoff | Per-type grid | Yes | **CORRECT** | 0/12 TSan reports across forced drain-owner migration |
| C1-fix bucket_index_of bounds/monotonic (total_buckets≥1 enforced) | Per-type grid | Yes (revised after conceding fatal underflow at total_buckets=0) | **CORRECT** | 460M+ property-test checks, 0 violations, both compilers |
| C2 per-type isolation, zero cross-contamination | Per-type grid | Yes | **CORRECT** | exact count/sum match, 0 contamination |
| C3-fix footprint bounded + budget-gated | Per-type grid | Yes (revised: added `MetricsBudgetExceeded` gate) | **CORRECT** | 11.44 GiB request correctly rejected, 0 allocations; in-budget case exact byte match |
| F1 opt-in branch costs ~nothing for non-opted types | Per-instance opt-in | Yes | **CORRECT** | codegen: 1 cmp + 1 untaken jne, 0 extra loads; wall-clock within noise |
| S1 freelist touched only by owning shard worker | Per-instance opt-in | Yes (dependency on 001/007 made explicit) | **CORRECT** | 0/12 TSan reports in InstanceFreelist; races only in accepted plain-write/atomic-ref pattern |
| S2r freelist overflow fails safe (was: fatal unchecked push) | Per-instance opt-in | **No** as originally stated (fatal); **Yes** after fix | **CORRECT** (post-fix) | debug: assert-abort (rc=134); release: saturating no-op, canary region byte-identical |
| C1 per-type ≥ Σ per-instance, equality iff no overflow | Per-instance opt-in | Yes | **CORRECT** | 200 randomized trials, 0 failures |
| C2 cardinality never changes bucket semantics | Per-instance opt-in | Yes | **CORRECT** | 500K values × 2 Specs, 0 divergences |
| C3 guarded registration fails fast, no partial mutation | Per-instance opt-in | Yes | **CORRECT** | CapExceeded, pointer-identical grid, 0 allocations |
| C4 recycled slot never inherits stale data (was serious gap) | Per-instance opt-in | Yes (fix: reset-on-release) | **CORRECT** | cap=1 forced reuse: scraped count=333, not 1110 (K+M) |
| F3 slot resolution amortizes to O(1) per activation lifetime | Per-instance opt-in | Yes | **CORRECT** | pop=1, push=1, reset=1 for N=1,000,000 messages |
| M1 bookkeeping bounded, small vs. histogram data (was: 3.5× larger) | Per-instance opt-in | Yes (fix: flat freelist span) | **CORRECT** | bookkeeping 2.56 MiB = 7.1% of 36.0 MiB histogram data (was 128 MiB = 355%) |
| F2 opted-in cost ≤2.3× baseline | Per-instance opt-in | Yes | **WRONG** (fast, marginal) | clang++ 1.48–1.55× (within bound); g++ 2.32–2.42× (small, repeatable overshoot) |

## Decision

**Winner: Design 3 — Per-metric buckets with opt-in per-instance cardinality
(bounded critical-arena).**

No design was disqualified by the safety gate: every claim tagged `safe`
across all three designs is currently proven **CORRECT**. Design 3's original
`S2` (unchecked `InstanceFreelist::push`) was a real, fatal, mechanically
demonstrated hole — a double-unbind silently corrupted adjacent bookkeeping
with no sanitizer signal — but the debate produced a concrete, cheap fix
(bounds-assert in debug, saturating no-op in release, idempotent
`unbind_activation`, reset-on-release), and that fix (`S2r`) was itself
compiled, run, and proven correct. Per the ranking rule, a stated cheap fix
that is then proven closes the gate; it does not disqualify the design.

With the gate clear, the decision turns on proven-vs-claimed volume and
severity, then hot-path numbers, then invariant fidelity:

- **Proven-claim record.** Design 3 has 9 of 10 claims proven CORRECT, with
  the sole failure (`F2`, opted-in cost ratio) a *marginal* overshoot — 2.32×
  vs. a claimed 2.3× bound, on one of two mandated compilers, and only for
  actors that explicitly opt in. FGT has 6 of 7 correct, but its failure
  (`F1`) is on its **default, always-on** hot path (every message, every
  actor), and the per-type grid design has 6 of 7 correct with a failure
  (`F2`) that is serious and unresolved: a reproducible **28–30% regression on
  clang++**, and — critically — an **89% regression in a matched-cardinality
  control** (type count pinned to 1, isolating pure addressing overhead from
  cardinality multiplication) that contradicts the design's central "grid
  addressing is free" premise on a mandated compiler.
- **Only design that actually answers the question asked.** FGT and the
  per-type grid design both foreclose the per-instance axis entirely (an
  explicit, honest trade-off, but it means neither design has a real
  "how do the two axes interact" mechanism to prove). Design 3 is the only
  one of the three that gives per-instance cardinality a concrete, bounded
  mechanism — a fixed-capacity critical arena addressed by prefix-sum offset
  + freelist-assigned instance slot, multiplying the *same* per-metric
  `Spec::boundaries()` — and every claim about that mechanism (`C1`–`C4`,
  `F3`, `M1`, `S1`, `S2r`) survived proof. This is the substantive design
  contribution 009's open question was asking for.
- **Core-invariant fidelity.** The task's ground invariant states shard
  counters/histograms should be "plain (non-atomic) integers, single-writer
  per shard; a scraper reads via a relaxed atomic snapshot... entirely off
  the hot path." Design 3's revised `HistogramBlock` is the only one of the
  three that matches this **literally**: plain (non-atomic) `uint64_t`
  fields on the write side, `std::atomic_ref` used only transiently by the
  scraper. FGT and the per-type grid design both keep genuine
  `std::atomic<uint64_t>` members with relaxed load-then-store on the write
  side — a pre-existing deviation from the letter of the invariant (inherited
  from the shipped `Histogram`, not introduced by either design), but a
  deviation nonetheless. TSan confirms design 3's only races are exactly the
  accepted write/atomic-ref-read pattern (9 and 8 races respectively,
  zero of which touch freelist bookkeeping).
- **Hot-path numbers, weighed but not decisive.** On raw measured ns/op,
  design 3's absolute costs (15–84ns/op for a full `record_latency()` call)
  are higher than FGT's (~5.9–6.2ns/op) or the per-type grid's (~2.7–3.9ns/op
  when it isn't regressing), because design 3's `HistogramBlock::record()`
  uses an O(bucket_count) linear boundary scan rather than a closed-form
  bucket calculation. This is a real, disclosed cost — but it is an
  **implementation detail of the bucket-search algorithm, orthogonal to the
  cardinality mechanism being judged**, and it is directly fixable (see spec
  recommendations below) by borrowing the per-type grid design's closed-form
  `bucket_index_of` in place of the linear scan, without touching design 3's
  cardinality architecture. Given that (a) design 3's *own* default
  (non-opted-in) path shows zero measurable branch overhead versus its own
  baseline, and (b) the two designs with better absolute numbers each have a
  proven, non-marginal performance claim failure on a mandated compiler
  (FGT: default-path overshoot; per-type grid: up to 89% regression even in
  isolation), a fixable, disclosed algorithmic-constant issue in a bounded,
  opt-in code path is preferred over a proven, unresolved regression in an
  always-on default path.

## Residual risks

- **Linear bucket-search cost.** Design 3's `HistogramBlock::record()`
  currently does an O(bucket_count) linear scan of `Spec::boundaries()`.
  Measured absolute cost (15–84ns/op) is materially higher than the
  closed-form alternatives proven in designs 1/2 (~2–6ns/op). Must be
  replaced before this ships against 023's budgets (see spec recommendation).
- **F2 marginal overshoot on g++.** The opted-in cost ratio (2.32–2.42×)
  slightly exceeds the claimed 2.3× bound under g++ specifically, repeatably.
  Expected to shrink or disappear once the bucket-search algorithm is fixed
  per above; must be re-measured after that change, not assumed fixed.
- **`critical_arena_reserve` oversubscription.** A single engine-wide shared
  pool; an operator who opts in many types or one type with large `N` can
  inflate the fixed per-shard footprint even though most of it goes unused
  at runtime. No sizing tool exists yet — only a hard `CapExceeded` gate at
  the boundary.
- **Silent freelist-exhaustion degradation.** Overflow instances fall back to
  per-type-only recording with no dedicated counter. An operator watching a
  per-instance dashboard for a specific hot activation could wrongly
  conclude it is quiet when it actually overflowed. Needs an explicit
  `instance_slot_exhausted` per-(shard,type) counter (itself a plain counter,
  no new mechanism).
- **`PerInstanceMetrics<N>` vs. pooled `Stateless<N>` actors.** Pool
  activations have no stable per-key identity to pin an instance slot to
  across their lifetime. Must be enforced as a compile-time
  `static_assert` incompatibility between the two CRTP policies, not left as
  a documentation note.
- **Instance-slot → ActorId label side-table.** Needed for per-instance
  scrape output to be meaningful (otherwise per-instance buckets are
  anonymous). Must be updated in lockstep with `bind_activation`/
  `unbind_activation` inside the same off-hot-path step to avoid a stale
  label.
- **S1's safety is inherited, not self-fenced.** Freelist safety under
  concurrent scrape depends on 001/007's guarantee that no deactivation
  happens while a handler (including a suspended coroutine continuation) is
  in flight. This design does not itself enforce that; a future change to
  activation lifecycle timing (e.g., for cross-shard migration, 010/021)
  would silently invalidate the proof and must be caught by keeping the S1
  coroutine-suspend stress test in CI as a regression guard.
- **`Spec::bucket_count` is developer-unbounded** on this axis; a Validation-time
  upper bound (matching the per-type grid design's `kHistogramCap`-style
  ceiling) should be added so a careless per-metric `Spec` can't regress the
  linear-scan (or its closed-form replacement) unpredictably.
- **ARM64 128B-cache-line false-sharing case remains empirically unverified**
  for the FGT design's padding mechanism (proven only via the generalized
  `Histogram`-array analog, not the literal `TypeHistogramBlock` at 128B
  line size) — not disqualifying for design 3 since it doesn't use that
  layout, but flagged as still-open for the codebase's existing
  `hot_cell.hpp` relaxed-store-under-weak-memory caveat this debate did not
  resolve.

## Spec recommendations for 009-Observability.md

1. **Resolve both "Open questions" bullets explicitly**, replacing them with:
   - *Bucket layout*: per-metric configurable via a compile-time
     `HistogramSpec` (`boundaries()` + `bucket_count`), not a single universal
     scheme. Add a Validation-time upper bound on `bucket_count` (e.g. ≤ 64,
     mirroring the per-type grid design's `kHistogramCap`) so an
     over-provisioned per-metric spec can't blow up per-record cost or
     per-cell memory.
   - *Cardinality*: two-level and default-safe — per-actor-**type** always
     exists (one block per registered type, dense `type_index`-addressed,
     sized once at build-time registration, never reallocated); per-actor-
     **instance** is strictly **opt-in** via a new CRTP policy
     (`PerInstanceMetrics<N>`), backed by one engine-wide, build-time-sized
     `critical_arena` (prefix-sum offsets per opted-in type + a per-(shard,
     type) fixed-capacity freelist, instance slot resolved once at activation
     creation and cached on the activation — never re-resolved per message).
     State explicitly: per-instance cardinality multiplies the *same*
     per-metric bucket layout into more physical blocks; it never changes
     bucket semantics.
2. **Replace the linear boundary scan with a closed-form bucket lookup** for
   `HistogramSpec`-declared metrics — reuse the `{shift, step_shift}`
   closed-form technique proven in the per-type grid design (`bucket_index_of`,
   proven within noise of the shipped `bucket_of`), or an equivalent
   branch-light scheme, in place of `while (v > b[i]) ++i;`. This is required
   before the per-instance design's numbers are acceptable against 023's
   hot-path budgets.
3. **State the exact write/read memory-order contract as a normative rule**,
   not just an example: histogram cells are plain (non-atomic) integers on
   the single-writer side; the scraper reads via `std::atomic_ref` at
   relaxed order and aggregates on read. Explicitly note this depends on
   002's drain-owner handoff establishing happens-before across a work-steal
   migration, and require the TSan migration-stress test (already proven
   clean in this debate) as a permanent CI regression guard for that
   dependency.
4. **Add a build()-time memory-budget gate** for any new metrics grid/arena:
   compute the exact worst-case footprint via `sizeof()` (not hand
   arithmetic — this debate found a real 3.5× undercount in an early draft)
   and reject `Engine::build()` with `std::unexpected(MetricsBudgetExceeded)`
   if it exceeds a configured `metrics_memory_budget_bytes` (013-style
   knob), before attempting the allocation.
5. **Document the opt-in per-instance failure modes as first-class,
   observable behavior**, not silent fallback: freelist exhaustion degrades
   to per-type-only recording for overflow instances (never allocates,
   never blocks) and must increment a dedicated
   `instance_slot_exhausted` counter; `PerInstanceMetrics<N>` combined with
   `Stateless<N>` pooled actors must be a compile-time `static_assert`
   failure (pool activations have no stable per-key identity for slot
   pinning); the instance-slot → `ActorId` label side-table must be updated
   atomically with `bind_activation`/`unbind_activation`.
6. **Note the inherited caveat, do not re-litigate it here**: the shipped
   `Histogram`'s relaxed load-then-store (non-RMW) single-writer pattern
   under weak memory order on ARM64 is still only partially verified (see
   `hot_cell.hpp`'s existing caveat); this ADR does not resolve it and it
   applies unchanged regardless of which histogram design is chosen.

## Tie-breaking experiment (if evidence had been insufficient)

Not needed — the evidence is sufficient to decide. If a future revision
wants to re-open this, the single most informative next experiment is:
re-run design 3's `F2` (opted-in-vs-baseline ratio) *after* replacing the
linear boundary scan with the closed-form `bucket_index_of` from the
per-type grid design, on both g++ and clang++, to confirm the marginal g++
overshoot (2.32–2.42× vs. 2.3× claimed) closes once the bucket-math constant
factor is fixed.
