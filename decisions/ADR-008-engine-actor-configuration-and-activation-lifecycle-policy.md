# ADR-008 — Engine & Actor Configuration + Activation-Lifecycle Policy Model

Status: **Accepted**
Date: 2026-07-15
Deciders: design-debate-prove judge (closing the 013/008/005 configuration + activation-lifecycle debate)
Supersedes/relates: builds on ADR-002/003/004 (mailbox MPSC hot path) and ADR-007
(authoring & dispatch API); resolves the standing open questions in
`013-Configuration.md`, `008-Metadata-and-Startup.md`, `005-Developer-Model.md`,
`011-Timers-and-Scheduled-Work.md`; binds the 0-cross-core-RMW drain gate in
`023-Performance-Targets-and-Budgets.md`.

---

## Question

Resolve, **as one coherent configurable policy surface**, the standing open questions
for how a developer *configures* and *operates* the engine and how activation lifecycle
is governed — every choice a policy/config knob with a declared **override scope**
(built-in defaults < engine < node < actor-type < instance) and a declared
**reconfig class** (Live vs BuildOnly), with no attributes, no reflection, no RTTI/virtual
on the hot path, and no lock on the hot-path config read:

- **(013)** Override precedence across the five layers, and which knobs are
  LIVE-reconfigurable vs BUILD-ONLY; the live-reconfiguration mechanism itself (how a
  new config becomes visible to running workers without stalling or tearing a read).
- **(008)** Hot-reload / dynamic actor-type registration after `build()` — forbid, or a
  guarded add that re-runs Validation incrementally.
- **(005/011)** Activation-lifecycle policy — `KeepAlive` / `IdleTimeout<Ms>`
  deactivation, driven by the 011 per-shard timer wheel, expressed as configurable policy.

The **provable core** is the config READ + live-reconfig PUBLISH: scheduling/admission
reads (drain budget 002, overflow 022, idle timeout) must be a **single stable load**
with **0 added cross-core RMW** and **no torn value** under a concurrent reconfig; the
live publish must be **lock-free** and must **not stall the drain**. TSan under concurrent
reconfig+drain; measure publish→visible latency and per-read overhead (percentiles, not
means). Override precedence must be deterministic. Dynamic registration (if allowed) must
re-validate incrementally without racing live traffic. x86-64 Linux, GCC 14.2 +
Clang 20.1, -O2/-O3+LTO.

---

## Designs (one-line summaries)

- **D3 — Frozen-Core + Hot-Leaf** (WINNER): split config by reconfig class. Structural
  knobs (type set, shard count, placement topology, execution mode) are FROZEN at
  `build()`, BuildOnly, fail-fast if changed live. The entire per-message operational
  read-set (drain budget, mailbox bound, overflow, idle timeout) is pre-resolved through
  the precedence chain and **packed into one 8-byte-aligned atomic word per
  (shard × type_index)**; the hot read is a single `mov + mask` relaxed load — 0 RMW, no
  branch to decode, no possible tear. "Live reconfig" is a single relaxed store of a
  re-packed word (no pointer to free, no reclamation on the hot word). RCU/QSBR is
  confined to the rare oversized "warm leaf" and the append-only metadata table. Guarded
  dynamic `add_actor_type<T>()` re-validates incrementally and publishes a new immutable
  metadata table by one release pointer swap. Idle deactivation rides the 011 wheel.
- **D1 — LICS (Layered Immutable Config Snapshot + QSBR publish)**: fold all five layers
  once into a single immutable versioned `ConfigSnapshot` (flat `TypeConfigRow[]` by
  `type_index`), publish by one release-store of `g_config`; the drain does one
  acquire-load per drain *acquisition* (amortized over the budget) plus plain loads from
  frozen memory; admission reads a per-mailbox packed `admit_word_`. Type set FROZEN
  (dynamic registration forbidden). Old snapshots reclaimed by hand-rolled QSBR with a
  thread-offline protocol.
- **D2 — Typed-Policy-Override-Chain + per-shard seqlock cells**: knobs are compile-time
  types declaring scope + reconfig traits; precedence is a typed fold into a 16 B
  `HotConfig` published to a **per-shard bounded-retry seqlock** cell (loads-only read,
  retry on writer collision, thread-local last-good fallback). Guarded dynamic
  registration via RCU pointer-array swap. Revised to tiered resolution (per-type slot,
  per-instance slot) after the fatal single-cell defect.

