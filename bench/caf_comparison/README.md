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

> **Re-run (2026-08-01, same day, branch `adr-038-drain-owner-eviction`) — after ADR-038's patch.**
> Every table below was re-measured after `include/quark/core/engine.hpp`/`engine_config.hpp` gained
> ADR-038's Bounded Cooperative Drain-Owner Eviction mechanism. The mechanism ships **default-off**
> (`drain_owner_steal_probe_limit=0`), and `quark_stress_bench.exe` now prints that config line on
> every run (`ADR-038 cooperative eviction: probe_limit=0 ack_spin_limit=128 (0 = shipped default,
> byte-identical to pre-ADR-038 behavior)`) so this is directly confirmable, not just claimed. **This
> is not a controlled A/B against the exact pre-patch binary on this exact session** — it's simply
> the next session's numbers, on a host that turned out noticeably noisier this time (see below) — so
> don't read any single delta against the table further down as an ADR-038 effect; the mechanism was
> disabled throughout. Every table's absolute numbers moved from the original session, **some
> substantially** — most visibly, this session's stress test shows Quark's tail latency already
> spiking hard at 8 pairs (1.33× oversubscription), not just 12 (2×), and **CAF's own tail latency
> also grew across every pair count** (its P=12 max latency alone: 124.7 µs → 883.7 µs, a ~7×
> increase, in code this patch never touched). Since an unrelated engine's numbers moved by a similar
> order of magnitude, the most honest read is host-session noise (background load, thermal state,
> whatever else was running), not a Quark-code regression — but it's reported in full, not smoothed
> over, per this file's own standing methodology.

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
| Spawn (10k actors) | 780 ns | 760 ns | 935 ns | 4,179 ns |
| Tell p50 / p99 / p999 | 100 / 700 / 1,100 ns | 100 / 700 / 1,100 ns | 100 / 400 / 800 ns | 100 / 400 / 800 ns |
| Ask p50 / p99 / p999 | 1,300 / 2,700 / 4,000 ns | 1,400 / 3,400 / 9,000 ns | 2,000 / 20,300 / 42,100 ns | 15,600 / 21,700 / 30,900 ns |
| Throughput | **7.73 M/s** | 7.02 M/s | 5.64 M/s | 5.51 M/s |

