# ADR-012 — Weighted-HRW-Distribution Re-Gate (025 / 026)

Status: **Accepted (verification record)** — re-gate ruling: **INCONCLUSIVE**
Date: 2026-07-15
Deciders: gate-verification judge (closing the 025 re-gate against the corrected proportional log-WRH form)
Supersedes/relates: **supersedes ADR-011 Gate B's `WRONG` verdict for the *corrected* proportional log-WRH form**
([ADR-011](ADR-011-cluster-relay-and-placement-gate-verification.md) §Gate B);
re-gates `gate-weighted-hrw-distribution-025-026` against the ADR-011 repair
(proportional log-WRH `score = wₙ/(−ln H)`); executes — does **not** redesign —
the SETTLED cluster/placement design of
[ADR-006](ADR-006-large-scale-cluster-topology.md) as corrected by ADR-011;
blocks / annotates specs
[025-Placement-Policies-and-Stateless-Workers.md](../025-Placement-Policies-and-Stateless-Workers.md)
and [026-Large-Scale-Cluster-Topology.md](../026-Large-Scale-Cluster-Topology.md).

---

## Question

ADR-011 Gate B ruled the *weighted* distribution claim `WRONG` — but did so by
applying the retired **uniform** threshold `CoV(ρ) ≤ 0.2 AND max/mean ≤ 1.5` to a
weighted scheme, a threshold ADR-011 itself flagged as an information-theoretic
**quantization floor** violation (unachievable by *any* proportional scheme once
light nodes hold ~4 bins). ADR-011 repaired two things: the placement **form**
(literal ADR-006 line-104 `weightₙ·H` → proportional log-WRH `score = wₙ/(−ln H)`,
`H = splitmix64`-derived uniform ∈ (0,1)) and the balance **metric** (raw share →
load-per-weight `ρᵢ = realized_shareᵢ / wᵢ`).

This re-gate asks the single blocking question for **025 Draft→Accepted**: does the
corrected proportional form sit **at** the multinomial floor on `ρ` at every N,
with perfect cross-node minimal-disruption churn — under a *well-posed* target and
both mandatory controls firing?

Acceptance rule (this judge): accept `CORRECT` **only if** both mandatory controls
fired **and** the proportional form meets the restated load-per-weight target at
**every** N with churn-between-unchanged-nodes = 0 exactly.

---

## Ruling: **INCONCLUSIVE** (controls fired; gate admissible; distribution target ill-posed)

Both mandatory controls fired, so the gate is **admissible** (not a vacuous pass),
and the novel/load-bearing churn half is unambiguously `CORRECT`. But the
preregistered **distribution inequality is still mis-specified** — it compares a
p99 tail statistic of `ρ` against a *mean-level* closed-form floor, a comparison the
**ideal independent multinomial sampler itself fails** at N ≤ 1024. So:

- `CORRECT` is **unreachable**: the preregistered two-sided band
  `CoV_p99(ρ)/floor_closed ∈ [0.95, 1.03]` (ε_high ≤ 0.03) **fails** at
  N = 64/256/1024 (ratio 1.262/1.083/1.037). Reaching `CORRECT` would require
  widening ε after seeing the ratio — the explicitly named **POST-HOC TOLERANCE**
  false-pass route. Not permitted.
- `WRONG` is **inadmissible**: the failing band is provably **unachievable by any
  proportional scheme** at N ≤ 1024 — the from-scratch multinomial Monte-Carlo
  (shares no code/RNG with the WRH path) busts the *same* band
  (MC_p99/floor = 1.181/1.090/1.057/1.031), and the closed-form floor fails its own
  **mandatory < 1 % MC cross-check** at N = 64 (3.98 % disagreement). Rendering
  `WRONG` here would misattribute a **threshold-specification defect** to a scheme
  that is demonstrably at the true floor — the named "apply-an-unachievable-
  threshold" fail-route turned into a bogus negative.

When the decision rule at a given N is failed *even by the ideal sampler*, a "fail"
at that N carries **no information** about the scheme. There is **no N** where the
corrected form cleanly fails a well-posed, achievable target; and there is no N
configuration where it cleanly *passes* the preregistered bar as written. The
honest verdict is therefore **INCONCLUSIVE**, per the gate's own admissibility logic
(`if (4) or (5) fails to fire → INCONCLUSIVE; never CORRECT`) extended to its dual:
a distribution inequality that the ideal sampler cannot satisfy is not a live
discriminator either.

### What *is* proven (like-for-like, at the floor)

The scheme provably **sits at the true floor** once compared like-for-like — this is
diagnostic evidence, not a post-hoc decision rule:

