# Quark benchmark harness (023)

Dev-tooling only — **nothing here is linked into the engine** (023 §Dependencies: "the engine gains nothing"). These microbenchmarks turn the hot-path claims the RFC leans on (*zero-cost*, *O(1)*, *no hot-path allocation*) into a **verdict a bench prints** against the [023 budgets](023-Performance-Targets-and-Budgets), not an assertion of taste. `014` proves the engine is *right* (virtual clock, deterministic sim); `023` proves it is *fast enough* (real hardware, wall clock). Same engine, opposite clock.

---

## 📊 Performance & CAF Comparison Charts

<div class="bench-chart-container" style="margin: 2rem 0; padding: 1.5rem; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px;">
  <h3 style="margin-top: 0; color: #38bdf8;">1. Latency & Throughput: Quark vs CAF</h3>
  <p style="color: #94a3b8; font-size: 0.9rem; margin-bottom: 1.5rem;">Lower is better for latency / time. Measured on 12-worker multi-threaded workloads.</p>
  
  <div style="display: flex; flex-direction: column; gap: 1.25rem;">
    <!-- Metric 1: Ask Latency (p50, 12 workers) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>Ask Latency p50 (12 workers)</span>
        <span style="color: #38bdf8;">Quark is ~11.0× Faster</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 9%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">226 ns</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">2,480 ns</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Metric 2: Ask Latency (p50, 1 worker) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>Ask Latency p50 (1 worker)</span>
        <span style="color: #38bdf8;">Quark is 3.3× Faster</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 30%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">147 ns</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">485 ns</div>
          </div>
        </div>
      </div>
    </div>

    <!-- Metric 3: Spawn 10k Actors (12 workers) -->
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.9rem; font-weight: 600; margin-bottom: 0.35rem;">
        <span>10k Actor Spawn Time (12 workers)</span>
        <span style="color: #38bdf8;">Quark is 5.0× Faster</span>
      </div>
      <div style="display: flex; flex-direction: column; gap: 0.4rem;">
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #38bdf8; font-weight: 600;">Quark</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 20%; height: 100%; background: linear-gradient(90deg, #0284c7, #38bdf8); border-radius: 4px; display: flex; align-items: center; padding-left: 8px; font-size: 0.75rem; font-weight: bold; color: white;">1.2 ms</div>
          </div>
        </div>
        <div style="display: flex; align-items: center; gap: 0.75rem;">
          <span style="width: 70px; font-size: 0.82rem; color: #94a3b8;">CAF</span>
          <div style="flex: 1; background: rgba(255,255,255,0.05); border-radius: 4px; overflow: hidden; height: 22px;">
            <div style="width: 100%; height: 100%; background: rgba(244, 63, 94, 0.75); border-radius: 4px; display: flex; align-items: center; justify-content: flex-end; padding-right: 8px; font-size: 0.75rem; font-weight: bold; color: white;">6.0 ms</div>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>

<div class="bench-chart-container" style="margin: 2rem 0; padding: 1.5rem; background: rgba(255,255,255,0.03); border: 1px solid rgba(255,255,255,0.1); border-radius: 12px;">
  <h3 style="margin-top: 0; color: #10b981;">2. Quark Hot-Path Budgets (023 Specifications)</h3>
  <p style="color: #94a3b8; font-size: 0.9rem; margin-bottom: 1.5rem;">Measured headroom vs 023 budget target (Shorter bar = higher efficiency margin).</p>

  <div style="display: flex; flex-direction: column; gap: 1rem;">
    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Mailbox p50 (59 ns measured / ≤ 100 ns budget)</span>
        <span style="color: #10b981; font-weight: bold;">59% of budget</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 59%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Ask Overhead p50 (147 ns measured / ≤ 1,000 ns budget)</span>
        <span style="color: #10b981; font-weight: bold;">15% of budget</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 15%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>VirtualBins Placement p99 (12.5 ns measured / ≤ 20 ns budget)</span>
        <span style="color: #10b981; font-weight: bold;">62% of budget</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 62%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>

    <div>
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem; margin-bottom: 0.25rem;">
        <span>Tagless Wire Encode p99 (50 ns measured / ≤ 200 ns budget)</span>
        <span style="color: #10b981; font-weight: bold;">25% of budget</span>
      </div>
      <div style="background: rgba(255,255,255,0.05); border-radius: 4px; height: 14px; overflow: hidden;">
        <div style="width: 25%; height: 100%; background: #10b981; border-radius: 4px;"></div>
      </div>
    </div>
  </div>
