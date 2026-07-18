# ADR-016 — Serialization wire fast-path encode budget + negotiation/evolution correctness: 016 promotion gate

Status: **Accepted (verification record)**
Date: 2026-07-15
Deciders: gate-verification judge (closing the single named 016 Draft→Accepted promotion gate)
Supersedes/relates: **executes — does not redesign** — the SETTLED serialization
design of [016-Serialization.md](../016-Serialization.md) (one reflection-free
`QUARK_SERIALIZE` describe → canonical tagged TLV for durable data + a
connect-negotiated tagless packed wire fast path; evolution via stable tags +
`QUARK_MIGRATE` chains). Stamps the wire fast-path encode budget owned by
[023-Performance-Targets-and-Budgets.md](../023-Performance-Targets-and-Budgets.md)
line 60 (**≤ 200 ns goal / ≤ 500 ns Hard**, near-memcpy). Mirrors the
scoped-promotion posture of [ADR-014](ADR-014-streaming-async-suspend-real-scheduler-gate.md)
and ADR-005: host-independent correctness + ratios PASS now; the tight absolute
goal-stamp defers to 023 reference silicon. Does not touch any other spec.

---

## Question

016's serialization mechanism is settled by design, but two things had **never been
executed**: (1) the near-memcpy wire-encode budget (023: ≤ 200 ns goal / ≤ 500 ns
Hard for the tagless fast path over a real small POD), and (2) the
negotiation/evolution correctness — that the connect-time fingerprint + ABI/endian
gate is actually *load-bearing* (mismatched tagless must demonstrably corrupt, not
merely "log a fallback"), that round-trip / additive-evolution / migration hold, and
that the whole codec is reflection-free under `-fno-rtti`. This gate executes both.

**The bar (decision inequality), in every one of the 4 build cells
{g++14.2, clang20.1} × {-O2, -O3}:** tagless encode of the named
`{u64 id; u32 qty; u32 flags; f64 price}` POD into a **caller-provided** buffer,
single-core pinned, **cold-buffer p99 ≤ 500 ns Hard** (report vs 200 ns goal);
**0 heap alloc** over ≥ 1e6 encodes; **reflection-free** (`-fno-rtti`, no
typeid/dynamic_cast, both compilers); round-trip identity for the tagless POD **and**
the tagged non-trivial `{u64,string,vector<Line>}` type over randomized values;
additive evolution **both** directions; a migration chain v→v+1→current on read; **AND
all three mandatory controls FIRED with reproduced effect** — (4) fingerprint-mismatch
forced-tagless corrupts, (5) endian/ABI-mismatch forced-tagless corrupts, (6)
budget-has-teeth two-sided (tagged measurably slower than tagless **and** tagless p99
within a small factor ≤ ~3–5× of same-byte-count memcpy). A control that did not fire,
or a benchmark that dodged the real POD / real store cost ⇒ **INCONCLUSIVE, not
CORRECT**.

---

## Gate ruling — `Serialization-wire-fast-path-encode-016` → **CORRECT**

Faithful, reflection-free implementation: one `QUARK_SERIALIZE(Type,(tag,member)...)`
macro expands to a single templated `quark_describe(Ar&,T&)` visitor instantiated
three ways from the same field list — TaggedWriter/TaggedReader (canonical LEB128
`key=(tag<<3)|wire_type` little-endian TLV, dispatch via plain function-pointer thunks,
no RTTI), TaglessWriter/TaglessReader (values in tag order, no keys, trivially-copyable
fields packed at fixed offsets into a caller buffer), and a `constexpr` FNV-1a
fingerprint folder over the `{(tag,wire_type)}` set. `negotiate()` selects tagless IFF
per-type fingerprint **and** `AbiTag(endian/ptr/layout)` both match, else canonical
tagged + a 009 warning. Named POD `sizeof = 24` (no padding), tagless encoded = 24 B,
tagged = 15 B. Benchmark pinned to core 0 (`taskset -c 0`); builds `taskset -c 0-3`,
sequential, `-j4` max. **All four cells pass.**

### Encode-budget evidence table — tagless wire-encode of the 24 B POD, COLD caller buffer, single core

Cold buffer = fresh 64 B cache line strided over 64 MB (> L3), so the real write-back
store cost is present (the warm-L1 dodge is closed). p99 is the Hard gate.

| Cell | tagless p50 | **tagless p99** | tagless p999 | vs 500 ns Hard | vs 200 ns goal |
|---|---|---|---|---|---|
| g++14 -O2 | 9.01 | **25.12** | 39.50 | ✓ (~20×) | ✓ |
| g++14 -O3 | 8.56 | **24.97** | 100.95 | ✓ (~20×) | ✓ |
| clang20 -O2 | 12.71 | **28.37** | 236.09 | ✓ (~18×) | ✓ |
| clang20 -O3 | 13.25 | **27.33** | 210.58 | ✓ (~18×) | ✓ |