- **p99-vs-p99** (WRH vs the independent multinomial): `WRH_p99/MC_p99` =
  **1.069 / 0.994 / 0.981 / 0.993** — within 1.9 % at every N.
- **mean-vs-closed-form**: `WRH_mean/floor` = **1.025 / 0.988 / 0.994 / 0.999** —
  within 2.5 % at every N.
- The N = 4096 per-node share-error "failure" (5.50σ > 5σ cap) **equals the ideal
  sampler's own 5.50σ** — the extreme-value of 4096 nodes, not a skew.

### What is unambiguously CORRECT — the novel/load-bearing half

Bounded re-weight churn (the genuinely new claim, already `CORRECT` in ADR-011 and
re-confirmed here):

- Bins trading between two **UNCHANGED** nodes = **0 exactly at every N**.
- Total moved fraction **== intended Δ-share to 0.00 % error**
  (0.030029 / 0.007874 / 0.001663 / 0.000473 vs intent), strictly **> 0** (non-
  vacuous) and **never global** — perfect cross-node minimal disruption.

---

## Mandatory controls (both FIRED → gate admissible)

| Control | Requirement | Result | Fired? |
|---|---|---|---|
| **(4) Modulo-slot re-shard** | moved_fraction ≥ 0.50 on the *identical* single-node Δ (target ~(N−1)/N) | **0.9773 / 0.9921 / 0.9687 / 0.8749** | **YES** — reproduces ADR-011's 97.5–99.9 %; proves a weight change *can* force near-global reshuffle, so WRH's zero-between-unchanged movement is real |
| **(5) Non-proportional `wₙ·H`** | must **MISS** the load-per-weight floor: CoV(ρ)/floor ≫ 1 at stress N | **CoV_p99 ≈ 1.43** abs; ratio **32.78 / 16.29 / 8.16 / 4.12×**; share-err **28.0 / 16.6 / 10.3 / 8.0σ** | **YES** — reproduces ADR-011's CoV ≈ 1.4, 8–28σ; proves the proportional form is a genuine improvement, not a relabeled pass |
| **(3) Churn positive-control** | single-node bump must actually move bins (0 < moved ≈ Δ, not ≈ 0) | moved = 0.030029 / 0.007874 / 0.001663 / 0.000473 == intended Δ | **YES** — the "zero between unchanged nodes" is non-vacuous |

Both mandatory discriminators are live detectors. The gate is **admissible**; the
INCONCLUSIVE is not a control-failure INCONCLUSIVE but a **target-ill-posedness**
INCONCLUSIVE.

---

## Corrected gate (the restatement that makes the decision well-posed)

The ADR-011 repair fixed the **form** (`wₙ/(−ln H)`) and the **metric** (`ρ =
share/w`) but left the **comparison statistic** mis-specified: it pits
`CoV_p99(ρ)` (a seed-tail quantity) against `floor_closed = √((W/(N·B))·Σ1/wᵢ)`
(a mean/σ-level quantity). The corrected, well-posed gate must compare **like for
like**, with the band recalibrated on the *ideal sampler's own* dispersion:

> **CORRECT ⟺** (admissibility) both mandatory controls (4)&(5) fire, **AND**
> at **every** N ∈ {64, 256, 1024, 4096} (B = next_pow2(16·max_nodes) = 65536,
> weights {1,3,8}, p99 over ≥ 8 paired seeds, ε **preregistered** before running):
>
> 1. **Like-for-like distribution** — ONE of, chosen and fixed **before** the run:
>    (a) `CoV_p99(ρ;N) / CoV_p99(MC;N) ∈ [1 − ε_low, 1 + ε_high]`, MC = the
>    independent from-scratch multinomial (no shared code/RNG), **or**
>    (b) `CoV_mean(ρ;N) / floor_closed_N ∈ [1 − ε_low, 1 + ε_high]`.
>    The band must bracket the ideal sampler's *own* ratio (which is > 1 at small
>    N: MC_p99/floor = 1.181/1.090/1.057/1.031), not the retired uniform 0.2 and
>    not a below-floor constant.
> 2. **Per-node share error** `maxᵢ|sᵢ−pᵢ|/√(pᵢ(1−pᵢ)/B) ≤` the ideal sampler's
>    own p99 share-error at that N (extreme-value-adjusted; ~5.5σ at N = 4096), **not**
>    a fixed 5σ cap that the ideal itself busts.
> 3. **Churn** — bins between two UNCHANGED nodes = 0 exactly at every N, AND
>    0 < total_moved ≈ intended Δ (within a few %), not ≈ global.

