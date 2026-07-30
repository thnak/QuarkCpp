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

### Key insight

The original "throughput drops with more producers" finding was **not** a Vyukov mailbox
limitation — it was `MessagePool`'s single shared free-list mutex serializing every producer
regardless of target actor (root-caused in commit `1964203`, GitHub issue #4). With one partition
per producer lane, Quark's mailbox scales in the right direction; CAF still leads in absolute
throughput at this producer range, which remains open (unexplored: per-thread buffer or
split-queue design on CAF's side, and Quark's own shard-keyed pool follow-up noted in
`message_pool.hpp`'s file banner).
