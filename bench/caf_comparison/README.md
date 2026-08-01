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
ninja -j4
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
| `quark_mpsc_bench.exe N` | Quark MPSC, N producers → 1 actor (shared mailbox) |
| `caf_mpsc_bench.exe N` | CAF MPSC, N producers → 1 actor (shared mailbox) |
| `quark_mpsc_sharded_bench.exe N` | Quark, N producers → N actors (contention-free ceiling) |
| `caf_mpsc_sharded_bench.exe N` | CAF, N producers → N actors (same shape) |

## Benchmarks

| Benchmark | Description |
|---|---|
| **Spawn** | Time to create 10,000 actors (ns/spawn) |
| **Tell** | Fire-and-forget message latency (p50/p99/p999 ns) |
| **Ask** | Request-response latency (p50/p99/p999 ns) |
| **Throughput** | Bulk fire-and-forget from a single producer (M msg/s) |
| **MPSC scaling** | Throughput with N producers → 1 actor, shared mailbox (M msg/s) |
| **Sharded MPSC scaling** | Throughput with N producers → N actors, one mailbox each — isolates enqueue/dispatch cost from mailbox contention |

## Machine

AMD Ryzen 5 4600H, 6 physical cores / 12 logical (SMT), Windows, clang++ 22.1.5. `hardware_concurrency()` = 12, so the `P=12` rows below run one OS thread per logical core — this repo's usual machine-safety cap (4 threads) was deliberately widened for this run to plot a full scaling curve; see `CLAUDE.md` for the standing rule.