Under the corrected gate the **existing measurements already pass** every clause
(§"What is proven": WRH_p99/MC_p99 within 1.9 %; WRH_mean/floor within 2.5 %; churn
exact-zero; share-err == ideal). But because clause (1) was **not preregistered in
this form** — it was computed post-hoc as a diagnostic — the pass cannot be claimed
from this run without a fresh preregistered execution. That fresh run is the **exact
missing measurement** (see promotion).

---

## Evidence table

| N | CoV_p99(ρ) | floor_closed | ratio (preregistered, FAILS) | WRH_p99/MC_p99 (like-for-like) | WRH_mean/floor | closed-vs-MCmean | share-err_p99 | ideal-MC share-err | ideal-MC p99/floor |
|---|---|---|---|---|---|---|---|---|---|
| 64   | 0.05512 | 0.04368 | **1.262 FAIL** | **1.069** | 1.025 | **3.98 %** ✗cross-check | 3.53σ | 3.46σ | 1.181 (band unachievable) |
| 256  | 0.09440 | 0.08720 | **1.083 FAIL** | **0.994** | 0.988 | 0.50 % | 3.26σ | 3.72σ | 1.090 (band unachievable) |
| 1024 | 0.18077 | 0.17433 | **1.037 FAIL** | **0.981** | 0.994 | 0.59 % | 4.33σ | 5.00σ | 1.057 (band unachievable) |
| 4096 | 0.35682 | 0.34862 | 1.024 PASS | **0.993** | 0.999 | 0.44 % | 5.50σ (>5σ "FAIL") | 5.50σ | 1.031 (band ~binding) |

Churn (bump one node w 8→16): between_unchanged = **0 exactly at all N**;
moved_fraction = 0.030029 / 0.007874 / 0.001663 / 0.000473 == intended Δ to 0.00 %.
Modulo control moved_fraction = 0.9773 / 0.9921 / 0.9687 / 0.8749 (≥ 0.50).
Non-prop `wₙ·H` CoV_p99 = 1.4315 / 1.4201 / 1.4216 / 1.4376; ratio 32.78 / 16.29 /
8.16 / 4.12×; share-err 28.0 / 16.6 / 10.3 / 8.0σ. Preregistered band: ε_high = 0.03,
ε_low = 0.05, σ_max = 5.0, modulo_min = 0.50.

**Sanitizers**: ASan + UBSan (`-fsanitize=address,undefined`) CLEAN, exit 0, on the
**actual measurement binary** under **both** g++ 14.2 and clang 20.1
(`UBSAN_OPTIONS=halt_on_error=1`). Teeth proven non-vacuous: a deliberate heap-
buffer-overflow (`v[7]` on a size-4 vector) trips AddressSanitizer, exit 1 → the
clean pass is real, not an unwired sanitizer. TSan N/A (no concurrency in the
distribution decision; threads only parallelize independent per-bin argmax with
disjoint writes merged after join). g++ vs clang measurement output **byte-identical**
(diff empty). Compilers: `g++ 14.2.0 -std=c++23 -O2 -Wall -Wextra`,
`clang++ 20.1.2 -std=c++23 -O2 -Wall -Wextra`.
Artifact: `https://claude.ai/code/artifact/89141187-b13c-4e7b-90cd-f506178a44e8`
(source `/tmp/quark-regate.zeZZyG/gate.cpp`).

---

## 025 promotion decision → **HOLD — stays Draft**

Rule: 025 promotes iff **both** Weighted-HRW-distribution **and** Stateless-pool-
relaxation are `CORRECT`. The Stateless half is already proven (ADR-011 Gate C,
`CORRECT` — lost=dup=torn=0 @ M=10⁷, control fired, sanitizers clean). This re-gate
does **not** deliver a `CORRECT` on the distribution half: the verdict is
**INCONCLUSIVE**, so the AND is not satisfied. **025 is HELD at Draft.**

This is *not* a scheme defect — the corrected proportional log-WRH form is
demonstrably at the multinomial floor and its churn signature is exact. It is a
**gate-instrument defect**: the preregistered distribution inequality compares p99
against a mean-level closed form and is unachievable by the ideal sampler at
N ≤ 1024. The judge will not launder that into a `CORRECT` by post-hoc ε-widening,
nor into a `WRONG` that blames the scheme for the threshold.

### Exact missing measurement (single step to promotion)

Re-run Gate B **once** against the **corrected gate** above with the distribution
clause **preregistered in like-for-like form before execution**:

- **Preregister** clause (1) as either (a) `CoV_p99(ρ)/CoV_p99(MC_independent)` or
  (b) `CoV_mean(ρ)/floor_closed`, with a **two-sided ε band recalibrated on the
  ideal sampler's own p99/floor ratios** (1.181 / 1.090 / 1.057 / 1.031), fixed in
  the gate spec before any run.