</div>

---

## The reference machine (023 §"The reference machine")

Budgets are stamped on **one modern x86-64 server core** (Zen 4 / Sapphire Rapids class, ~3–4 GHz, warm L1/L2, release build, single core pinned, turbo noted per run). Numbers off other silicon are compared as a **ratio** (ARM64: ≤1.5× latency / ≥0.66× throughput), never as absolutes. Treat ns figures as regression tripwires and margin checks, not as the canonical stamped numbers (those live in the ADRs: ADR-005/006/007/009/010/013/016).

---

## Harness (`bench_harness.hpp`) — one place for stats and budgets

- **Percentiles, never just means** — p50/p99/p999 print with every latency line; the mean prints *alongside* (never instead of) the tail, because a design can post a great mean and a catastrophic p999 (a hidden lock, an alloc spike, a rehash).
- **Report variance** — stddev + coefficient of variation (CoV) print with every latency line, so a regression must exceed the noise band to count.
- **Warmup + steady-state** — the caller discards the first N iterations; the harness summarizes only steady-state samples.
- **PAL clock** — timing rides `pal::clock::now()` (019). A sub-clock-resolution op (e.g. the ~5 ns placement lookup, below steady_clock's ~25 ns per-call overhead) is reported as **amortized ns/op** over a tight loop instead of per-call percentiles — the honest std-only measurement at that scale.
- **The 023 budget table lives once** in `namespace quark::bench::budget` — benches reference it, never re-hardcode the numbers.

---

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

**Measured results, machine-of-record, and reproduce steps: [`../PERFORMANCE.md`](PERFORMANCE).**

---

## Two kinds of gate — which are CI-safe

The 023 budgets split into two enforcement classes:

- **Deterministic invariant gates (machine-independent — these ARE CI tests, under `tests/`):** descriptor+handle ≤64 B (`static_assert`), **0 hot-path heap allocations** (`*_noalloc_test`, a hooked global `operator new` counts every alloc), **0 cross-core RMW on the drain path** (`*_drain_rmw_check` objdump greps for lock/cmpxchg/xadd/xchg-mem/mfence), and the zero-cost objdump parity checks (UniformFIFO byte-identical, guarded-vs-no-guard ratio). These pass or fail deterministically on any machine and are the permanent gates.
- **Noise-sensitive numeric benchmarks (reported, NOT pass/fail here):** ns latency and M/s throughput. These are meaningful on a pinned reference core (023 §Regression gating), so they print a `[goal]`/`[hard]`/`[MISS]` verdict for eyeballing and trend-tracking.

---

## The regression gate (`ci_bench_gate.sh` / `bench-gate`)

The one place the numeric budgets become an actual pass/fail is the **hard-veto** gate — it never flaps on noise because a `[MISS]` means a design blew past the *hard ceiling* (2.5× the goal for latency), far outside any runner's noise band, not merely under the goal.

```bash
cmake --build build --target bench-gate      # builds every *_bench, then runs the gate
bench/ci_bench_gate.sh [build/bench]          # or run it directly against built benches
```

Grading (from the `[goal]`/`[hard]`/`[floor]`/`[MISS]` tokens the harness prints):

- **`[MISS]`** — a **Hard** budget violated (over the hard ceiling / under the hard floor) → the gate **FAILS** (exit 1). A bench that crashes counts as a hard failure. This is the veto 023 §"The budgets" demands ("a design that misses a Hard budget is rejected").
- **`[hard]` / `[floor]`** — a **Goal** missed but the hard bound held → **WARN** only; the gate stays green and prints the regression for trend-tracking.
- **`[goal]`** — met; silent pass.
