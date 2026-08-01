# Quark vs CAF Benchmark Suite

Compares **Quark v0.1.0** against **CAF v1.1.0** (C++ Actor Framework) on latency and throughput.

## Prerequisites

- **Quark** — built from the parent project (`D:/GitSrc/QuarkCpp/build/quark.lib`)
- **CAF** — cloned and built at `C:/Users/thanh/AppData/Local/Temp/actor-framework/build/`
- **clang++** (C++20/23), **Ninja**, **CMake** ≥ 3.24

## Build

```bash
mkdir -p build/caf_comparison
cd build/caf_comparison
cmake ../../bench/caf_comparison -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -j8
```

## Run

### Quick suite (all benchmarks)

```bash
run_all.bat
```

### Individual benchmarks

All accept a CLI argument for thread count or producer count:

| Command | What it measures |
|---|---|
| `quark_bench.exe 1` | Quark @ 1 worker (single-threaded baseline) |
| `quark_bench.exe 0` | Quark @ max workers (12 on this machine) |
| `caf_bench.exe 1` | CAF @ 1 thread (fair comparison) |
| `caf_bench.exe 0` | CAF @ max threads (12) |
| `quark_mpsc_bench.exe 4` | Quark MPSC with 4 producers → 1 actor |
| `caf_mpsc_bench.exe 4` | CAF MPSC with 4 producers → 1 actor |
| `quark_mpsc_sharded_bench.exe 4` | Quark, 4 producers → 4 actors (1 producer per actor — contention-free ceiling) |
| `caf_mpsc_sharded_bench.exe 4` | CAF, 4 producers → 4 actors (same shape) |

## Benchmarks

| Benchmark | Description |
|---|---|
| **Spawn** | Time to create 10,000 actors (ns/spawn) |
| **Tell** | Fire-and-forget message latency (p50/p99/p999 ns) |
| **Ask** | Request-response latency (p50/p99/p999 ns) |
| **Throughput** | Bulk fire-and-forget (M msg/s) |
| **MPSC scaling** | Throughput with N producers → 1 actor (M msg/s) |

## Results (Ryzen 5 4600H, Windows, clang++ 22.1.5)

### Single-threaded (1 worker each)

| Metric | Quark | CAF | Winner |
|---|---|---|---|
| Spawn | **613 ns** | 880 ns | Quark 1.4× |
| Tell p50 | 200 ns | **100 ns** | CAF 2× |
| Tell p999 | 41,800 ns | **600 ns** | CAF 70× |
| Ask p50 | **1,200 ns** | 2,300 ns | Quark 1.9× |
| Throughput | 3.42 M/s | **5.14 M/s** | CAF 1.5× |

### MPSC scaling

