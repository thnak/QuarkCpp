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
