// The shared 023 microbenchmark harness — dev-tooling ONLY (never linked into the engine; 023
// §Dependencies "the engine gains nothing"). It pins the ONE place the 023 budget numbers live and the
// ONE statistics path every bench reports through, so a hot-path claim ("mailbox enqueue ≤100 ns")
// becomes a verdict a bench prints, not a number each bench re-hardcodes.
//
// 023 §"The benchmark harness" requirements, enforced here:
//   * PERCENTILES, NEVER JUST MEANS — p50/p99/p999 (the tail is where the bugs hide). `mean` is printed
//     ALONGSIDE, never instead of, the tail; a design can post a great mean and a catastrophic p999.
//   * REPORT VARIANCE — stddev + coefficient of variation (CoV) print with every latency line, so a
//     regression must exceed the noise band to count (a noisy sample is not a regression).
//   * WARMUP + STEADY-STATE — the caller discards the first `kWarmup` iterations; helpers here assume
//     the passed samples are already steady-state.
//   * PINNED CORE — every bench is single-threaded and run `taskset -c 0` (never saturate: this box can
//     hang/power off if all cores spin — see the repo machine-safety rule). Multi-thread throughput
//     benches cap to `taskset -c 0-3`, ≤4 threads, NEVER `hardware_concurrency()`.
//   * PAL CLOCK — timing rides `pal::clock::now()` (019 `mono_now`), high-resolution + low-overhead.
//
// A budget is Hard (a design that misses it is REJECTED) or Goal (aspirational; a miss is a tracked
// regression, not a veto) — 023 §"The budgets". The reference machine (023 §"The reference machine") is
// one modern x86-64 server core, release build, single core pinned; ns figures off other silicon are
// ratio-compared, not absolute. See bench/README.md.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "pal/pal.hpp"

namespace quark::bench {

// ---- The 023 budget table — the SINGLE source of the numbers (023 §"The budgets") --------------
// Latency budgets are ns; throughput budgets are M msg/s/core. `_goal` is the aspiration, `_hard` the
// veto ceiling/floor. Lower-is-better for latency; higher-is-better for throughput.
namespace budget {
// Hot-path latency (023 §Hot-path latency; ADR-007 tell→dispatch p50 46.8 / p99 62.1 ns).
inline constexpr double local_tell_goal_ns = 100.0;     // enqueue→dequeue(→dispatch), sequential
inline constexpr double local_tell_hard_ns = 250.0;
inline constexpr double tell_p999_goal_ns = 5'000.0;    // p999 local tell
inline constexpr double tell_p999_hard_ns = 50'000.0;
inline constexpr double ask_p50_goal_ns = 1'000.0;      // local ask round-trip (ADR-007 p50 83 ns)
inline constexpr double ask_p99_hard_ns = 20'000.0;
inline constexpr double wire_encode_goal_ns = 200.0;    // 016 tagless near-memcpy (ADR-016 p99 25–28 ns)
inline constexpr double wire_encode_hard_ns = 500.0;
inline constexpr double placement_p99_goal_ns = 20.0;   // 010/026 HRW/VirtualBins, N-independent (ADR-006)
inline constexpr double placement_p99_hard_ns = 50.0;

// Throughput (023 §Throughput) — M msg/s/core.
inline constexpr double tell_sustained_goal_mps = 10.0;
inline constexpr double tell_sustained_floor_mps = 4.0;
inline constexpr double tell_peak_goal_mps = 50.0;
inline constexpr double tell_peak_floor_mps = 20.0;

// Memory & density (023 §Memory & density) — bytes.
inline constexpr std::size_t descriptor_hard_bytes = 64;   // handle+descriptor ≤ one cache line (003)
}  // namespace budget

// ---- Statistics (023 §Microbenchmarks — percentiles + variance) --------------------------------
struct Stats {
    double p50 = 0, p99 = 0, p999 = 0;
    double mean = 0, stddev = 0, min = 0, max = 0;
    double cov = 0;                 // coefficient of variation = stddev/mean (the noise band)
    std::size_t n = 0;
};

[[nodiscard]] inline double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

// Summarize steady-state samples (the caller has already discarded warmup). Sorts in place.
[[nodiscard]] inline Stats summarize(std::vector<double>& samples) {
    Stats s;
    s.n = samples.size();
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    s.min = samples.front();
    s.max = samples.back();
    s.p50 = percentile(samples, 0.50);
    s.p99 = percentile(samples, 0.99);
    s.p999 = percentile(samples, 0.999);
    double sum = 0;
    for (double v : samples) sum += v;
    s.mean = sum / static_cast<double>(s.n);
    double var = 0;
    for (double v : samples) { const double d = v - s.mean; var += d * d; }
    s.stddev = std::sqrt(var / static_cast<double>(s.n));
    s.cov = s.mean > 0 ? s.stddev / s.mean : 0;
    return s;
}

// A lower-is-better latency verdict against a goal/hard pair.
[[nodiscard]] inline const char* lat_verdict(double v, double goal, double hard) {
    return v <= goal ? "[goal]" : (v <= hard ? "[hard]" : "[MISS]");
}
// A higher-is-better throughput verdict against a goal/floor pair.
[[nodiscard]] inline const char* thr_verdict(double v, double goal, double floor) {
    return v >= goal ? "[goal]" : (v >= floor ? "[floor]" : "[MISS]");
}

// Print a full latency line: the tail percentiles (verdict on p50 and p999) PLUS mean+variance, so the
// noise band is always visible and the mean can never masquerade as the whole story (023 §"means lie").
inline void report_latency(const char* label, Stats s, double p50_goal, double p50_hard,
                           double p999_goal, double p999_hard) {
    std::printf("%s  (n=%zu)\n", label, s.n);
    std::printf("  p50  = %8.1f ns  %s (goal ≤ %.0f / hard ≤ %.0f)\n",
                s.p50, lat_verdict(s.p50, p50_goal, p50_hard), p50_goal, p50_hard);
    std::printf("  p99  = %8.1f ns\n", s.p99);
    std::printf("  p999 = %8.1f ns  %s (goal ≤ %.0f / hard ≤ %.0f)\n",
                s.p999, lat_verdict(s.p999, p999_goal, p999_hard), p999_goal, p999_hard);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f  (noise band; regression must exceed it)\n",
                s.mean, s.stddev, s.cov);
}

// Print a throughput line (M msg/s/core) with a goal/floor verdict.
inline void report_throughput(const char* label, double mps, double goal, double floor, double secs) {
    std::printf("%s\n", label);
    std::printf("  %8.2f M msg/s/core  %s (goal ≥ %.0f / floor ≥ %.0f)   (%.3fs)\n",
                mps, thr_verdict(mps, goal, floor), goal, floor, secs);
}

// Convenience: nanoseconds between two PAL clock stamps.
[[nodiscard]] inline double ns_between(pal::clock::time_point a, pal::clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

}  // namespace quark::bench