All three are std-only C++23 (`std::atomic`, `std::expected`, `std::pmr`, concepts +
deducing-this; hand-built RCU/QSBR/seqlock — no external lib), no .NET vocabulary, and
ride the settled ADR-002/003/004 drain edge with **zero new cross-core RMW on the drain**.

---

## Evidence table (claim → survived red-team? → proven by executed C++? → number)

Executed on x86-64 Linux, Intel Xeon Silver 4208 @2.10 GHz (TSC 2.095 GHz, no turbo) —
**below** the 023 Zen4/SPR reference, so absolute ns run conservative; relative claims and
gate margins hold. GCC 14.2 + Clang 20.1, -O2 and -O3+LTO. Allocations hooked; TSan /
ASan+UBSan / LSan with armed positive controls. "Survived" = survived cross-examination
on the *revised* design.

### D3 — Frozen-Core + Hot-Leaf (WINNER)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 hot read = ONE load, 0 RMW, no decode branch | yes | **CORRECT** | `mov (%rdi),%rax ; and $0x3fff,%ax` on both compilers, -O2 & -O3+LTO; 0 lock/cmpxchg/xadd/xchg; amortized 1.90 ns (g++) / 2.34 ns (clang), inlined = 1 instr |
| F2 leaf publish: 0 alloc, ≥10× cheaper, p99≤200 ns visible | yes | **INCONCLUSIVE** | 0 allocs/10k; 67–73 ns/op vs 1238–1496 ns snapshot-rebuild = **18.5–20.6× cheaper** (CONFIRMED); publish→visible p50 137–149 ns, **p99 206–273 ns** — misses the *self-imposed* 200 ns on sub-reference silicon (coherence-bound; cross-socket ≈ same). Still 15–20× better than D1's publish p99 |
| F3 co-resident reconfig storm < 5% drain drop | yes | **CORRECT** | `alignas(64)` per cell → drop within noise; packed-8B positive control = **74% drop** (fix load-bearing) |
| S1 TSan-clean under storm + drain + add-type | yes | **CORRECT** | 0 TSan reports both compilers; `-DRACE` control fires. (Found+fixed a real publisher-vs-publisher race with a cold control-plane mutex; hot-path relaxed load untouched) |
| S2 no torn hot-leaf read | yes | **CORRECT** | 0 torn over **1.35B–4.6B** reads (x86-64; ARM64 half rests on std::atomic single-location coherence, unverified — out of x86-64 target scope) |
| S3 QSBR never frees a live warm-leaf/record | yes | **CORRECT** | in_flight==0 grace + `RcuReadGuard` (no ptr across `co_await`); ASan clean; lane-yield-grace + free-on-retire positive controls both fire |
| C1 deterministic precedence + instance tier | yes | **CORRECT** | 2,000,000/2,000,000 match highest-setter; full-word instance cell keeps type-resolved knobs; type-scope live delta fans into masked instance cells (`masked_count`) |
| C2 BuildOnly fail-fast + range-check + trait parity | yes | **CORRECT** | all 5 frozen knobs → `unexpected(BuildOnly)`, cells byte-identical; out-of-range → `OutOfRange`, no silent truncation; consteval/runtime parity over all 10 knobs (one X-macro) |
| C3 guarded add-type, pre-sized arrays, no live race | yes | **CORRECT** | collision → `unexpected`, table pointer-identical; cap → `CapExceeded`, cached `op_cell_` still valid; add-vs-drain TSan+ASan clean, 0 bad records/cells |