**Quark still wins single-threaded throughput, by a consistent margin** (7.73 vs 5.64 M/s,
**1.37×** — matches the prior session's 1.39× closely). Quark wins spawn clearly at 12 workers
(760 ns vs CAF's 4,179 ns, **5.5×** — CAF's spawn cost keeps ballooning with scheduler thread
count, Quark's doesn't) and wins `ask` p50 decisively at 12 workers (1,400 vs 15,600 ns, **~11×**
— CAF's ask latency degrades badly under its own multi-threaded scheduler; at 1 worker the gap is
smaller but still Quark's favor, 1,300 vs 2,000 ns). `tell` p50 is tied at 100 ns for both engines
at both worker counts; CAF leads `tell` tail latency (p999) at both worker counts (800 ns flat vs
Quark's 1,100 ns). `ask` tail latency swung noisily session-to-session for both engines (a known
noisy metric — occasional scheduling stalls dominate the p99/p999 tail on a shared host) but the
qualitative story held: Quark's `ask` p999 stays in the single-digit-µs range at both worker
counts while CAF's climbs into the tens of µs. Quark's throughput drops only slightly from 1→12
workers (7.73→7.02 M/s, ~9%) — still expected for this single-producer bench shape (extra
idle-scanning workers with no more work to parallelize).

## MPSC scaling, shared mailbox (N producers → 1 actor)

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 4.39 | 5.76 | 1.31× |
| 2 | 7.52 | 9.08 | 1.21× |
| 4 | 9.73 | 14.24 | 1.46× |
| 8 | 12.04 | 18.55 | 1.54× |
| 12 | 12.16 | 21.30 | 1.75× |

Scaling P=1→P=12: Quark 4.39→12.16 M/s (**2.77×**), CAF 5.76→21.30 M/s (**3.70×**). CAF leads at every producer count this session (no P=2 near-tie this time) and the gap widens steadily with P rather than plateauing — this is the shared-mailbox shape, where every producer contends for the same actor's mailbox and the same `MessagePool` partition-lane traffic, and CAF's scheduler scales that contention better.

```
Quark            1 | #########                                     4.39
                 2 | ################                              7.52
                 4 | ####################                          9.73
                 8 | #########################                    12.04
                12 | #########################                    12.16

CAF              1 | ############                                  5.76
                 2 | ###################                           9.08
                 4 | #############################                14.24
                 8 | ######################################       18.55
                12 | ############################################ 21.30
```

## Sharded MPSC scaling (N producers → N actors, contention-free ceiling)

Same per-producer volume/pacing as above, but `producer[i]` only ever `tell()`s `actor[i]` — one shard/worker per producer on the Quark side, one scheduler thread per producer on the CAF side. No two producers ever contend for the same mailbox; this isolates enqueue/dispatch cost from mailbox/free-list contention.

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 3.80 | 6.08 | 1.60× |
| 2 | 9.85 | 11.84 | 1.20× |
| 4 | 13.18 | 17.34 | 1.32× |
| 8 | 17.75 | 20.66 | 1.16× |
| 12 | 16.93 | 22.67 | 1.34× |

```
Quark            1 | #######                                       3.80
                 2 | ###################                           9.85
                 4 | #########################                    13.18
                 8 | ##################################           17.75
                12 | #################################            16.93

CAF              1 | ###########                                   6.08
                 2 | #######################                      11.84
                 4 | #################################            17.34
                 8 | ########################################     20.66
                12 | ############################################ 22.67
```

**CAF leads at every sharded producer count, including P=1**, consistent with the prior session
(no P=2 near-tie in either run since the warmup fix landed). CAF's advantage this session ranges
1.16×-1.60× — not uniform, and P=8 is the closest gap (1.16×) rather than P=2 this time, another
sign of run-to-run variance in exactly where the narrowest point sits. The oversubscription
regression is still real and still Quark-specific: this bench spawns **N producer OS threads *and*
N Quark worker threads** (`EngineConfig{workers=N, shards=N}`), so P=12 means 24 OS threads on 12
logical cores. Quark's throughput *regresses* from P=8 (17.75 M/s) to P=12 (16.93 M/s, -4.6%) while
CAF keeps climbing (20.66→22.67 M/s) — CAF's scheduler tolerates this oversubscribed regime better
than Quark's does today, the same qualitative finding as the prior session, at a smaller magnitude
this time (-4.6% vs the prior session's -10.4%). Scaling P=1→P=12: Quark 3.80→16.93 M/s
(**4.46×**), CAF 6.08→22.67 M/s (**3.73×**) — Quark's overall fold-scaling is again higher despite
trailing in absolute terms throughout, entirely because its P=1 starting point is lower, not
because it closes the gap along the way (same shape as the prior session, 3.66× vs 3.42×).

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

### Results (ramped 1 → 12 pairs, 3s sustained + 1s warmup each, re-run 2026-08-01 after ADR-038's patch — mechanism confirmed default-off on every run)

| Pairs | Threads | Quark M/s | Quark per-lane | Quark p999 | Quark max | CAF M/s | CAF per-lane | CAF p999 | CAF max |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 4.9 | 4.9 | 19.6 µs | 47.7 µs | 5.9 | 5.9 | 0.6 µs | 44.7 µs |
| 2 | 4 | 10.1 | 4.84-5.26 | 8.2 µs | 24.2 µs | 10.8 | 5.28-5.51 | 0.7 µs | 19.1 µs |
| 4 | 8 | 17.8 | 3.76-5.14 | 20.8 µs | 118.7 µs | 17.1 | 4.25-4.29 | 3.1 µs | 43.2 µs |
| 6 | 12 (= cores) | 19.3 | 2.99-3.65 | 32.3 µs | 515.9 µs | 21.6 | 3.43-3.80 | 9.7 µs | 136.1 µs |
| 8 | 16 (1.33× oversub.) | 19.4 | 2.24-2.62 | **263.5 µs** | 27,125.1 µs | 23.4 | 2.54-3.26 | 12.6 µs | 288.0 µs |
| 12 | 24 (2× oversub.) | 21.1 | 1.30-2.49 | 148.6 µs | 25,080.1 µs | 25.1 | 1.82-2.33 | 53.5 µs | 883.7 µs |

All twelve runs (six pair counts × two engines) **PASS** — zero lost or duplicated messages, no
crash, no hang, even at 2× oversubscription. Every Quark run in this table confirmed
`probe_limit=0` (ADR-038's cooperative eviction shipped disabled) via the bench's own printed
config line — these are default-behavior numbers, not a demonstration of that mechanism.

**This session's numbers moved substantially from the prior session's table (preserved above the
callout in the Machine section's history) — both engines, not just Quark** — see the re-run
callout near the top of this file before drawing conclusions from any single delta here.

1. **CAF leads aggregate throughput at every pair count**, same as the prior session (5.9 vs 4.9
   M/s @1, up to 25.1 vs 21.1 M/s @12) — consistent with the shared-mailbox MPSC table above.
2. **Quark's p999 does *not* climb monotonically with pair count this session — it spikes hardest
   at 8 pairs (1.33× oversubscription), not at 12 (2×).** 19.6 → 8.2 → 20.8 → 32.3 → **263.5** →
   148.6 µs across 1/2/4/6/8/12 pairs. Max latency shows the same pattern: both oversubscribed
   points (8 and 12 pairs) land in the same ~25-27 ms range (27.1 ms and 25.1 ms respectively) —
   effectively a step function once oversubscription starts, not a curve that keeps climbing with
   the oversubscription ratio. This is a genuinely different shape from the prior session's
   near-monotonic climb (3.9→5.9→16.6→15.6→26.4→106.7 µs, worst exactly at 12 pairs) — both
   sessions agree that oversubscription is where Quark's tail blows up, but not on the precise
   pair count where it's worst. Treat "oversubscription causes a tail-latency cliff" as the robust
   finding and "exactly which oversubscribed pair count is worst" as still noisy across sessions.
3. **CAF's p999 was monotonic this session** (0.6 → 0.7 → 3.1 → 9.7 → 12.6 → 53.5 µs) — no repeat
   of the prior session's "worst exactly at 6 pairs, recovers at 8" wobble. **CAF's absolute
   numbers also grew across the board versus the prior session** (e.g. max at 12 pairs: 124.7 µs →
   883.7 µs, ~7×) in code this patch never touched — the single strongest piece of evidence in this
   file that session-to-session host noise, not a Quark-specific code change, explains most of the
   movement between the two sessions' tables.
4. **Quark's tail latency is worse than CAF's at every pair count from 2 onward, and the gap
   widens sharply once oversubscription starts — this part is robust across both sessions.** At 12
   pairs this session: Quark's p999 (148.6 µs) is **~2.8×** CAF's (53.5 µs), and Quark's worst
   single message (25.1 ms) is **~28×** CAF's (883.7 µs). At 8 pairs the gap is even starker:
   Quark's p999 (263.5 µs) is **~21×** CAF's (12.6 µs). The exact multiples moved between sessions
   (prior session: ~4× and ~163× at 12 pairs) but the direction and the "oversubscription is where
   it gets bad" story did not — this cross-session-stable qualitative finding is what motivated
   [ADR-038](../../decisions/ADR-038-scheduler-oversubscription-tail-latency.md)'s investigation,
   and what its Round 2 re-confirmed by driving the fix candidate through this exact bench's real
   `worker_loop` (see Key Finding #4 below).

## Key findings

1. **Warmup discipline changes the competitive picture more than any single optimization in this file.** Every number above was re-measured after switching every bench's warmup from a fixed op count to ~1s of wall-clock time (see the callout in the single-threaded section). The headline single-threaded throughput gap shrank from a since-retracted 2.2× to a real, stable **~1.37-1.39×** across both sessions, and sharded MPSC flipped from "Quark ahead at P=1/P=2, parity at P=4" to **CAF ahead at every producer count including P=1**, unchanged in this session's re-run. Neither engine's underlying performance changed at the warmup fix — only the measurement did. Treat this as a standing caution: an under-warmed bench doesn't just add noise, it can flip which engine looks like it's winning.
2. **Quark still wins clearly on `ask` latency and spawn at high worker counts.** Quark's spawn cost stays flat with worker count (760 ns @12) while CAF's balloons (4,179 ns @12, **5.5×** worse) — the same story both sessions. Quark's `ask` p50 leads CAF's at both worker counts this session too (1,300 vs 2,000 ns @1; 1,400 vs 15,600 ns @12), though the exact tail-latency numbers for `ask` are noisy session-to-session for both engines (a known-noisy metric on a shared host — see the single-threaded section).
3. **The shared-mailbox gap widened this session** (1.21×-1.75×, vs the prior session's 0.99×-1.58×) — CAF now leads at every producer count including P=2, where the prior session showed a near-tie. Sharding away mailbox contention narrows but does not close the gap at any producer count in either session.
4. **Oversubscription hurts Quark specifically**, and this is the one finding that has now been directly investigated, not just observed. The sharded bench's one-worker-per-producer design means P=12 spawns 24 OS threads on a 12-thread machine; Quark's throughput regresses P=8→P=12 in both sessions (this session: 17.75→16.93 M/s, -4.6%) while CAF's keeps climbing. The sustained stress test above quantifies the mechanism as a latency-tail cost, not a correctness or throughput-collapse one: aggregate throughput and message integrity hold in every run, but Quark's tail latency blows up once oversubscription starts (this session: p999 up to 263.5 µs and max up to 27.1 ms at 8-12 pairs, vs the prior session's 106.7 µs / 20.3 ms peak at 12 pairs — the two sessions disagree on exactly which oversubscribed pair count is worst but agree oversubscription is where it happens). **Root-cause confirmed, and a candidate fix built, proven, and re-tested — twice.** [ADR-038](../../decisions/ADR-038-scheduler-oversubscription-tail-latency.md) traced this to `try_drain_shard`'s `drain_owner` CAS stranding other workers behind a preempted shard owner. A `design-debate-prove` round built and proved three competing fixes; a naive `yield()`-based backoff was *proven counterproductive* (worse max latency in 9/10 trials), while **bounded cooperative drain-owner eviction** won and is implemented (`EngineConfig::drain_owner_steal_probe_limit`, present but disabled in every number in this file). Round 2 then re-ran the mechanism's own tie-breaking experiment against this exact bench's real `worker_loop` (not the original proving harness) and **refuted** the hope that its disclosed p999 side-effect was a proving artifact — enabled, it regressed p999 further (median +130% at P=12) rather than fixing it. The mechanism stays **default-off**; see `002-Scheduler.md`'s "Bounded cooperative drain-owner eviction (ADR-038)" section and the ADR's Round 2 for the full evidence and the concrete next step (a cheaper eviction-probe heuristic).
5. **Quark's single-producer `throughput` bench gets only slightly slower with more workers** (7.73 M/s @1 → 7.02 M/s @12 this session, ~9% — consistent with the prior session's ~10%) — still expected for a fixed single-producer workload spread across more idle-scanning workers, not a regression signal.
6. **Quark has no mailbox backpressure today.** `MessagePool` cold-allocates unboundedly when a partition is exhausted rather than throttling producers (RFC 022 documents this as an open gap, not yet implemented) — confirmed by the stress-test incident described below: hammering a single shared mailbox harder than its one consumer can drain grew RSS to ~18GB in seconds. Not a risk under the paired/sharded topologies real workloads use, but a real gap if a workload ever does route sustained heavy traffic at a single actor identity.
7. **This session's numbers are noticeably noisier than the prior session's, on both engines** — see the re-run callout in the Machine section. CAF's own tail latency grew substantially (e.g. P=12 max: 124.7 µs → 883.7 µs) in code this session's patch never touched, which is the strongest evidence that host-session noise, not a Quark-specific regression, explains most of the movement in this file's absolute numbers between the two 2026-08-01 sessions. A `taskset`-pinned Linux CI re-run remains the natural follow-up for numbers that don't need this caveat.

## History

Full narrative of each investigation round (the `MessagePool` partitioning fix, ADR-035's park/wake backoff, ADR-036's activation linger, the `ReclaimSink` bug that made every pre-2026-08-01 Quark number in this file understate real throughput, ADR-037's TLS magazine, and ADR-038's scheduler-oversubscription investigation) is preserved in git history for this file and in the relevant ADRs:

- `decisions/ADR-035-worker-park-wake-backoff-policy.md`
- `decisions/ADR-036-activation-linger-idle-churn-reduction.md`
- `decisions/ADR-037-message-pool-freelist-sync.md`
- `decisions/ADR-038-scheduler-oversubscription-tail-latency.md` — the sustained stress test's
  oversubscription tail-latency finding (Key Finding #4), a `design-debate-prove` round that built
  and proved a candidate fix (Bounded Cooperative Drain-Owner Eviction), and a Round 2 that
  re-tested it against this exact bench's real `worker_loop` — refuted, stays default-off.

Prior versions of this README (`git log -- bench/caf_comparison/README.md`) have the session-by-session before/after tables for each of those fixes if you need the historical progression rather than just the current state above.
