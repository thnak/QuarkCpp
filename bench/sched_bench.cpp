// Hot-path microbenchmark for the 002 Scheduler vs the 023 budgets.
//
//  - Full-lifecycle throughput (post→schedule→drain→close-out), single core, msg/s/core
//    (023 sustained goal ≥ 10 M/s / floor ≥ 4 M).
//  - Local-tell latency through the scheduler, same-core occ-1, p50/p99/p999 ns
//    (023 goal ≤ 100 ns / hard ≤ 250 ns; p999 goal ≤ 5 µs / hard ≤ 50 µs).
//  - Cross-thread wakeup latency through the real worker pool (informational — a targeted futex wake
//    is inherently µs-class; this is the realistic post→woken-lane→dispatch tail).
//  - UniformFIFO zero-cost: RunQueue<UniformFIFO> enqueue+select vs a raw ActivationMpsc control.
//
// Pin it: `taskset -c 0 sched_bench` (latency/throughput/zero-cost are single-core); the wakeup
// probe needs a worker lane too, so pin that section with `-c 0-1`. NEVER saturate the machine.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/run_queue.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

struct Ping {
    std::uint64_t v;
};

struct Echo : Actor<Echo, Sequential> {
    using protocol = Protocol<Ping>;
    std::uint64_t sum = 0;
    void handle(const Ping& p) noexcept { sum += p.v; }
};

struct Beat : Actor<Beat, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<std::uint64_t>* seen = nullptr;
    void handle(const Ping&) noexcept { seen->fetch_add(1, std::memory_order_release); }
};

// A same-thread full lifecycle step: post → run-queue schedule → select → acquire → drain → close-out.
// Reuses one descriptor (0 alloc), one activation, one Schedulable — the honest per-core lifecycle.
void bench_throughput() {
    constexpr std::uint64_t kOps = 30'000'000;
    Echo actor;
    Activation act{&actor, Echo::dispatch_table()};
    RunQueue<UniformFIFO> rq;
    Schedulable s;
    s.activation = &act;
    Ping msg{1};
    Descriptor d;
    d.payload = &msg;
    stamp<Echo, Ping>(d);

    std::uint64_t checksum = 0;
    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        if (act.post(&d)) rq.enqueue(&s);
        const RunResult r = rq.select();
        if (r.status != RunStatus::Item) { std::fprintf(stderr, "sched miss\n"); return; }
        (void)act.try_acquire();
        act.drain_step(64);
        (void)act.close_out();
        checksum += r.item->band;
    }
    const auto t1 = pal::clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(kOps) / secs / 1e6;
    std::printf("full-lifecycle throughput (post→schedule→drain→close-out, single core):\n");
    std::printf("  %8.1f M msg/s/core   (023 sustained goal ≥ 10 / floor ≥ 4)  %s\n", mps,
                mps >= 10 ? "[goal]" : (mps >= 4 ? "[floor]" : "[MISS]"));
    std::printf("  (checksum=%llu, %.3fs, actor.sum=%llu)\n",
                static_cast<unsigned long long>(checksum), secs,
                static_cast<unsigned long long>(actor.sum));
}

void bench_latency() {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kSamples = 2'000'000;
    Echo actor;
    Activation act{&actor, Echo::dispatch_table()};
    RunQueue<UniformFIFO> rq;
    Schedulable s;
    s.activation = &act;
    Ping msg{1};
    Descriptor d;
    d.payload = &msg;
    stamp<Echo, Ping>(d);

    std::vector<double> ns;
    ns.reserve(kSamples);
    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const auto t0 = pal::clock::now();
        if (act.post(&d)) rq.enqueue(&s);
        const RunResult r = rq.select();
        (void)act.try_acquire();
        act.drain_step(64);
        (void)act.close_out();
        const auto t1 = pal::clock::now();
        if (r.status != RunStatus::Item) { std::fprintf(stderr, "sched miss\n"); return; }
        if (i >= kWarmup) ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    const double p50 = percentile(ns, 0.50), p99 = percentile(ns, 0.99), p999 = percentile(ns, 0.999);
    std::printf("local-tell latency through the scheduler (same-core occ-1):\n");
    std::printf("  p50   = %8.1f ns   (023 goal ≤ 100 / hard ≤ 250)  %s\n", p50,
                p50 <= 100 ? "[goal]" : (p50 <= 250 ? "[hard]" : "[MISS]"));
    std::printf("  p99   = %8.1f ns\n", p99);
    std::printf("  p999  = %8.1f ns   (023 goal ≤ 5000 / hard ≤ 50000)  %s\n", p999,
                p999 <= 5000 ? "[goal]" : (p999 <= 50000 ? "[hard]" : "[MISS]"));
}

