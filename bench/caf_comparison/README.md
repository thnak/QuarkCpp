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

| Producers | Quark (M/s) | CAF (M/s) | CAF vs Quark |
|---|---|---|---|
| 1 | 2.72 | 4.31 | 1.6× |
| 2 | 2.64 | 8.37 | 3.2× |
| 4 | 1.70 | 12.99 | 7.6× |
| 8 | 1.09 | 19.23 | 17.6× |
| 12 | 0.84 | 21.58 | 25.7× |

### Key insight

Quark's Vyukov MPSC mailbox uses a single `tail_.exchange(acq_rel)` that serializes all producers on one cache line — throughput *drops* with more producers. CAF scales up, suggesting a per-thread buffer or split-queue design.