**Correction (2026-07-30):** the table below (2026-07-27) was investigated as GitHub issue #4 and
led to a real fix — `MessagePool` gained an optional `num_partitions` ctor arg (commit `1964203`,
2026-07-28) that spreads producer-side free-list contention across independent partitions instead
of one shared mutex. But `quark_mpsc_bench.cpp` still constructed its pool as `MessagePool
pool{4096}` — a single-arg call, so `num_partitions` silently defaulted to `1` (the library's
back-compat default) — meaning **this bench never actually exercised the fix it prompted**. Fixed
to `MessagePool pool{4096, num_producers}`, matching how the partitioning feature is meant to be
used (one partition per producer lane). Also: the original 1/2/4/8/**12** sweep exceeds this
repo's machine-safety rule (CLAUDE.md caps multi-thread stress at 4 threads on this box), so the
refreshed sweep below stops at 4 — the 8/12 rows are left as historical, now-stale data, not
re-verified.

| Producers | Quark, stale/unpartitioned (M/s) | Quark, fixed (M/s) | CAF (M/s) | CAF vs Quark (fixed) |
|---|---|---|---|---|
| 1 | 2.72 | 1.90 | 3.99 | 2.1× |
| 2 | 2.64 | 3.15 | 8.66 | 2.75× |
| 4 | 1.70 | 5.76 | 13.39 | 2.3× |
| 8 *(stale, unverified)* | 1.09 | — | — | — |
| 12 *(stale, unverified)* | 0.84 | — | — | — |

The direction reverses: Quark's fixed throughput now **scales up** with producer count (1.90 →
3.15 → 5.76 M/s) instead of dropping, and the CAF gap at 4 producers shrinks from 7.6× to 2.3×.
The 1-producer number moved too (2.72 → 1.90, run-to-run machine variance — neither run was
core-pinned); treat the ratios, not the raw single-thread values, as the signal until a pinned
re-run exists.

**Update (2026-07-30, ADR-035):** the remaining gap was root-caused to a second, independent
issue — `Engine::worker_loop` parked the instant its scan found nothing, with no backoff, so
producers landing on an already-parked worker paid a real OS wake syscall (futex/`WaitOnAddress`)
synchronously inside their own `tell()` call. Measured directly via `metrics_snapshot().wakeups`:
31.0%/15.8%/4.4% of sends at 1/2/4 producers were paying that cost. A full design-debate-prove
cycle (`decisions/ADR-035-worker-park-wake-backoff-policy.md`) picked a bounded 256-iteration
pre-park spin (`EngineConfig::pre_park_spin_limit`, default 256) as the winner over a
higher-throughput but worse-worst-case adaptive alternative. Re-measured in the same session,
same host, with both fixes (partitioned pool + ADR-035 spin) live:

| Producers | Quark (partition fix only) | Quark (+ ADR-035 spin) | CAF (this session) | CAF vs Quark |
|---|---|---|---|---|
| 1 | 1.90 M/s | 2.24 M/s | 2.90 M/s | 1.3× |
| 2 | 3.15 M/s | 4.00 M/s | 5.27 M/s | 1.3× |
| 4 | 5.76 M/s | 6.31 M/s | 8.53 M/s | 1.35× |

The gap tightens further, from ~2.1–2.75× down to a consistent ~1.3×. CAF's own numbers moved
between sessions too (this run: 2.90/5.27/8.53 vs the earlier 3.99/8.66/13.39) — unpinned
run-to-run noise on a shared dev box, not a real change in CAF — so the *ratio* within one session
is the trustworthy signal, not the cross-session absolute deltas. Tell p999 did **not** improve on
this host in the ADR-035 prove phase (both the winning and rejected designs measured
statistically-indistinguishable p999 vs baseline here) — flagged as unproven/host-noise in the ADR,
not claimed as fixed.

### Key insight

Two independent, now-fixed bottlenecks accounted for most of the original gap: (1) `MessagePool`'s
single shared free-list mutex serializing every producer regardless of target actor (root-caused
in commit `1964203`, GitHub issue #4) — fixed by per-producer-lane partitioning; (2)
`Engine::worker_loop` paying a real OS wake syscall on ~15-31% of sends because it parked
immediately on the first empty scan with no backoff — fixed by ADR-035's bounded pre-park spin.
CAF still leads in absolute throughput at this producer range (~1.3× now, down from ~2.1-2.75×),
which remains open — unexplored: CAF's own producer-side design, and Quark's shard-keyed pool
follow-up noted in `message_pool.hpp`'s file banner, plus ADR-035's own recommended round-2 hybrid
(EWMA-gated near-zero idle cost + the tighter fixed-spin shape) for a further throughput gain
without the adaptive design's measured ~380µs worst-case tail.

**Update (2026-07-31, ADR-036):** the follow-up round targeted a THIRD bottleneck the OS-wake fix
didn't touch — `metrics_snapshot().activations` (Idle->Scheduled exec-state transitions) stayed at
18-25% of messages even after ADR-035, because the mailbox drains to empty and reactivates every
~4-5 messages under a tight producer loop, each cycle paying a CAS + run-queue enqueue + idle_mask_
scan regardless of whether an OS wake syscall fires. A design-debate-prove round (
`decisions/ADR-036-activation-linger-idle-churn-reduction.md`) shipped a bounded, activation-scoped
post-drain linger (`EngineConfig::activation_linger_spin_limit`, default 32): before an activation
commits to the Running->Idle transition on an empty mailbox, it re-polls its OWN mailbox a bounded
number of times first, and if a message lands during that window it's picked up directly with no
exec-state transition at all. **Proven, honest result: this helps substantially under sustained
backlog/contention (+26.4% throughput in the prover's dedicated 3-producers-vs-1-worker stress
test, activations 23.98%->1.56%) but does NOT reliably help — and does not meaningfully move — the
kind of single/light-producer, mostly-idle traffic this bench suite's own MPSC/throughput
benchmarks exercise.** Re-measured on the same pinned Linux host, same session:

| Producers | Quark (post-ADR-035) | Quark (+ ADR-036 linger) | Change |
|---|---|---|---|
| 1 | 0.99 M/s | 1.13 M/s | +14%, within run-to-run noise on this host |
| 2 | 2.32 M/s | 2.25 M/s | flat |
| 4 | 3.12 M/s | 3.22 M/s | +3%, within noise |

Single-threaded Tell/throughput (1 worker, `taskset -c 0`) similarly barely moved (throughput
1.06->1.11 M/s, Tell p50 80->81ns, p999 22,855->24,317ns) — expected, since a single producer in a
tight loop against an otherwise-idle actor is close to the "sparse/idle-ish" traffic regime
ADR-036's own proof found the linger does NOT help (F1: 93.10% vs 92.31% baseline activations at
that pacing, essentially no reduction). **The lesson, stated plainly: ADR-036 is a real fix for a
real problem (sustained backlog), but this bench suite's specific shape (single/light producers
against a fast-draining actor) isn't that problem** — closing the remaining CAF gap in *this*
benchmark shape needs a different angle than activation-churn reduction. A rejected sibling design
(cutting the fixed per-transition cost via cache-line isolation) was proven NOT to help either — the
false-sharing mechanism is real in isolation (4.45x in a synthetic control) but swamped by
park/wake round-trip cost in the real engine (-1.6%, i.e. the wrong direction, against a
pre-declared 3%-improvement bar).

**Correction (2026-07-31, ADR-036 round 3):** the table and single-threaded numbers above were
measured against ADR-036's ROUND-1 design — an unconditional linger that always spun the full
bound. That design was subsequently found to catastrophically regress this repo's own
`activation_bench`/`sched_bench` gate benches (12-17x, zero-concurrency case the round-1 proof never
exercised) and was replaced by an evidence-gated adaptive bound (`decisions/ADR-036-...md`'s round-3
section). The adaptive mechanism's effective spin is 0 whenever an activation hasn't recently seen
real batch evidence — a strictly *more* conservative floor than round-1's always-spin behavior — so
the numbers above should, if anything, move no worse than flat/within-noise for this bench suite's
own single/light-producer shape (the same regime round-1 already showed near-zero effect for), but
this has **not been re-measured** against the adaptive code and should not be assumed without
re-running. Flagged as a follow-up bench task, not a blocker for the adaptive fix itself (which
exists to correct a regression, not to move these particular numbers).

**Correction (2026-07-31, ADR-036 round 4): the sustained-backlog win cited above (+26.4%,
23.98%->1.56%) is retracted, not carried forward.** Round 4 fixed the harness that produced those
round-1 numbers (its `drain_budget` let a single `drain_step` call absorb an entire backlog wave,
masking any real per-config difference) and, with that fixed plus a second warm-up/process-order
measurement bias also closed, re-measured the contention win for the adaptive mechanism as shipped:
**no statistically distinguishable benefit** over `linger_spin_limit=0`, cross-validated under g++
and clang++, P∈{1,2,3,4} producers (`decisions/ADR-036-...md`'s round-4 section has the full data).
`activation_linger_spin_limit`'s default is now **0** — this bench suite's own Producers-1/2/4
table above and the single-threaded Tell numbers were already measured on a build where the linger
barely moved anything for this traffic shape; with the default now 0, a fresh run of this table
would exercise byte-for-byte pre-ADR-036 `drain_step` and should reproduce the original
`post-ADR-035` column, not the `+ADR-036 linger` one. Not re-run here — flagged as the same
follow-up bench task as before, now with a clearer expected outcome.

**Update (2026-07-31, re-measured with `activation_linger_spin_limit` default = 0):** the follow-up
above is now run. Rebuilt `quark_bench.exe`/`quark_mpsc_bench.exe` from current master (commit
`a3d66c2`, the merge that sets the default to 0) with `ninja -j4` (ADR-035's `pre_park_spin_limit`
is untouched by this change, still default 256), and reran `caf_bench.exe`/`caf_mpsc_bench.exe` in
the same session, same unpinned Windows host, per this file's own single-session-ratio methodology.
Ran the full MPSC sweep twice back-to-back to sanity-check within-session stability:

| Producers | Quark (session run 1) | Quark (session run 2) | CAF (session run 1) | CAF (session run 2) |
|---|---|---|---|---|
| 1 | 1.90 M/s | 2.07 M/s | 3.77 M/s | 4.64 M/s |
| 2 | 3.15 M/s | 3.69 M/s | 6.79 M/s | 7.87 M/s |
| 4 | 5.73 M/s | 6.12 M/s | 11.19 M/s | 11.97 M/s |

Single-threaded (1 worker), run 1 shown (run 2 in parentheses, same ballpark):

| Metric | Quark | CAF |
|---|---|---|
| Spawn | 912.4 ns (1069.6 ns) | 1037.8 ns (1080.7 ns) |
| Tell p50 | 300.0 ns (400.0 ns) | 200.0 ns (100.0 ns) |
| Tell p999 | 49,800 ns (51,000 ns) | 1,200 ns (700 ns) |
| Ask p50 | 900.0 ns (800.0 ns) | 2,000 ns (2,200 ns) |
| Throughput | 2.21 M/s (2.50 M/s) | 4.19 M/s (4.54 M/s) |

**Did the prediction hold?** Partially, and the split is informative. Quark's own MPSC numbers this
session (1.90–2.07 / 3.15–3.69 / 5.73–6.12 M/s) land close to the prior `post-ADR-035` column
(2.24 / 4.00 / 6.31 M/s) — within the same noisy Windows-host band already documented above, and
consistent with the prediction that disabling the linger reproduces the pre-ADR-036 code path.
**But CAF's own throughput this session (3.77–4.64 / 6.79–7.87 / 11.19–11.97 M/s) came in 30–55%
*above* its own ADR-035-session numbers (2.90 / 5.27 / 8.53 M/s)** — and CAF's binary is completely
untouched by this config change, so that jump is pure cross-session host noise on this shared,
unpinned box, exactly the kind this file has repeatedly flagged. Net effect: the CAF-vs-Quark ratio
this session is a consistent **~2.0×** at every producer count (1.98× / 2.16× / 1.95× run 1;
2.24× / 2.13× / 1.96× run 2) — not the ~1.3× measured in the ADR-035 session. Since Quark's side
roughly reproduced and CAF's side did not, the ratio shift looks attributable to CAF-side host
noise, not a Quark regression from the config change — but this Windows session alone can't prove
that cleanly; the pinned Linux re-run below is the more trustworthy check for it. Single-threaded
Tell p999 stayed just as noisy as before (49.8–51.0 µs for Quark, both a `post-ADR-035`-era and a
pre-ADR-036-era number sit inside that spread) — no new signal there either way.

## Results (Linux/WSL2, g++ 15.2.0, `taskset`-pinned, ADR-035 + partitioned pool live)

All numbers above are from an unpinned, shared Windows dev host — genuinely useful for *ratios*
(as noted throughout), but not trustworthy at the sub-microsecond scale the Tell latency numbers
live at. This section is a from-scratch, `taskset -c 0-3`-pinned Linux run (WSL2/Ubuntu, real
g++ 15.2.0, CAF v1.1.0 built from source at the same commit `0378ece` the Windows numbers used) —
the same methodology `CLAUDE.md`'s machine-safety rules prescribe for real benchmarking. **Caveat:**
WSL2 is itself a lightweight VM, not bare metal — its futex/scheduling costs are not guaranteed to
match a native Linux CI runner; treat this as a second, more-trustworthy-than-Windows data point,
not a final answer.

### Single-threaded (1 worker, `taskset -c 0`)

| Metric | Quark | CAF | Note |
|---|---|---|---|
| Spawn | **1,310.8 ns** | 1,597.2 ns | Quark wins, consistent with Windows |
| Tell p50 | 80 ns | 70 ns | **~parity** — the Windows "2×" gap was measurement noise, not real |
| Tell p999 | 22,855 ns | **2,635 ns** | CAF wins, real (~8.7×) — much smaller than Windows' noisy 70×, but genuine |
| Ask p50 | 13,076 ns | 12,205 ns | ~parity |
| Throughput | 1.06 M/s | **4.31 M/s** | CAF wins, ~4× — the real, substantiated gap |

### 4 workers (`taskset -c 0-3`)

| Metric | Quark | CAF |
|---|---|---|
| Spawn | **1,032 ns** | 5,529.4 ns (Quark wins big) |
| Throughput | 0.94 M/s | 2.06 M/s (CAF ~2.2×) |

### MPSC scaling (`taskset -c 0-3`)

| Producers | Quark (M/s) | CAF (M/s) | CAF vs Quark |
|---|---|---|---|
| 1 | 0.99 | 1.85 | 1.87× |
| 2 | 2.32 | 3.07 | 1.32× |
| 4 | 3.12 | 7.81 | 2.50× |

Quark still scales *up* with producer count on Linux (0.99 → 2.32 → 3.12), confirming the
partitioning + ADR-035 fixes hold cross-platform, not just on the Windows host they were developed
on. The ratio pattern differs from the Windows session (a flat ~1.3× at every producer count) —
here it's non-monotonic (1.87× → 1.32× → 2.50×), plausibly a WSL2-specific virtualization/core-
topology artifact rather than a Quark-specific regression, but not yet root-caused.

**Corrected picture**: with pinned data, Quark and CAF are close on *latency* (spawn, tell p50,
ask) — the large Windows-reported gaps there were mostly measurement noise. The real, reproducible
gap is **bulk throughput** (single-producer ~4×, 4-worker ~2.2×, MPSC 1.3–2.5× depending on
producer count) and **Tell p999 tail latency** (~8.7×, smaller than first thought but real). This
narrows what's actually worth investigating next: not "Quark's messaging is slow," but
specifically "Quark's sustained bulk throughput and tail latency trail CAF's" — a much more
targeted question than the original benchmark suite suggested.

**Update (2026-07-31, re-measured with `activation_linger_spin_limit` default = 0):** this is the
follow-up the ADR-036-round-4 correction note (above, in the Windows section) predicted for this
pinned table specifically — its `post-ADR-035` column is the Linux data this note refers to.
**Methodology / what was found:** no reusable `caf_comparison` CMake build directory existed on
WSL2 for this repo's own checkout, but a prior CAF build was found and reused, as instructed. Three
WSL2 checkouts of QuarkCpp existed (`~/QuarkCpp`, `~/work/QuarkCpp`, `~/build-verify/QuarkCpp`);
`~/QuarkCpp` tracked `origin` cleanly and was fast-forwarded to current master (`a3d66c2`, moving
one stray untracked test file aside first so the merge wasn't blocked — no content was discarded,
the incoming commit already supersedes it). CAF was already built at
`~/quark-caf-linux/caf/build`, confirmed still at commit `0378ece` — the *same* commit the original
pinned run used, so this is a same-CAF-binary, updated-Quark-headers comparison, not a new CAF
build. No `caf_comparison` CMake project was reused (none was found configured for this pair of
paths); the four binaries were recompiled directly with `g++ 15.2.0 -std=c++23/-std=c++20 -O3
-DNDEBUG`, mirroring the committed `CMakeLists.txt`'s settings (`Release` → `-O3 -DNDEBUG` for GCC),
linked against `libcaf_core.so` from that existing CAF build. Confirmed
`activation_linger_spin_limit = 0` in the rebuilt headers before running. All runs `taskset`-pinned
exactly as the original pinned session (`-c 0` single-threaded, `-c 0-3` for 4 workers / MPSC — the
4-worker runs pass an explicit `4`, not `0`; passing `0` resolves to
`std::thread::hardware_concurrency()` (12 on this box) which would spawn 12 OS threads and violate
this repo's machine-safety cap even under `taskset` CPU pinning, so `0` was avoided).

### Single-threaded (1 worker, `taskset -c 0`) — re-measured

| Metric | Quark | CAF | vs original pinned run |
|---|---|---|---|
| Spawn | 1,796.8 ns | 2,101.4 ns | both higher than original (1,310.8 / 1,597.2 ns), same ratio ballpark |
| Tell p50 | 81.0 ns | 80.0 ns | ~parity, matches original (80 / 70 ns) |
| Tell p999 | 84,071 ns | 3,466 ns | both higher than original (22,855 / 2,635 ns) — WSL2 run-to-run noise, CAF still wins, same order of magnitude |
| Ask p50 | 14,828 ns | 17,854 ns | close to original (13,076 / 12,205 ns) |
| Throughput | 0.94 M/s | 2.81 M/s | both lower than original (1.06 / 4.31 M/s); ratio narrower this run (~3.0× vs ~4.1×) |

### 4 workers (`taskset -c 0-3`, explicit `4`, not `0`) — re-measured

| Metric | Quark | CAF | Original pinned run |
|---|---|---|---|
| Spawn | 2,255.7 ns | 8,601.5 ns | 1,032 ns / 5,529.4 ns |
| Throughput | 0.71 M/s | 2.19 M/s | 0.94 M/s / 2.06 M/s |

### MPSC scaling (`taskset -c 0-3`) — re-measured, this is the direct test of the ADR-036-round-4 prediction

| Producers | Quark (M/s) | CAF (M/s) | CAF vs Quark | Predicted `post-ADR-035` column | Predicted `+ADR-036 linger` column |
|---|---|---|---|---|---|
| 1 | 1.02 | 1.84 | 1.80× | 0.99 | 1.13 |
| 2 | 2.12 | 5.27 | 2.49× | 2.32 | 2.25 |
| 4 | 3.03 | 6.62 | 2.18× | 3.12 | 3.22 |

**Did the prediction hold?** Mostly, but not cleanly, and it's worth being precise about why.
Quark's own re-measured numbers (1.02 / 2.12 / 3.03 M/s) sit close to the predicted `post-ADR-035`
column (0.99 / 2.32 / 3.12 M/s) — within about 9% at the widest (producer=2), which is inside the
noise band this table's own original caveats already allow for on WSL2. That's consistent with —
not contradicted by — the prediction that `linger=0` reproduces byte-for-byte pre-ADR-036
`drain_step` behavior. **However**, the original ADR-036 text itself already found the
`post-ADR-035` and `+ADR-036 linger` columns statistically indistinguishable at this traffic shape
(+14%/flat/+3%, all called "within run-to-run noise" in that same section) — so a single fresh run
landing near one predicted column doesn't, by itself, cleanly rule out the other; both predicted
columns are within a few percent of each other and within this run's own noise margin. **CAF's own
throughput moved far more than Quark's** — especially at 2 producers (5.27 M/s here vs 3.07 M/s in
the original pinned run, +72%) — which is the more striking result: since neither this Quark config
change nor anything else here touches the CAF binary, that's WSL2/host scheduling noise dominating
CAF's number more than Quark's this run. This reinforces, on the Linux side too, the file's
recurring point: **the ratio's stability matters more than any single absolute figure**, and here
even the ratio is noisy (1.80×–2.49× across 1/2/4 producers, vs the original pinned run's
1.32×–2.50×) — not a clean confirmation of a specific number, but nothing in this data contradicts
"linger defaulting to 0 reproduces the pre-ADR-036 code path" either. A cleaner test would need
either several repeated pinned runs per config (to shrink the noise band below the two predicted
columns' own ~3% separation) or a metric less sensitive to OS scheduling noise than wall-clock
throughput (e.g. `metrics_snapshot().activations` churn directly, as ADR-036's own prover did).

## Sharded MPSC: is the CAF gap mailbox contention, or something else?

Every MPSC number above (`quark_mpsc_bench`/`caf_mpsc_bench`) uses the shape "N producer threads →
1 actor, 1 shared mailbox" — the worst case for contention. `quark_mpsc_sharded_bench.exe` /
`caf_mpsc_sharded_bench.exe` (added 2026-07-31) isolate that variable: N producer threads → **N
actors**, `producer[i]` only ever `tell()`s `actor[i]`, one shard/worker per producer on the Quark
side (`EngineConfig{workers=N, shards=N, ...}`) and N scheduler threads on the CAF side. No two
producers ever contend for the same mailbox. Same per-producer message volume and pacing as the
shared-mailbox bench, so the two are directly comparable — this is the contention-free throughput
ceiling the shared-mailbox numbers should be compared against.

**Results (Windows, unpinned, same host/session as the table above, default `activation_linger_spin_limit=0`):**

| Producers | Quark, shared mailbox | Quark, sharded (N actors) | Δ | CAF, shared mailbox | CAF, sharded (N actors) | Δ |
|---|---|---|---|---|---|---|
| 1 | 1.90–2.07 M/s | 1.91 M/s | ~flat | 3.77–4.64 M/s | 4.33 M/s | ~flat |
| 2 | 3.15–3.69 M/s | 3.24 M/s | ~flat | 6.79–7.87 M/s | 6.86 M/s | ~flat |
| 4 | 5.73–6.12 M/s | 6.00 M/s | ~flat | 11.19–11.97 M/s | 11.08 M/s | ~flat |

**Removing mailbox contention entirely did not move the numbers** for either framework, at this
producer range, on this host. That's a real (if negative) result, not a wash: it says the
shared-mailbox MPSC numbers throughout this file are not primarily contention-bound at P≤4 — the
per-thread `tell()`/enqueue cost and OS scheduling overhead dominate over mailbox CAS/free-list
contention at this scale. It also means the CAF-vs-Quark gap documented throughout this file
(~1.3×–2.5× depending on session/platform) is **not explained by Quark's mailbox contending worse
than CAF's under load** — sharding away all contention left the same gap in roughly the same place.
Consistent with the "Contention factor" the CAF benches already print (throughput vs their own
1-thread baseline): the bottleneck these benches are measuring looks like it sits in per-message
enqueue/dispatch cost, not lock/CAS contention on a shared queue — matching this file's own "Key
insight" section above, which already root-caused two *other* bottlenecks (free-list mutex,
park/wake syscalls) rather than mailbox contention itself.

**Caveat:** this is a single Windows session, unpinned, same noise caveats as every other Windows
table in this file — not yet re-run on the `taskset`-pinned WSL2/Linux host the way the shared-
mailbox numbers were. Given the effect (or lack of one) is large and consistent across all three
producer counts and both frameworks, it's unlikely to be pure noise, but a pinned Linux confirmation
run is the natural follow-up, same as every other number in this file.