// Realistic cross-thread tell: post one message, wait for the woken lane to dispatch it, record the
// round-trip. Inherently futex-wake-bound (µs), reported for honesty — NOT the 100 ns same-core path.
void bench_wakeup_latency() {
    constexpr std::uint64_t kWarmup = 5'000;
    constexpr std::uint64_t kSamples = 100'000;
    std::atomic<std::uint64_t> seen{0};
    Beat actor;
    actor.seen = &seen;
    Engine<> eng(EngineConfig{1, 1, 64, 64});
    auto act = std::make_unique<Activation>(&actor, Beat::dispatch_table());
    Schedulable* s = eng.register_activation(ActorId{TypeKey{1}, 0}, *act);

    std::vector<Ping> msgs(kWarmup + kSamples);
    std::vector<Descriptor> descs(kWarmup + kSamples);
    for (std::size_t i = 0; i < descs.size(); ++i) {
        msgs[i].v = i;
        descs[i].payload = &msgs[i];
        stamp<Beat, Ping>(descs[i]);
    }
    eng.start();
    std::vector<double> ns;
    ns.reserve(kSamples);
    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const std::uint64_t target = i + 1;
        const auto t0 = pal::clock::now();
        eng.post(s, &descs[i]);
        while (seen.load(std::memory_order_acquire) < target) { /* spin */ }
        const auto t1 = pal::clock::now();
        if (i >= kWarmup) ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    eng.stop();
    const double p50 = percentile(ns, 0.50), p99 = percentile(ns, 0.99), p999 = percentile(ns, 0.999);
    std::printf("cross-thread wakeup tell latency (post→woken-lane→dispatch, realistic, pin -c 0-1):\n");
    std::printf("  p50=%.0f ns  p99=%.0f ns  p999=%.0f ns  (futex-wake-bound; informational)\n", p50,
                p99, p999);
}

// UniformFIFO must be zero-cost vs a raw single MPSC (ADR-010 F1). Same enqueue+select tight loop.
void bench_zerocost() {
    constexpr std::uint64_t kOps = 50'000'000;
    Schedulable s;
    // control: raw ActivationMpsc
    ActivationMpsc raw;
    std::uint64_t c1 = 0;
    auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        raw.enqueue(&s);
        const RunResult r = raw.try_pop();
        c1 += (r.status == RunStatus::Item);
    }
    auto t1 = pal::clock::now();
    const double raw_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kOps);
    // uniform: RunQueue<UniformFIFO>
    RunQueue<UniformFIFO> uq;
    std::uint64_t c2 = 0;
    t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        uq.enqueue(&s);
        const RunResult r = uq.select();
        c2 += (r.status == RunStatus::Item);
    }
    t1 = pal::clock::now();
    const double uni_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kOps);
    const double delta = uni_ns - raw_ns;
    std::printf("UniformFIFO zero-cost (enqueue+select, ns/op):\n");
    std::printf("  raw ActivationMpsc = %6.3f ns   RunQueue<UniformFIFO> = %6.3f ns   Δ = %+.3f ns  %s\n",
                raw_ns, uni_ns, delta, (delta < 0.5 && delta > -0.5) ? "[within noise]" : "[check]");
    std::printf("  (c1=%llu c2=%llu — objdump the two select/enqueue TUs for byte-identity)\n",
                static_cast<unsigned long long>(c1), static_cast<unsigned long long>(c2));
}

}  // namespace

int main() {
    std::printf("== Quark 002 Scheduler bench (pin with taskset -c 0; wakeup section -c 0-1) ==\n");
    bench_throughput();
    bench_latency();
    bench_zerocost();
    bench_wakeup_latency();
    return 0;
}