Warm p50: g++ 4.6–4.7, clang 8.2–8.7 ns (supplementary). **K=1 conservative
cross-check** (per-op timer left in, ~24 ns rdtscp overhead **not** amortized): cold
p99 34–36 (g++) / 153–157 (clang) ns — still far under 500 Hard even with timer
overhead folded in, so the K=32 batching is not hiding a tail.

**Baseline A (memcpy, same 24 B, same harness, cold):** p99 **26.7–29.5 ns**. Tagless
near-memcpy factor = **0.85–1.06×** — at or below raw memcpy (well within ≤ 3–5×).
**Baseline B (canonical tagged encode, same POD, cold):** p50 14.3–23.2 vs tagless
8.6–13.3 → **tagged/tagless = 1.67–1.83× slower**, beyond measurement noise.

Ns numbers are per-core and fully valid under the single-core cap; the gate is
per-core by design (023). All four `DCE`-defeat guards hold: per-iteration varied input
via `DoNotOptimize`, output consumed into a volatile sink, and disassembly confirms the
encode body (incl. `field<double>` load) is emitted in the hot loop — the constant-input
/ dead-output elision route is closed.

### 0-alloc + reflection-free — MEASURED, not asserted

- **0 heap alloc:** global `operator new`/`new[]`/`delete` hooked and counted = **0 over
  1.2e6 tagless encodes in every cell**. Tagged POD path also 0 (disclosed). Buffers
  pre-provided, no `reserve`/`push_back`/`resize` inside the measured window — the
  caller-provided fixed buffer is written at fixed offsets by bulk copy, verified no
  reallocation.
- **Reflection-free:** `-fno-rtti` enforced on every build; no `typeid`/`dynamic_cast`
  anywhere; the codec-only TU (tagless + tagged encode, tagged decode, fingerprint)
  shows **0 RTTI symbols via `nm` on both compilers**. The only typeinfo symbols in the
  bench binary are `std::bad_alloc`/`std::exception` pulled in by the alloc-hook's
  `throw` — ABI-mandated exception RTTI, not `typeid`/`dynamic_cast`, and not from the
  codec. Disclosed and accepted.

### Round-trip + evolution + migration — ALL PASS (all 4 cells)

| Property | Oracle | Result |
|---|---|---|
| Tagless POD round-trip | encode→decode identity, 2e5 randomized values incl. NaN / −0.0 | **PASS** |
| Tagged non-trivial round-trip | `Order{u64, string, vector<Line>}`, 2e4 randomized values | **PASS** |
| Additive evolution — old skips unknown tag | old reader over new bytes | **PASS** |
| Additive evolution — new defaults missing tag | new reader over old bytes | **PASS** |
| Migration chain v1→v2→v3 on read | registered `QUARK_MIGRATE` upcaster | **PASS** |

Randomized full-range values (incl. edge/sign/NaN for the double) close the
constant-value round-trip route. ASan+UBSan correctness run clean, exit 0, both
compilers.

### Control outcomes — ALL THREE FIRED, reproduced effect

| Control | Injected condition | Required outcome | Observed | Fired? |
|---|---|---|---|---|
| **(4) fingerprint-mismatch** | force tagless between peers with an added/removed field (distinct fingerprint) | decode demonstrably CORRUPT | qty read-back `f01b866e` vs writer `aabbccdd` (wrong value) **+ ASan heap-buffer-overflow READ size 8 in `TaglessReader::field<double>`**, both g++ and clang | **YES** |
| **(5) endian/ABI-mismatch** | force tagless across a byte-swapped / different-padding ABI tag | decode demonstrably wrong values | id `0102030405060708` → `0807060504030201` byte-swapped wrong | **YES** |
| **(6) budget-has-teeth** (two-sided) | compare tagged vs tagless, and tagless vs memcpy | (a) tagged p50 clearly > tagless; (b) tagless p99 ≤ ~3–5× memcpy | (a) tagged/tagless **1.67–1.83×**; (b) tagless **0.85–1.06×** memcpy — BOTH hold, all cells | **YES** |

Each control is non-vacuous and load-bearing. (4) reproduces actual corruption
(wrong value **and** an ASan-caught out-of-bounds read), not a "would-fall-back" log —
the negotiation gate is proven to prevent silent corruption. **No fingerprint-collision
blindness:** the three schemas hash distinctly — `Pod=eac4545243089048`,
`PodB(+field)=2532c9d1258e57ff`, `PodEvo(+tag5)=8ca67d4d98d2afc7` — so (4)'s mismatched
peer is genuinely rejected, not accidentally matched. (5) proves the ABI/endian tag is
load-bearing. (6) is two-sided: the fast path is not vacuous (tagged is measurably
slower) **and** the near-memcpy claim is a measured ratio, not an assertion.

### Anti-cheat closure

