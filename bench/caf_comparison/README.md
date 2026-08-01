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
| `quark_stress_bench.exe [pairs] [duration_s] [warmup_s]` | Quark sustained-duration survivability test, N producer↔actor lanes (see below) |
| `caf_stress_bench.exe [pairs] [duration_s] [warmup_s]` | CAF equivalent, same topology/safety design, direct comparison |

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

**All numbers below are a single unpinned Windows session (2026-08-01)**, taken back-to-back, current master (`65cb64f`, ADR-037's TLS acquire/reclaim magazine live in `MessagePool`), **after switching every bench's warmup from a fixed op count to a fixed ~1s of wall-clock time** (see the callout below). Absolute numbers on this shared, unpinned host still vary run-to-run; treat ratios and trends as the signal, not any single value. A `taskset`-pinned Linux/WSL2 re-run remains the natural follow-up for tighter absolute numbers (see `PERFORMANCE.md`/`decisions/ADR-037-message-pool-freelist-sync.md` for the pinned-Linux methodology used elsewhere in this repo).

> **Why every warmup phase in this suite is now wall-clock-bounded, not a fixed op count:**
> earlier same-day numbers showed `quark_bench.exe 1`'s single-threaded throughput swinging from
> 6.33 to 13.27 M/s across five identical back-to-back runs (>2× spread) despite already having a
> 1,000,000-op warmup — while `caf_bench.exe 1` stayed within 5.30-5.49 M/s (~3.5%) on the same
> host, same session. A fixed op count finishes in single-digit milliseconds at Quark's throughput —
> nowhere near enough for this laptop's CPU turbo-boost/frequency scaling to reach steady state
> before the timed section starts, and evidently enough to leave Quark specifically exposed to
> whatever clock state the OS handed it. Switching every warmup phase in every bench (this file's
> four metrics, both MPSC benches, both sharded MPSC benches — the stress-test pair already used a
> time-based warmup and needed no change) to ~1 full second of sustained wall-clock warmup before
> timing starts brought the same five-run check down to **7.09-7.55 M/s (~6% spread)** — close to an
> order of magnitude tighter. **Every table below reflects the fixed, wall-clock-warmed-up
> binaries** — treat any older copy of this README (or the historical `10.74 M/s` figure) as
> measuring a since-fixed methodology bug, not a stable property of Quark.

## Single-threaded vs max-threaded (1 producer, 1 actor)

| Metric | Quark @1 | Quark @12 | CAF @1 | CAF @12 |
|---|---|---|---|---|
| Spawn (10k actors) | 774 ns | 780 ns | 975 ns | 4,578 ns |
| Tell p50 / p99 / p999 | 100 / 700 / 1,100 ns | 100 / 700 / 1,100 ns | 100 / 300 / 500 ns | 100 / 400 / 900 ns |
| Ask p50 / p99 / p999 | 600 / 28,700 / 47,100 ns | 1,300 / 3,600 / 33,600 ns | 1,900 / 30,900 / 54,100 ns | 20,800 / 38,000 / 54,300 ns |
| Throughput | **7.69 M/s** | 6.90 M/s | 5.55 M/s | 5.34 M/s |

**Quark still wins single-threaded throughput, but by a much smaller and now-trustworthy margin**
(7.69 vs 5.55 M/s, **1.39×** — not the old, methodology-inflated 2.2×). Quark also wins spawn
clearly at 12 workers (780 ns vs CAF's 4,578 ns — CAF's spawn cost balloons with scheduler thread
count, Quark's doesn't) and wins `ask` p50 decisively at both worker counts (600 vs 1,900 ns @1;
1,300 vs 20,800 ns @12 — CAF's ask latency degrades badly under its own multi-threaded scheduler).
`tell` p50 is now tied at 100 ns for both engines; CAF still leads `tell` tail latency (p999) at
both worker counts, and `ask` p999 is close either way. Quark's throughput now *drops only
slightly* from 1→12 workers (7.69→6.90 M/s) rather than the sharp 1→12 drop seen pre-warmup-fix —
still expected for this single-producer bench shape (extra idle-scanning workers with no more work
to parallelize), just a smaller effect once the measurement itself is stable.

## MPSC scaling, shared mailbox (N producers → 1 actor)

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 5.04 | 7.05 | 1.40× |
| 2 | 8.93 | 8.88 | **0.99×** (Quark ahead) |
| 4 | 9.44 | 14.24 | 1.51× |
| 8 | 12.25 | 19.30 | 1.58× |
| 12 | 13.86 | 21.46 | 1.55× |

Scaling P=1→P=12: Quark 5.04→13.86 M/s (**2.75×**), CAF 7.05→21.46 M/s (**3.04×**). CAF starts and finishes ahead here (P=2 is a near-tie) — this is the shared-mailbox shape, where every producer contends for the same actor's mailbox and the same `MessagePool` partition-lane traffic.

```
Quark            1 | ##########                                    5.04
                 2 | ##################                            8.93
                 4 | ###################                           9.44
                 8 | #########################                     12.25
                12 | ############################                  13.86

CAF              1 | ##############                                7.05
                 2 | ##################                            8.88
                 4 | #############################                 14.24
                 8 | ########################################      19.30
                12 | ############################################  21.46
```

## Sharded MPSC scaling (N producers → N actors, contention-free ceiling)

Same per-producer volume/pacing as above, but `producer[i]` only ever `tell()`s `actor[i]` — one shard/worker per producer on the Quark side, one scheduler thread per producer on the CAF side. No two producers ever contend for the same mailbox; this isolates enqueue/dispatch cost from mailbox/free-list contention.

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 4.49 | 6.39 | 1.42× |
| 2 | 10.51 | 10.96 | 1.04× |
| 4 | 14.04 | 16.45 | 1.17× |
| 8 | 18.32 | 20.59 | 1.12× |
| 12 | 16.42 | 21.83 | 1.33× |

```
Quark            1 | #########                                     4.49
                 2 | #####################                         10.51
                 4 | ############################                  14.04
                 8 | #####################################         18.32
                12 | #################################             16.42

CAF              1 | #############                                 6.39
                 2 | ######################                        10.96
                 4 | #################################             16.45
                 8 | ##########################################    20.59
                12 | ############################################  21.83
```

**CAF now leads at every sharded producer count, including P=1** — a real change from before the
warmup fix, where Quark led P=1/P=2 and was at parity at P=4. With both engines properly warmed up,
CAF's advantage is consistent (1.04×-1.42×) rather than opening up only at high P, though it's not
uniform either — P=2 is the closest gap (1.04×). The oversubscription regression is still real and
still Quark-specific: this bench spawns **N producer OS threads *and* N Quark worker threads**
(`EngineConfig{workers=N, shards=N}`), so P=12 means 24 OS threads on 12 logical cores. Quark's
throughput *regresses* from P=8 (18.32 M/s) to P=12 (16.42 M/s) while CAF keeps climbing (20.59→
21.83 M/s) — CAF's scheduler tolerates this oversubscribed regime better than Quark's does today.
Scaling P=1→P=12: Quark 4.49→16.42 M/s (**3.66×**), CAF 6.39→21.83 M/s (**3.42×**) — Quark's overall
fold-scaling is actually slightly higher despite trailing in absolute terms throughout, entirely
because its P=1 starting point is lower, not because it closes the gap along the way.

## Sustained stress test (whole-engine survivability)

All benchmarks above run a fixed op count and finish in 1-2s — the CPU never stays pinned long
enough to reveal drift, latency-tail blowup, or a backlog that only shows up under sustained
pressure. `quark_stress_bench.exe` instead runs **paired producer↔actor lanes** (lane `i`'s
producer only ever `tell()`s actor `i`, one dedicated shard/worker per lane — the same shape as
the sharded MPSC bench above, not the shared-mailbox one) continuously for a fixed wall-clock
duration, and verifies `sent == received` at the end as the actual survivability verdict.

**Why paired lanes, not a shared mailbox:** an earlier version of this bench hammered a single
actor's mailbox with N producer threads, sustained. That's an artificial worst case no real
production workload creates (nothing routes every message to one actor identity), and it made
this session's dev box unresponsive at 8 producers — the single consumer fell behind, `MessagePool`
partitions exhausted, and the pool's documented cold-path behavior (`message_pool.hpp:136`: "grows
cold if the partition is genuinely exhausted") cold-allocated a fresh heap `Cell` per message with
no backpressure, ballooning to ~18GB RSS before being force-killed. That's a real, currently-true
property of `MessagePool` (RFC 022 "Resource Governance and Overload Control" documents the gap;
no circuit-breaker/backpressure is implemented yet) — but it's specific to the single-hot-mailbox
shape, not representative of a normally-routed workload. The paired topology below is what an
actual engine sees under sustained load.

**Safety design** (after the incident above): the producer-stop decision never blocks on
`ask()`/`request()` — pure wall-clock checks only. A hard global send cap and a process watchdog
(`std::quick_exit` past an absolute ceiling) back it up as independent, redundant circuit breakers.
`caf_stress_bench.exe` mirrors `quark_stress_bench.exe` exactly — same topology, same safety
design, same report format — for a direct comparison. See either file's header for the full
rationale. (One tuning bug surfaced and was fixed while gathering these numbers: the post-run
drain-wait poll originally used a flat 15s budget for a sequential pass over all lanes, which
under-times at higher pair counts — a real 12-pair CAF run completed cleanly at 25 M/s but the
correctness check couldn't finish confirming all 12 lanes within 15s and reported INCONCLUSIVE for
a run that was actually fine. Both files now scale the drain budget with pair count.)

*(These stress-test numbers predate and are unaffected by the warmup-methodology fix above —
`quark_stress_bench.exe`/`caf_stress_bench.exe` already used a proper ~1s wall-clock warmup from
the start, which is in fact what motivated checking whether the other benches' warmup was adequate
in the first place.)*

### Results (ramped 1 → 12 pairs, 3s sustained + 1s warmup each, same session as above)

| Pairs | Threads | Quark M/s | Quark per-lane | Quark p999 | Quark max | CAF M/s | CAF per-lane | CAF p999 | CAF max |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 5.0 | 5.0 | 3.9 µs | 18.2 µs | 5.7 | 5.7 | 4.9 µs | 26.3 µs |
| 2 | 4 | 9.7 | 4.80-4.93 | 5.9 µs | 12.8 µs | 10.7 | 5.11-5.54 | 2.5 µs | 19.1 µs |
| 4 | 8 | 17.1 | 3.79-5.10 | 16.6 µs | 39.3 µs | 17.6 | 4.24-4.54 | 1.1 µs | 105.6 µs |
| 6 | 12 (= cores) | 19.6 | 2.68-3.83 | 15.6 µs | 196.0 µs | 21.1 | 3.27-3.72 | 16.0 µs | 128.3 µs |
| 8 | 16 (1.33× oversub.) | 20.8 | 2.32-3.14 | 26.4 µs | 339.2 µs | 22.8 | 2.54-3.15 | 11.5 µs | 77.6 µs |
| 12 | 24 (2× oversub.) | 21.6 | 1.11-2.57 | 106.7 µs | 20,260.9 µs | 23.9 | 1.75-2.18 | 25.6 µs | 124.7 µs |

All twelve runs (six pair counts × two engines) **PASS** — zero lost or duplicated messages, no
crash, no hang, even at 2× oversubscription.

1. **CAF leads aggregate throughput at every pair count** (5.7 vs 5.0 M/s @1, up to 23.9 vs 21.6
   M/s @12) — consistent with the shared-mailbox MPSC table above where CAF also starts and stays
   ahead, so this isn't new, just confirmed under sustained load.
2. **Quark's p999 climbs almost monotonically with pair count and blows up specifically at 2×
   oversubscription.** 3.9 → 5.9 → 16.6 → 15.6 → 26.4 → 106.7 µs across 1/2/4/6/8/12 pairs — a
   small 4→6 reversal that's within this shared host's usual 10-30% run-to-run noise, then a real
   signal: p999 roughly **4×** and max latency **60×** from 8→12 pairs (26.4→106.7 µs, 339 µs→20.3
   ms) as 24 threads compete for 12 cores. This is the oversubscription cost Key Finding #4 flags.
3. **CAF's p999 is *not* monotonic — it has a real minimum at 4 pairs, then spikes hardest exactly
   at 6 pairs, not at the higher pair counts.** 4.9 → 2.5 → **1.1 (best)** → 16.0 (worst so far) →
   11.5 (partial recovery) → 25.6 µs. The spike lands precisely where CAF's OS thread count (6
   producers + 6 scheduler threads = 12) first *equals* the logical core count — worse than both
   4 pairs (8 threads, room to spare) and 8 pairs (16 threads, already oversubscribed but somehow
   recovering some of the 6-pair cost). That specific shape (worst right at the saturation
   boundary, better on both sides of it) is unusual enough that it warrants repeated runs before
   trusting it as a real CAF scheduler characteristic rather than host noise — every number in this
   file is a single unpinned-Windows-host run (see the Machine section above).
4. **Regardless of that CAF wobble, Quark's tail latency is worse than CAF's at every pair count
   from 2 onward, and the gap widens sharply as pairs increase.** At 12 pairs: Quark's p999
   (106.7 µs) is **~4×** CAF's (25.6 µs), and Quark's worst single message (20.3 ms) is **~163×**
   CAF's (124.7 µs) — CAF's single worst message across the whole run is still cheaper than
   Quark's *typical* top-0.1% message. This cross-engine gap is the robust signal; the exact shape
   of CAF's own curve across pair counts (point 3) is what needs re-running to trust further than
   "consistently better than Quark's here."

## Key findings

1. **Warmup discipline changes the competitive picture more than any single optimization in this file.** Every number above was re-measured after switching every bench's warmup from a fixed op count to ~1s of wall-clock time (see the callout in the single-threaded section). The headline single-threaded throughput gap shrank from a since-retracted 2.2× to a real, stable **1.39×** (7.69 vs 5.55 M/s), and sharded MPSC flipped from "Quark ahead at P=1/P=2, parity at P=4" to **CAF ahead at every producer count including P=1**. Neither engine's underlying performance changed — only the measurement did. Treat this as a standing caution: an under-warmed bench doesn't just add noise, it can flip which engine looks like it's winning.
2. **Quark still wins clearly on `ask` latency and spawn at high worker counts.** Quark's `ask` p50 is 600 ns @1 / 1,300 ns @12 vs CAF's 1,900 ns @1 / 20,800 ns @12 — CAF's ask path degrades sharply under its own scheduler at 12 threads, Quark's doesn't. Spawn is similar: 780 ns @12 for Quark vs 4,578 ns for CAF.
3. **The shared-mailbox gap (0.99×-1.58×) is a real, if smaller-than-previously-measured, contention cost.** CAF leads at every producer count except P=2 (a near-tie); sharding away mailbox contention narrows but no longer closes the gap at low P the way the pre-fix numbers suggested.
4. **Oversubscription at P=12 hurts Quark specifically**, and this survives the warmup fix. The sharded bench's one-worker-per-producer design means P=12 spawns 24 OS threads on a 12-thread machine; Quark's throughput regresses P=8→P=12 (18.32→16.42 M/s) while CAF's keeps climbing (20.59→21.83 M/s). The sustained stress test above quantifies the mechanism: it's a latency-tail cost, not a correctness or throughput-collapse one — Quark's p999 climbs almost monotonically with pair count and roughly quadruples again specifically from 8→12 pairs (26.4→106.7 µs, max 339 µs→20.3 ms), while aggregate throughput and message integrity both hold. CAF's p999 is lower than Quark's at every pair count from 2 upward, though CAF's own curve isn't monotonic either (it dips to a minimum at 4 pairs then spikes at 6 before partially recovering at 8) — see the stress-test section for the full six-pair-count table and the caveat that these are single-run samples on a shared, unpinned host. **Root-cause confirmed and partially fixed by [ADR-038](../../decisions/ADR-038-scheduler-oversubscription-tail-latency.md):** `try_drain_shard`'s `drain_owner` CAS can strand other workers behind a preempted shard owner under oversubscription. A `design-debate-prove` round built and proved three competing fixes; a naive `yield()`-based backoff was *proven counterproductive* (worse max latency in 9/10 trials), while **bounded cooperative drain-owner eviction** won and is implemented (`EngineConfig::drain_owner_steal_probe_limit`) but ships **default-off** pending re-measurement of a disclosed p999 side-effect on a quiet host — see `002-Scheduler.md`'s "Bounded cooperative drain-owner eviction (ADR-038)" section and the ADR for the full evidence.
5. **Quark's single-producer `throughput` bench now gets only slightly slower with more workers** (7.69 M/s @1 → 6.90 M/s @12, versus a much sharper pre-fix drop) — still expected for a fixed single-producer workload spread across more idle-scanning workers, not a regression signal; the warmup fix mostly removed noise, not the underlying effect.
6. **Quark has no mailbox backpressure today.** `MessagePool` cold-allocates unboundedly when a partition is exhausted rather than throttling producers (RFC 022 documents this as an open gap, not yet implemented) — confirmed by the stress-test incident above: hammering a single shared mailbox harder than its one consumer can drain grew RSS to ~18GB in seconds. Not a risk under the paired/sharded topologies real workloads use, but a real gap if a workload ever does route sustained heavy traffic at a single actor identity.

## History

Full narrative of each investigation round (the `MessagePool` partitioning fix, ADR-035's park/wake backoff, ADR-036's activation linger, the `ReclaimSink` bug that made every pre-2026-08-01 Quark number in this file understate real throughput, and ADR-037's TLS magazine) is preserved in git history for this file and in the relevant ADRs:

- `decisions/ADR-035-worker-park-wake-backoff-policy.md`
- `decisions/ADR-036-activation-linger-idle-churn-reduction.md`
- `decisions/ADR-037-message-pool-freelist-sync.md`

Prior versions of this README (`git log -- bench/caf_comparison/README.md`) have the session-by-session before/after tables for each of those fixes if you need the historical progression rather than just the current state above.