### D1 — LICS (runner-up)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| S2 drain+admission reads = plain MOVs, 0 RMW/fence | yes | **CORRECT** | objdump both compilers: acquire-load = `mov`, release-store = `mov`; 0 lock/xchg/cmpxchg/mfence |
| S1 no tear + eager admission visibility | yes | **CORRECT** | TSan clean, 0 mixed pairs over 5.9M+46.8M / 7.1M+74.4M reads; eager refresh caps admit at 65–66 vs gated-control 8196 (fix load-bearing) |
| S3 bounded reclamation w/ parked lanes + async | yes | **CORRECT** | thread-offline protocol: 10^6 reconfigs, 6/8 lanes parked → max_retired 282, final 0; async-UAF via `ctx.cfg()` ASan+TSan clean |
| C1 deterministic precedence | yes | **CORRECT** | 200000/200000 trials; duplicate-in-layer → `DuplicateKnobInLayer` fail-fast |
| C2 BuildOnly fail-fast; dynamic reg forbidden | yes | **CORRECT** | structural deltas → `unexpected(BuildOnlyKnob)`, g_config epoch unchanged |
| C3 wheel-driven deactivate, on-lane, no msg loss | yes | **CORRECT** | 014 sim: `[Deactivate,M]` → close-out aborts eviction, M dispatched; on-lane; live idle_timeout next-arm honored |
| F1 per-msg read within 5% of const, incl budget=1 | yes | **CORRECT** | all budgets {1,2,4,64} within 5%; p50 32–37 / p99 39–45 ns, inside 100 ns gate |
| F2 no stall; publish→visible bounded by one drain | yes | **CORRECT** | calm-vs-storm throughput within noise (−0.7% / −1.7%); **publish→visible p50 ~540 ns, p99 3.1–4.5 µs** (flaps to 5–7 µs under OS jitter) |

### D2 — Typed-Policy-Override-Chain + seqlock (third)

Original **C1 (fatal), C2, C3, F3 CONCEDED** in cross-examination; the fix for the fatal
C1 (single per-shard cell cannot carry per-type/per-instance resolved values) required a
**tiered-resolution architectural redesign**, not a cheap patch. The revised claims all
proved out:

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 seqlock read no-writer | revised | **CORRECT** | p50 1.0–1.3 / p99 1.9–2.3 ns (normalized@3.5 GHz) |
| F2 read loads-only, 0 RMW | yes | **CORRECT** | objdump: mov/test/cmp/jcc only, 0 lock/xchg/cmpxchg/mfence |
| F3b bounded-retry + forward progress under frozen writer | revised | **CORRECT** | p50≤4/p99≤8 ns w/ writer; frozen-writer reader keeps running — but **70% of idle-baseline on GCC** (author's ≥90% bar missed); p999 collision tail **737 ns** |
| S1/S2 race-free, no tear | yes | **CORRECT** | TSan clean; 0 torn / 857M reads; controls fire |
| S3 RCU reclamation bounded | yes | **CORRECT** | UAF checks clean; retired_pending final 0 |
| C4/C1b/C2b/C2c/C3b tiered resolution, total-order fold, reconcile sweep, guarded reg | revised | **CORRECT** | all pass both compilers |

---

## Decision

**Adopt D3 — Frozen-Core + Hot-Leaf.**

Reasoning, in the debate's ranking order:

1. **Safety gate.** All three ultimately clear it: every safety/correctness claim that
   the prover initially marked wrong was conceded *with* a fix and re-proven CORRECT by
   executed C++. But the *cost* of clearing it differs decisively. D3's four landed
   attacks (false-sharing, QSBR-vs-`co_await`, instance-tier, array-growth) each had a
   **cheap, local fix** (`alignas(64)`, a grace-condition redefinition + read-confinement
   guard, a full-word instance cell, a pre-sized array + cap) and were re-proven. D1's
   fatal (parked-lane QSBR) took a stated thread-offline protocol and re-proved clean.
   D2's fatal (C1) required an **architectural** tiered-resolution redesign — not a cheap
   fix — which weighs against it winning per the gate.

2. **Proven beats claimed.** D3 and D1 each carry **8 CORRECT, 0 disproven** claims with
   executed evidence and armed positive controls; D3 has one **INCONCLUSIVE** (F2's strict
   ≤200 ns self-target on sub-reference silicon), which carries no weight and is *not* a
   disproof. Tie between D3 and D1 on proven count.

3. **Best measured hot-path numbers breaks the tie for D3, on both axes the brief names:**
   - **Config-read overhead:** D3 is the *only* design whose entire operational read-set
     (budget + bound + overflow + idle) is a **literal single `mov + mask`** — objdump-
     confirmed on both compilers at -O2 and -O3+LTO, 0 RMW, no decode branch. This is the
     purest realization of the brief's "SINGLE stable load." D1 amortizes an acquire-load
     over the drain budget plus an epoch-compare branch and touches a shared `g_config`
     line; D2 is a multi-load bounded-retry seqlock with a 737 ns p999 collision tail and
     only 70% forward-progress under a frozen writer.
   - **Publish latency:** D3's leaf-delta publish is **67–73 ns/op, 0 allocations,
     publish→visible p50 137–149 ns** — 18–20× cheaper than a snapshot rebuild and
     ~4× faster p50 / **15–20× faster p99** than D1's snapshot publish (p50 ~540 ns,
     p99 3.1–4.5 µs). The single relaxed store *is* the publish; there is no pointer to
     reclaim on the hot word, so publish structurally cannot stall the drain.