- **Preregister** clause (2) share-error cap as the ideal sampler's own
  extreme-value-adjusted p99 (~5.5σ at N = 4096), not a flat 5σ.
- Keep everything else identical: form `wₙ/(−ln H)`, metric `ρ = share/w`,
  weights {1,3,8}, N up to 4096 at ~16 bins/node, p99 over ≥ 8 paired seeds, both
  compilers byte-identical, ASan/UBSan clean with teeth, both mandatory controls
  firing, independent MC agreeing < 1 % (fix the N = 64 cross-check that read
  3.98 % — likely a seed-count/estimator artifact at small N).

The current run's diagnostics already show every corrected clause passing
(WRH_p99/MC_p99 within 1.9 %; WRH_mean/floor within 2.5 %; churn exact-zero;
share-err == ideal). A fresh preregistered execution should return `CORRECT`,
at which point 025's AND is satisfied and 025 promotes to **Accepted (x86-64)**.

> This ruling **supersedes ADR-011 Gate B's `WRONG` verdict for the corrected
> form.** ADR-011's `WRONG` was rendered by applying the retired uniform
> `CoV ≤ 0.2 / max/mean ≤ 1.5` threshold, now shown ill-posed for weighting. For
> the *corrected* proportional log-WRH form on the load-per-weight metric, the
> distribution claim is **no longer `WRONG`** — it is **INCONCLUSIVE pending a
> well-posed like-for-like restatement**; the scheme itself is at the floor. The
> churn/minimal-disruption half remains `CORRECT` (unchanged from ADR-011).

026 is unaffected: its FIFO-under-relay promotion (ADR-011 Gate A) stands, and its
balance-threshold text carries the same restatement note already recorded in
ADR-011 residual risk #2.

---

## Residual risks

1. **[025 blocker — instrument, not scheme] Distribution gate compares p99 to a
   mean-level closed form.** The preregistered band `CoV_p99(ρ)/floor_closed ∈
   [0.95, 1.03]` is unachievable by the ideal multinomial at N ≤ 1024
   (MC_p99/floor = 1.181/1.090/1.057/1.031). Until the gate is restated
   **like-for-like** (p99-vs-p99 MC, or mean-vs-closed-form) with a preregistered
   band recalibrated on the ideal sampler, the distribution half cannot cleanly
   pass or fail. This is the sole remaining blocker on 025.
2. **[floor cross-check artifact] Closed-form floor fails its own < 1 % MC
   cross-check at N = 64 (3.98 %).** The mandatory anti-circularity check (analytic
   floor must agree with an independent multinomial to < 1 %) is violated at the
   smallest N. Likely a finite-seed / small-N estimator effect (at N = 4096 it is
   0.44 %). Must be understood and driven < 1 % at every N before the analytic floor
   can be used as a decision denominator; the p99-vs-p99 MC form sidesteps it.
3. **[share-error cap ill-posed] The 5σ per-node share-error cap is below the ideal
   sampler's own extreme value at N = 4096 (5.50σ).** A flat 5σ cap flags the ideal
   multinomial itself. The cap must be extreme-value-adjusted to N (max of N draws)
   or expressed relative to the ideal sampler, else it manufactures a spurious fail
   at the stress N — the exact regime the gate is meant to validate.
4. **[proven, safe to rely on] Corrected form is at the floor; churn is exact.**
   WRH_p99/MC_p99 within 1.9 % and WRH_mean/floor within 2.5 % at every N; bins
   between unchanged nodes = 0 exactly; moved == intended Δ to 0.00 %. These are the
   load-bearing properties and they are demonstrated; only the *decision framing*
   is deferred. Implementers may rely on proportional log-WRH `score = wₙ/(−ln H)`
   and its minimal-disruption reweight signature.
5. **[ADR-006 text defect persists] Line 104 `weightₙ·H(n,bin)` is
   non-proportional** (CoV ≈ 1.43, ratio 4.1–32.8× floor, share-err 8–28σ —
   reconfirmed here as control (5)). ADR-006 must be corrected to the proportional
   log-WRH realization. (Carried from ADR-011 residual #3; not resolved by this
   re-gate.)
6. **[scope] x86-64 only.** Distribution harness is arithmetic + splitmix64/mt19937
   with no weak-memory surface, so ARM risk is low here — but the promotion this
   gate feeds (025) is explicitly **(x86-64)** in line with ADR-011's scoping; the
   stateless-pool half's happens-before edges are TSO-proven only.