Every named false-pass route is closed: real named 24 B POD with natural padding (not a
scalar toy); cold cache-strided destination gated on p99, not warm-only; DCE defeated
with disassembly proof; memcpy baseline A and tagged baseline B both measured on the
same rig/harness; controls 4/5 reproduce corruption affirmatively (value-diff + ASan),
not a decorative log; p50/p99/p999 distributions, gate on p99, not means; 0-alloc hooked
over 1.2e6 with both paths disclosed; `-fno-rtti` enforced + `nm`-confirmed on both
compilers; all 4 cells reported and passing (no single-compiler / single-O cherry-pick);
randomized full-range round-trip; distinct fingerprints; caller-provided fixed buffer
with no growth. **Rig: Xeon Silver 4208 @ 2.10 GHz, invariant TSC 2.095 GHz —
sub-reference** vs. the 023 ~3–4 GHz Zen4/SPR reference core (line 22). The
one-directional headroom argument is applied correctly: a **slower** box meeting the
500 ns Hard ceiling (~20× under here) implies the **faster** 023 reference silicon meets
it; the argument is never run backwards from a fast box to a slow one.

**Verdict: CORRECT.** Every conjunct of the decision inequality holds in all four build
cells, and all three mandatory controls fired non-vacuously with reproduced effect.

---

## 016 promotion decision → **PROMOTE — Accepted (x86-64)**

Rule: *016 promotes past Draft iff the wire fast-path encode gate is CORRECT with all
three controls firing.* It is. The near-memcpy tagless encode meets the 500 ns Hard
ceiling (and even the 200 ns goal) on a sub-reference box with ~20× headroom, at 0 heap
alloc, reflection-free under `-fno-rtti`; round-trip / additive-evolution / migration
all pass; and the connect-time negotiation is proven load-bearing by reproduced
corruption when forced past a fingerprint or ABI/endian mismatch.

**016-Serialization moves Draft → Accepted (x86-64).** The promotion is **scoped to
x86-64**. Two items defer, consistent with the ADR-014/005 posture (host-independent
correctness + ratios stamp now; tight absolute ns and non-x86 await their own re-gate):

- The tight **≤ 200 ns goal-stamp defers to 023 reference silicon** — exactly as 023's
  other absolute numbers defer. It was in fact *met* on this sub-reference box (p99
  25–28 ns), which only strengthens the headroom implication, but the formal goal-stamp
  is carried on the Zen4/SPR reference core with the rest of 023's absolute budgets.
- The **ARM64 / big-endian cross-peer path** is *exercised* here via the fallback
  control (5) — the endian/ABI tag correctly rejects a byte-swapped peer and corruption
  is reproduced when the gate is bypassed — but its **live promotion waits on the ARM64
  weak-memory / endianness re-gate**. The tagless bulk-copy is x86-ABI-shaped; a real
  ARM64 peer's layout/endianness and the canonical-tagged big-endian byte-swap boundary
  must be re-run on ARM hardware before any non-x86 promotion of the fast path.

---

## Residual risks

1. **[deferred — ≤ 200 ns goal-stamp on reference silicon]** The Hard 500 ns ceiling is
   stamped (met ~20× over on a sub-reference 2.10 GHz box; headroom is one-directional so
   the faster reference core also passes). The **tight 200 ns goal**, though numerically
   met on this rig, is formally carried to the 023 Zen4/SPR reference core with 023's
   other absolute latency budgets — not stamped by this x86-64-scoped promotion. This is
   why the promotion is scoped, not full Accepted.

2. **[deferred — ARM64 / big-endian cross-peer re-gate]** The endian/ABI tag is proven
   load-bearing (control 5 reproduces byte-swap corruption when bypassed), but only the
   x86-64 same-ABI tagless path and the little-endian canonical tagged path are executed
   here. A real ARM64 peer (different struct padding/alignment, AArch64 weak memory) and
   the big-endian byte-swap boundary of the canonical tagged form must be re-run on ARM
   hardware before the fast path promotes on non-x86. Deferred with the rest of ARM (no
   hardware); carries no weight against this x86-64 ruling.

3. **[disclosed — exception-RTTI symbols in the bench binary]** The codec TU is
   RTTI-free (`nm`-confirmed, 0 symbols, both compilers); the only typeinfo symbols in
   the *bench* binary are `std::bad_alloc`/`std::exception` from the alloc-hook's `throw`
   — ABI-mandated exception RTTI, not `typeid`/`dynamic_cast` and not from the codec.
   Accepted; any future codec change must keep the codec-only TU at 0 RTTI symbols.

4. **[not re-gated here — durable-path breadth]** This gate proved the *wire* fast path
   plus the tagged round-trip / additive-evolution / migration mechanics on the named
   types. The full durable surface 016 also serves — snapshot/event-log record headers
   `{type_key, schema_version}` (012), the Validation-error path for using a non-described
   type in a persistent actor (008), and long-horizon multi-hop migration chains — is
   consistent with the described mechanism but was not the subject of this encode-budget
   gate; it is owned by 012/008 verification, not re-opened here.