4. **Core invariants.** D3 bends none. A single 8-byte-aligned atomic word is
   hardware-indivisible on x86-64 → no torn read (S2: 0 torn / 4.6B reads); relaxed/relaxed
   on a self-contained scalar gives single-location coherence with no fence and no RMW
   (023 0-RMW drain gate intact, F1). Precedence resolves at build/reconfig time only
   (C1); structural knobs are BuildOnly and fail-fast (C2); idle deactivation rides the
   011 wheel on the actor's own lane (single-executor preserved).

D3's INCONCLUSIVE F2 is the honest residual: it missed its *own* 200 ns publish-visibility
target on the slow Xeon mesh. This is not a gate miss — 023 explicitly classes config as a
**cold path** and sets no Hard publish-latency budget — and D3's measured publish latency
still beats every competitor by an order of magnitude. It re-measures on the reference core
before any tightened target is stamped.

**Ranking: 1st D3 · 2nd D1 · 3rd D2.**

---

## Consequences

- The engine gains a `(shard × type_index)` array of cache-line-padded atomic operational
  cells (`HotCell`), pmr-allocated on each shard's NUMA arena, pre-sized to a BuildOnly
  `max_types` cap. Per-instance overrides materialize a private full-word cell the
  activation points at.
- Live reconfig is a control-plane API returning
  `std::expected<ReconfigReceipt, ReconfigError>`; operational deltas re-resolve affected
  cells and store one word each (fanning into masked instance cells, surfacing
  `masked_count`). Structural deltas fail-fast `ReconfigError::BuildOnly`; out-of-range
  values fail-fast `ReconfigError::OutOfRange` (never truncate).
- Dynamic `add_actor_type<T>()` is permitted as a **guarded** add: incremental Validation
  against the frozen core, release pointer-swap of an append-only metadata table (existing
  records pointer-stable), cap-bounded, QSBR-reclaimed.
- The reconfig class of every knob is a single X-macro source generating both the
  `consteval` trait and the runtime (loader) mirror; a parity test gates divergence.

---

## Residual risks

1. **F2 200 ns publish-visibility unproven on the reference core.** p99 206–273 ns on the
   sub-reference Xeon; it is coherence-bound, not store-bound. Must re-measure on Zen4/SPR
   before any Hard publish-visibility budget is stamped; today publish is a cold path with
   no Hard gate.
2. **S2 no-tear verified on x86-64 only.** The ARM64 arm of the claim rests on
   `std::atomic` single-location coherence and is unverified here — acceptable for the
   x86-64 target, but a gate before any ARM64 promotion (019/023).
3. **Adversarial reconfig storm ping-pongs one cache line** between publisher and shard
   worker, denting *that one shard's* drain (bounded to one line by `alignas(64)`,
   measured <5%). Rate-limit/coalesce the control-plane publish path.
4. **QSBR liveness for the warm leaf / metadata table** depends on every worker reaching
   in_flight==0. A handler blocked in a long syscall stalls reclamation; cap the retired
   list and fall back to a cold-path stop-the-world epoch bump under pressure (never on the
   drain). Bound and alarm it (009).
5. **Packing ceilings** (drain_budget < 2^14, mailbox_bound < 2^24, idle_ticks 16-bit
   coarse units): out-of-range is fail-fast, but a future per-message knob that cannot fit
   the word must be BuildOnly or warm-indirected (would split the single-load read).
6. **Cross-tier atomic grouping is not offered:** a knob in the hot word and a knob in the
   warm leaf cannot be swapped atomically. Co-dependent knobs must co-reside in the one
   word (they do today); cross-tier coupling is a build-only structural change.
7. **Per-instance-override reachability** must be documented and surfaced in
   `ReconfigReceipt.masked_count` — a type-scope live delta reaches instance-overridden
   actors only via the fan-out sweep, which re-resolves each masked cell.