**All numbers below are a single unpinned Windows session (2026-08-01)**, taken back-to-back, current master (`65cb64f`, ADR-037's TLS acquire/reclaim magazine live in `MessagePool`). Absolute numbers on this shared, unpinned host vary run-to-run by 10-30%; treat ratios and trends as the signal, not any single value. A `taskset`-pinned Linux/WSL2 re-run remains the natural follow-up for tighter absolute numbers (see `PERFORMANCE.md`/`decisions/ADR-037-message-pool-freelist-sync.md` for the pinned-Linux methodology used elsewhere in this repo).

## Single-threaded vs max-threaded (1 producer, 1 actor)

| Metric | Quark @1 | Quark @12 | CAF @1 | CAF @12 |
|---|---|---|---|---|
| Spawn (10k actors) | 842 ns | 678 ns | 1,537 ns | 4,594 ns |
| Tell p50 / p99 / p999 | 200 / 2,000 / 2,500 ns | 200 / 4,100 / 15,300 ns | 100 / 400 / 900 ns | 200 / 900 / 3,900 ns |
| Ask p50 / p99 / p999 | 700 / 1,500 / 3,300 ns | 1,400 / 3,400 / 8,100 ns | 2,600 / 20,000 / 47,000 ns | 17,100 / 28,900 / 70,400 ns |
| Throughput | **10.74 M/s** | 4.80 M/s | 4.88 M/s | 4.97 M/s |

**Quark wins single-threaded throughput outright** (10.74 vs 4.88 M/s, 2.2×) and spawn at both worker counts — this is the ADR-037 magazine effect: a single thread touching an uncontended `MessagePool` partition now pays almost no mutex tax. Quark's `ask` latency is also consistently lower than CAF's at both worker counts. The one place CAF still leads clearly is `tell` tail latency (p999) and, oddly, Quark's own throughput *drops* going from 1→12 workers — expected for this specific bench shape: it's still a single producer, so the extra 11 worker threads add idle-scan/park overhead with no more work to parallelize. That's a property of this bench's single-producer design, not of Quark's scaling in general — see the MPSC tables below for the real many-producer picture.

## MPSC scaling, shared mailbox (N producers → 1 actor)

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 3.97 | 4.52 | 1.14× |
| 2 | 5.37 | 7.96 | 1.48× |
| 4 | 9.03 | 12.40 | 1.37× |
| 8 | 11.81 | 17.81 | 1.51× |
| 12 | 13.08 | 19.72 | 1.51× |

Scaling P=1→P=12: Quark 3.97→13.08 M/s (**3.29×**), CAF 4.52→19.72 M/s (**4.36×**). CAF both starts and scales ahead here — this is the shared-mailbox shape, where every producer contends for the same actor's mailbox and the same `MessagePool` partition-lane traffic.

```
Quark            1 | ########                                      3.97
                 2 | ###########                                   5.37
                 4 | ##################                            9.03
                 8 | ########################                      11.81
                12 | ##########################                    13.08

CAF              1 | #########                                     4.52
                 2 | ################                              7.96
                 4 | #########################                     12.40
                 8 | ####################################          17.81
                12 | #######################################       19.72
```

## Sharded MPSC scaling (N producers → N actors, contention-free ceiling)

Same per-producer volume/pacing as above, but `producer[i]` only ever `tell()`s `actor[i]` — one shard/worker per producer on the Quark side, one scheduler thread per producer on the CAF side. No two producers ever contend for the same mailbox; this isolates enqueue/dispatch cost from mailbox/free-list contention.

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 3.62 | 3.41 | **0.94×** (Quark ahead) |
| 2 | 8.67 | 8.50 | **0.98×** (Quark ahead) |
| 4 | 12.65 | 12.47 | **0.99×** (parity) |
| 8 | 15.15 | 18.47 | 1.22× |
| 12 | 14.99 | 19.24 | 1.28× |

```
Quark            1 | #######                                       3.62
                 2 | #################                             8.67
                 4 | #########################                     12.65
                 8 | ##############################                15.15
                12 | ##############################                14.99

CAF              1 | #######                                       3.41
                 2 | #################                             8.50
                 4 | #########################                     12.47
                 8 | #####################################         18.47
                12 | ######################################        19.24
```

**Quark is at parity or ahead of CAF through P=4** once mailbox contention is removed — the shared-mailbox gap above is mostly a contention/scheduling cost, not a fundamental per-message enqueue disadvantage. The gap only opens at P=8/P=12, and it opens for a specific, identifiable reason: this bench spawns **N producer OS threads *and* N Quark worker threads** (`EngineConfig{workers=N, shards=N}`), so at P=12 that's 24 OS threads competing for 12 logical cores — genuine oversubscription. Quark's throughput actually *regresses* from P=8 (15.15 M/s) to P=12 (14.99 M/s), while CAF keeps climbing (18.47→19.24 M/s), suggesting CAF's scheduler tolerates this oversubscribed regime better than Quark's does today. Scaling P=1→P=12: Quark 3.62→14.99 M/s (**4.14×**), CAF 3.41→19.24 M/s (**5.64×**).

## Key findings

1. **Quark wins single-threaded and low-contention multi-producer throughput.** Post-ADR-037, the magazine removes essentially all of the uncontended mutex tax — single-thread tell throughput now beats CAF (10.74 vs 4.88 M/s), and sharded (contention-free) MPSC is at parity through 4 producers.
2. **The shared-mailbox gap (1.14×-1.51×) is real and is a contention cost, not a raw-speed deficit.** Sharding away mailbox contention closes most of it at P≤4; the remaining gap is CAF's more mature contended-mailbox/scheduler path.
3. **Oversubscription at P=12 hurts Quark specifically.** The sharded bench's one-worker-per-producer design means P=12 spawns 24 OS threads on a 12-thread machine; Quark's throughput plateaus/regresses there while CAF's keeps climbing. Worth a follow-up: does Quark's scheduler have a specific oversubscription cost (e.g. more/costlier park-wake cycles under contention for the CPU itself, not just the mailbox) that CAF's doesn't?
4. **Quark's single-producer `throughput` bench actually gets slower with more workers** (10.74 M/s @1 → 4.80 M/s @12) — expected for a fixed single-producer workload spread across more idle-scanning workers, not a regression signal.

## History

Full narrative of each investigation round (the `MessagePool` partitioning fix, ADR-035's park/wake backoff, ADR-036's activation linger, the `ReclaimSink` bug that made every pre-2026-08-01 Quark number in this file understate real throughput, and ADR-037's TLS magazine) is preserved in git history for this file and in the relevant ADRs:

- `decisions/ADR-035-worker-park-wake-backoff-policy.md`
- `decisions/ADR-036-activation-linger-idle-churn-reduction.md`
- `decisions/ADR-037-message-pool-freelist-sync.md`

Prior versions of this README (`git log -- bench/caf_comparison/README.md`) have the session-by-session before/after tables for each of those fixes if you need the historical progression rather than just the current state above.
