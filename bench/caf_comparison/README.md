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

> **Re-run #2 (2026-08-01, same day) — bench-fidelity fix. This one changes the headline
> conclusion, not just the noise.** A user comparing this file's tables noticed that Quark's
> "1 producer" row in the MPSC-scaling and sharded-MPSC-scaling tables didn't match the
> "Single-threaded" table's throughput number, even though both describe the identical shape (one
> producer thread telling one actor as fast as possible). Investigating found two real, unrelated
> measurement bugs, not a difference in engine behavior:
> 1. **`quark_mpsc_bench.cpp`/`quark_mpsc_sharded_bench.cpp`/`caf_mpsc_bench.cpp`/
>    `caf_mpsc_sharded_bench.cpp` all called `now()` twice around *every* message**, unconditionally,
>    purely to feed a latency sample that was only ever actually recorded for 1-in-16 messages — the
>    other 15 clock reads were pure waste baked into the timed region. Fixed: the timestamp calls now
>    only run on sampled iterations. This alone took Quark's measured P=1 throughput from ~4.4 M/s to
>    ~7-7.8 M/s (matching `quark_bench.exe 1`'s un-instrumented number) in a spot-check.
> 2. **Both engines' `PingActor`/`ping_actor` in the shared-mailbox MPSC benches did a per-message
>    atomic increment into a counter that was never read anywhere** — pure, symmetric-but-unnecessary
>    tax. Removed (both engines), matching `quark_bench.cpp`'s/CAF's already-empty handler in every
>    *other* bench in this suite.
> 3. **The one that matters most: `caf_bench.cpp`'s `bench_throughput()` had a hardcoded
>    `sleep_for(500ms)` INSIDE the timed window** — captured before `t1`, not after — deflating
>    *every* CAF single-threaded throughput number in every session of this file to date by a fixed
>    ~500ms/~1.3-1.8s, roughly 25-30%. Fixed by moving `t1`'s capture to immediately after the send
>    loop (matching `quark_bench.cpp`'s own shape exactly); the flush+sleep is still there, just
>    after the measurement, for teardown safety. **This is the fix that changes conclusions**: this
>    file's headline "Quark wins single-threaded throughput by ~1.37-1.39×" claim, repeated across
>    every session above, was measuring a bugged CAF baseline. `caf_stress_bench.cpp` was checked
>    for the same pattern and is clean (its only `sleep_for` is a legitimate polling interval,
>    captured before the final timestamp, not padding after it).
>
> **Every table from here down reflects the bug-fixed binaries, a fresh (third) session.** Numbers
> above this callout (and the "Re-run #1" callout above) are kept for history, not deleted, but
> should now be read as "measuring a since-fixed methodology bug," the same disposition this file
> already gives the original pre-warmup-fix numbers below.

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
| Spawn (10k actors) | 772 ns | 775 ns | 936 ns | 4,255 ns |
| Tell p50 / p99 / p999 | 100 / 700 / 1,100 ns | 100 / 2,300 / 2,900 ns | 100 / 400 / 800 ns | 100 / 200 / 3,600 ns |
| Ask p50 / p99 / p999 | 600 / 1,900 / 3,800 ns | 1,300 / 3,500 / 9,000 ns | 2,000 / 16,500 / 39,000 ns | 15,500 / 22,200 / 33,000 ns |
| Throughput | 6.90 M/s | 7.27 M/s | **7.73 M/s** | **7.38 M/s** |

**With both bench-fidelity bugs fixed, CAF now leads single-threaded throughput** (7.73 vs
6.90 M/s, CAF **1.12×** ahead) and the two engines are essentially tied at 12 workers (7.38 vs
7.27 M/s, CAF barely ahead). **This reverses every prior session's headline "Quark wins
single-threaded throughput" finding in this file** — that finding was built on a CAF baseline
deflated ~25-30% by the `sleep_for(500ms)`-inside-the-timed-window bug described in the callout
above, not a real engine capability gap. Quark still wins clearly elsewhere: spawn at 12 workers
(775 ns vs CAF's 4,255 ns, **5.5×**, unaffected by either fix — CAF's spawn cost keeps ballooning
with scheduler thread count, Quark's doesn't) and `ask` p50 at both worker counts (600 vs 2,000 ns
@1, **3.3×**; 1,300 vs 15,500 ns @12, **~11.9×** — CAF's ask latency degrades badly under its own
multi-threaded scheduler). `tell` p50 is tied at 100 ns for both engines at both worker counts.

## MPSC scaling, shared mailbox (N producers → 1 actor)

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | 7.11 | 8.81 | 1.24× |
| 2 | 8.51 | 11.95 | 1.40× |
| 4 | 9.83 | 15.28 | 1.55× |
| 8 | 12.93 | 21.23 | 1.64× |
| 12 | 12.97 | 22.84 | 1.76× |

Scaling P=1→P=12: Quark 7.11→12.97 M/s (**1.82×**), CAF 8.81→22.84 M/s (**2.59×**). CAF leads at every producer count, and the gap widens steadily with P — this is the shared-mailbox shape, where every producer contends for the same actor's mailbox and the same `MessagePool` partition-lane traffic, and CAF's scheduler scales that contention better. Quark's P=1 number here (7.11 M/s) now sits close to the single-threaded throughput bench's 6.90 M/s, as it should — this is the fix's direct validation.

```
Quark            1 | ##############                                7.11
                 2 | ################                              8.51
                 4 | ###################                           9.83
                 8 | #########################                    12.93
                12 | #########################                    12.97

CAF              1 | #################                             8.81
                 2 | #######################                       11.95
                 4 | #############################                 15.28
                 8 | #########################################     21.23
                12 | ############################################  22.84
```

## Sharded MPSC scaling (N producers → N actors, contention-free ceiling)

Same per-producer volume/pacing as above, but `producer[i]` only ever `tell()`s `actor[i]` — one shard/worker per producer on the Quark side, one scheduler thread per producer on the CAF side. No two producers ever contend for the same mailbox; this isolates enqueue/dispatch cost from mailbox/free-list contention.

| Producers | Quark (M/s) | CAF (M/s) | CAF/Quark |
|---|---|---|---|
| 1 | **7.80** | 6.70 | **0.86×** (Quark ahead) |
| 2 | 8.78 | 14.09 | 1.61× |
| 4 | 13.16 | 21.24 | 1.61× |
| 8 | 17.93 | 24.53 | 1.37× |
| 12 | 17.76 | 28.25 | 1.59× |

```
Quark            1 | ############                                  7.80
                 2 | ##############                                8.78
                 4 | #####################                         13.16
                 8 | ############################                  17.93
                12 | ############################                  17.76

CAF              1 | ##########                                    6.70
                 2 | ######################                        14.09
                 4 | #################################             21.24
                 8 | ######################################        24.53
                12 | ############################################  28.25
```

**Quark leads at P=1 this session** (7.80 vs 6.70 M/s) — a genuine reversal, though CAF's P=1
number here (6.70) being *lower* than its own shared-mailbox P=1 number (8.81, same paragraph
above) for a strictly-easier topology (no shared-mailbox contention at all) is itself a little odd
and is flagged as noisy rather than trusted as a real CAF characteristic — every number in this
file is a single unpinned-host run. From P=2 onward CAF leads clearly (1.37×-1.61×). The
oversubscription regression is real but much smaller this session: Quark's throughput barely moves
from P=8 (17.93 M/s) to P=12 (17.76 M/s, **-0.9%**, versus -4.6% and -10.4% in the two prior
sessions) while CAF keeps climbing (24.53→28.25 M/s). Scaling P=1→P=12: Quark 7.80→17.76 M/s
(**2.28×**), CAF 6.70→28.25 M/s (**4.22×**) — CAF's fold-scaling is now clearly higher than
Quark's, a different shape from both prior sessions (where Quark's fold-scaling led despite
trailing in absolute terms) and a direct consequence of CAF's now-much-higher, no-longer-deflated
P=1 baseline in the *other* two tables — though this table's own CAF-P=1 anomaly noted above means
this specific fold-scaling number should be treated with extra caution.

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
   `worker_loop` (see Key Finding #5 below).

## Key findings

1. **A bench-fidelity bug flipped the single-threaded throughput headline — this is the biggest correction in this file's history, bigger than the warmup fix below.** `caf_bench.cpp` measured its throughput number with a 500ms sleep captured *inside* the timed window, deflating every CAF single-threaded throughput figure in every prior session by ~25-30%. Fixed (see the "Re-run #2" callout in the Machine section): **CAF now leads single-threaded throughput, 7.73 vs 6.90 M/s (1.12×)**, reversing the "Quark wins by ~1.37-1.39×" claim every earlier session in this file made. A second, related bug (unconditional per-message clock reads in the MPSC/sharded benches, purely to feed a 1-in-16 sample) was also fixed, which is why this session's P=1 rows in the MPSC tables now actually match the single-threaded table's number, closing the exact discrepancy that prompted this investigation. Treat every throughput comparison from earlier sessions in this file's git history as measuring these bugs, not a stable property of either engine.
2. **Warmup discipline was a real, separate fix from #1 and remains valid.** Switching every bench's warmup from a fixed op count to ~1s of wall-clock time (see that callout) fixed a >2× run-to-run swing in Quark's own numbers and flipped sharded MPSC from "Quark ahead at P=1/P=2" to "CAF ahead everywhere." Both fixes are real; they just corrected different things (#1 was a bug in measuring CAF, this one was a bug in measuring Quark) and happened to land in the same file across different sessions.
3. **Quark still wins clearly on `ask` latency and spawn at high worker counts — unaffected by either bug fix.** Quark's spawn cost stays flat with worker count (775 ns @12) while CAF's balloons (4,255 ns @12, **5.5×** worse). Quark's `ask` p50 leads CAF's at both worker counts (600 vs 2,000 ns @1, **3.3×**; 1,300 vs 15,500 ns @12, **~11.9×**) — CAF's ask latency degrades badly under its own multi-threaded scheduler, Quark's doesn't.
4. **The shared-mailbox gap is real and CAF-favoring at every producer count** (1.24×-1.76× this session) — sharding away mailbox contention narrows it somewhat at low P (sharded P=1: Quark actually leads, 0.86×) but CAF pulls ahead again from P=2 onward and the gap widens with P in both topologies, the shared-mailbox shape where every producer contends for the same actor's mailbox and `MessagePool` partition-lane traffic.
5. **Oversubscription hurts Quark specifically**, and this is the one finding that has now been directly investigated, not just observed — unaffected by the bench-fidelity fixes above (the stress test's `sent`/`received` counters and duration measurement were never in the buggy code path). The sharded bench's one-worker-per-producer design means P=12 spawns 24 OS threads on a 12-thread machine; Quark's throughput barely regresses P=8→P=12 this session (17.93→17.76 M/s, **-0.9%**, smaller than either prior session's -4.6%/-10.4%) while CAF's keeps climbing. The sustained stress test above quantifies the mechanism as a latency-tail cost, not a correctness or throughput-collapse one: aggregate throughput and message integrity hold in every run, but Quark's tail latency blows up once oversubscription starts. **Root-cause confirmed, and multiple candidate fixes built, proven, and re-tested across four rounds.** [ADR-038](../../decisions/ADR-038-scheduler-oversubscription-tail-latency.md) traced this to `try_drain_shard`'s `drain_owner` CAS stranding other workers behind a preempted shard owner. Round 1 built and proved three competing fixes (a naive `yield()`-based backoff was *proven counterproductive*; bounded cooperative drain-owner eviction won). Round 2 refuted the hope that its p999 side-effect was a proving-harness artifact. Round 3 built a cheaper heuristic that materially helped but didn't close the gap. **Round 4 combined it with yield-escalation, found no further improvement, and — more importantly — found that re-measuring the exact same configuration in a fresh session flipped its sign** (+9.9% worse → -1.0% better), evidence this shared host's noise floor exceeds the effect sizes being chased. The investigation is now **closed pending a quiet, pinned CI host**, not resolved; every knob stays at its default-off/zero value. See `002-Scheduler.md`'s "Bounded cooperative drain-owner eviction (ADR-038)" section and the ADR's Rounds 2-4 for the full evidence.
6. **Quark's single-producer `throughput` bench gets slightly faster with more workers this session** (6.90 M/s @1 → 7.27 M/s @12, +5%) — a change from both prior sessions' slight *slowdown* (~9-10%), plausibly just session-to-session noise on a shared host rather than a real trend reversal; not concerning either way since the effect size is small in both directions.
7. **Quark has no mailbox backpressure today.** `MessagePool` cold-allocates unboundedly when a partition is exhausted rather than throttling producers (RFC 022 documents this as an open gap, not yet implemented) — confirmed by the stress-test incident described below: hammering a single shared mailbox harder than its one consumer can drain grew RSS to ~18GB in seconds. Not a risk under the paired/sharded topologies real workloads use, but a real gap if a workload ever does route sustained heavy traffic at a single actor identity.
8. **Every absolute number in this file remains a single unpinned-host run**, now across three same-day sessions that have each moved meaningfully from the last — two for real, disclosed reasons (warmup, then bench-fidelity bugs) and some purely from host noise (see ADR-038 Round 4's finding that even a *fixed* configuration's measured sign can flip session-to-session). A `taskset`-pinned Linux CI re-run remains the natural follow-up before treating any absolute number here as canonical; ratios and directional trends, cross-checked against ADR-level evidence where it exists (like ADR-038), are the more trustworthy signal this file can currently offer.

## History

Full narrative of each investigation round (the `MessagePool` partitioning fix, ADR-035's park/wake backoff, ADR-036's activation linger, the `ReclaimSink` bug that made every pre-2026-08-01 Quark number in this file understate real throughput, ADR-037's TLS magazine, ADR-038's scheduler-oversubscription investigation across four rounds, and the bench-fidelity fixes described in the "Re-run #2" callout above) is preserved in git history for this file and in the relevant ADRs:

- `decisions/ADR-035-worker-park-wake-backoff-policy.md`
- `decisions/ADR-036-activation-linger-idle-churn-reduction.md`
- `decisions/ADR-037-message-pool-freelist-sync.md`
- `decisions/ADR-038-scheduler-oversubscription-tail-latency.md` — the sustained stress test's
  oversubscription tail-latency finding (Key Finding #5), four proving rounds (a candidate fix
  built and proven — Bounded Cooperative Drain-Owner Eviction; refuted against the real
  `worker_loop`; a cheaper heuristic that materially helped but didn't close the gap; a combined
  attempt that didn't help further, plus a measurement-noise finding that closed the investigation
  pending a quiet host) — stays default-off throughout.
- **Bench-fidelity fixes (2026-08-01, same day as the ADR-038 patch re-run)**: user-reported
  discrepancy between the "1 producer" rows of the MPSC/sharded-MPSC tables and the single-threaded
  throughput table led to finding and fixing (a) unconditional per-message clock reads in four bench
  files that only needed to sample 1-in-16 messages, (b) unread per-message atomic counters in two
  `PingActor`/`ping_actor` handlers, and (c) a `sleep_for(500ms)` captured inside `caf_bench.cpp`'s
  timed throughput window, which had deflated every CAF single-threaded throughput number in this
  file's history by ~25-30% — reversing this file's headline single-threaded-throughput conclusion.
  See the "Re-run #2" callout in the Machine section for the full writeup; no dedicated ADR, this
  was a bench-harness fix, not an engine design decision.

Prior versions of this README (`git log -- bench/caf_comparison/README.md`) have the session-by-session before/after tables for each of those fixes if you need the historical progression rather than just the current state above.
