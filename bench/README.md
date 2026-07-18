# Quark benchmark harness (023)

Dev-tooling only — **nothing here is linked into the engine** (023 §Dependencies: "the engine gains
nothing"). These microbenchmarks turn the hot-path claims the RFC leans on (*zero-cost*, *O(1)*, *no
hot-path allocation*) into a **verdict a bench prints** against the [023 budgets](../023-Performance-Targets-and-Budgets.md),
not an assertion of taste. `014` proves the engine is *right* (virtual clock, deterministic sim); `023`
proves it is *fast enough* (real hardware, wall clock). Same engine, opposite clock.

## ⚠️ Machine safety — read first

**Every bench is single-threaded and MUST be pinned to one core.** This dev box can hang or power off if
all cores saturate. Run latency/throughput micros as:

```
taskset -c 0 build/bench/<name>
```

Never run a bench without `taskset`, never pin to more than the 4 permitted cores (`-c 0-3`), and there
is deliberately **no N-core aggregate-scaling bench** here (023's `≥0.8·N` line needs a quiesced
many-core CI box, not this machine). Build with `-j4`, never `-j$(nproc)`.

## The reference machine (023 §"The reference machine")

Budgets are stamped on **one modern x86-64 server core** (Zen 4 / Sapphire Rapids class, ~3–4 GHz, warm
L1/L2, release build, single core pinned, turbo noted per run). Numbers off other silicon are compared
as a **ratio** (ARM64: ≤1.5× latency / ≥0.66× throughput), never as absolutes. This repo's dev box is
**not** the reference core — treat its ns figures as regression tripwires and margin checks, not as the
canonical stamped numbers (those live in the ADRs: ADR-005/006/007/009/010/013/016).

## Harness (`bench_harness.hpp`) — one place for stats and budgets

- **Percentiles, never just means** — p50/p99/p999 print with every latency line; the mean prints
  *alongside* (never instead of) the tail, because a design can post a great mean and a catastrophic
  p999 (a hidden lock, an alloc spike, a rehash).
- **Report variance** — stddev + coefficient of variation (CoV) print with every latency line, so a
  regression must exceed the noise band to count.
- **Warmup + steady-state** — the caller discards the first N iterations; the harness summarizes only
  steady-state samples.
- **PAL clock** — timing rides `pal::clock::now()` (019). A sub-clock-resolution op (e.g. the ~5 ns
  placement lookup, below steady_clock's ~25 ns per-call overhead) is reported as **amortized ns/op**
  over a tight loop instead of per-call percentiles — the honest std-only measurement at that scale.
- **The 023 budget table lives once** in `namespace quark::bench::budget` — benches reference it, never
  re-hardcode the numbers.

## Benches

| Bench | Path measured | Budget (023) |
|---|---|---|
| `mailbox_bench` | 003 mailbox enqueue→dequeue | tell p50 ≤100/250 ns; peak ≥50/20 M/s |
| `sched_bench` | 002 post→schedule→drain→close-out; UniformFIFO zero-cost | tell ≤100 ns; sustained ≥10/4 M/s |
| `ask_bench` | 006/ADR-007 ask round-trip: engine-overhead vs realistic `block_on` | ask p50 ≤1 µs / p99 ≤5 µs |
| `stream_bench` | 024 credit-ring ingest: throughput, per-frame cost, vs discrete tell | ≥10/4 M/s; ≤100 ns/frame; ≥3× cheaper |
| `activation_bench` | 001/002 activate→process→deactivate; cold activation; idle footprint | cold ≤10/50 µs; ≥1 M/GB; cycle ≥10/4 M/s |
| `serialize_bench` | 016 tagless wire encode vs memcpy | ≤200/500 ns, near-memcpy |
| `supervision_bench` (+`_noguard`) | 007/ADR-009 handler-guard success-path cost | guarded/no-guard ratio ≤ noise |
| `placement_bench` | 010/026 VirtualBins O(1) vs raw HRW O(N) | placement p99 ≤20/50 ns, **N-independent** |

**Measured results, machine-of-record, and reproduce steps: [`../PERFORMANCE.md`](../PERFORMANCE.md).**

## Two kinds of gate — which are CI-safe

The 023 budgets split into two enforcement classes:

- **Deterministic invariant gates (machine-independent — these ARE CI tests, under `tests/`):**
  descriptor+handle ≤64 B (`static_assert`), **0 hot-path heap allocations** (`*_noalloc_test`, a hooked
  global `operator new` counts every alloc), **0 cross-core RMW on the drain path** (`*_drain_rmw_check`
  objdump greps for lock/cmpxchg/xadd/xchg-mem/mfence), and the zero-cost objdump parity checks
  (UniformFIFO byte-identical, guarded-vs-no-guard ratio). These pass or fail deterministically on any
  machine and are the permanent gates.
- **Noise-sensitive numeric benchmarks (reported, NOT pass/fail here):** ns latency and M/s throughput.
  These are meaningful only on a pinned, quiesced reference core (023 §Regression gating), so they print
  a `[goal]`/`[hard]`/`[MISS]` verdict for eyeballing and trend-tracking but are **not** wired as CTest
  pass/fail — a shared/noisy runner would flap the gate. Gate these on a dedicated CI box against a
  rolling baseline with a per-metric noise band (>5% sustained), never a single sample.

## The regression gate (`ci_bench_gate.sh` / `bench-gate`)

The one place the numeric budgets become an actual pass/fail is the **hard-veto** gate — it never flaps
on noise because a `[MISS]` means a design blew past the *hard ceiling* (2.5× the goal for latency), far
outside any runner's noise band, not merely under the goal.

```
cmake --build build --target bench-gate      # builds every *_bench, then runs the gate
bench/ci_bench_gate.sh [build/bench]          # or run it directly against built benches
```

Grading (from the `[goal]`/`[hard]`/`[floor]`/`[MISS]` tokens the harness prints):

- **`[MISS]`** — a **Hard** budget violated (over the hard ceiling / under the hard floor) → the gate
  **FAILS** (exit 1). A bench that crashes counts as a hard failure. This is the veto 023 §"The budgets"
  demands ("a design that misses a Hard budget is rejected").
- **`[hard]` / `[floor]`** — a **Goal** missed but the hard bound held → **WARN** only; the gate stays
  green and prints the regression for trend-tracking (the noisy-runner-safe half of 023 §"Regression
  gating"). On a dedicated reference box, wire these to a rolling-baseline noise band separately.
- **`[goal]`** — met; silent pass.

The gate is **not** in the default CTest set (perf belongs on a pinned, quiesced machine and the sweep is
slow). Enable a `perf`-labelled CTest entry with `-DQUARK_BENCH_CI_GATE=ON` then `ctest -L perf`. It is
skipped entirely under a sanitizer build (ASan/TSan make ns numbers meaningless). CI wires it in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) alongside the four-config correctness matrix.
Benches are pinned to ≤4 cores (repo machine-safety rule), auto-narrowed to the CPUs actually present.
