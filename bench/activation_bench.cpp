// Hot-path microbenchmark for the 001/002 activation LIFECYCLE — activate → process → deactivate —
// vs the 023 budgets (§Memory & density: cold activation ≤ 10 µs / hard ≤ 50 µs; idle density
// ≥ 1 M activations/GB, ADR-015 proved 192 B/idle). An actor is activated LAZILY on its first
// message and deactivated (returned to Idle) when its mailbox drains, so this path runs constantly in
// a large, sparse actor population. Three measurements:
//
//   1. IDLE FOOTPRINT — sizeof(Activation) for a Sequential actor (the engine overhead per idle
//      actor, excl. user state), and the density it implies (activations/GB). Deterministic — a
//      compile-time size, no timing noise. 023 hard ceiling: ≤ 2 KB/idle; goal ≥ 1 M/GB.
//   2. COLD ACTIVATION latency — a FRESH activation's first message: post → acquire (activate) →
//      dispatch → close_out (deactivate). Measured over many never-run activations so the caches are
//      genuinely cold (the honest "first message" cost). 023: ≤ 10 µs goal / ≤ 50 µs hard.
//   3. ACTIVATE/DEACTIVATE CYCLE throughput — the STEADY reused lifecycle (post → try_acquire →
//      drain_step → close_out → Idle), M cycles/s/core. This is the per-core lifecycle rate a sparse
//      population sustains; it must clear the 023 sustained-tell floor (≥ 10 M/s goal / ≥ 4 M hard).
//
// Pin it: `taskset -c 0 build/bench/activation_bench` — single core, never saturate.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/dispatch.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct Ping {
    std::uint64_t v;
};

struct Echo : Actor<Echo, Sequential> {
    using protocol = Protocol<Ping>;
    std::uint64_t sum = 0;
    void handle(const Ping& p) noexcept { sum += p.v; }
};

// Fill a descriptor (non-movable — holds atomics) in place, stamped for Echo/Ping. Reusable across
// activations: each completes it before the next post; complete()+release() bumps the generation so
// try_claim succeeds again.
void stamp_ping(Descriptor& d, Ping& msg) {
    d.payload = &msg;
    d.payload_size = static_cast<std::uint32_t>(sizeof(Ping));
    d.trace_id = 0;
    d.deadline_ns = 0;
    stamp<Echo, Ping>(d);
}

// 1) Idle footprint — a compile-time size, reported as density.
void report_footprint() {
    const std::size_t act_bytes = sizeof(Activation);
    const std::size_t actor_bytes = sizeof(Echo);
    const double per_gb = 1.0e9 / static_cast<double>(act_bytes);
    std::printf("1) idle footprint (engine overhead per idle activation, excl. user state):\n");
    std::printf("   sizeof(Activation) = %zu B  %s (023 hard ≤ 2048 B/idle)\n", act_bytes,
                act_bytes <= 2048 ? "[under ceiling]" : "[OVER]");
    std::printf("   sizeof(Echo actor) = %zu B  (the user's state, separate)\n", actor_bytes);
    std::printf("   density = %.2f M activations/GB  %s (023 goal ≥ 1 M/GB / hard ≥ 0.5 M/GB)\n",
                per_gb / 1e6,
                per_gb >= 1e6 ? "[goal]" : (per_gb >= 5e5 ? "[hard]" : "[MISS]"));
    std::printf("   (ADR-015 proved 192 B/idle on the reference build — this is the whole-object size,\n");
    std::printf("    which includes the embedded Mailbox; the per-idle marginal cost is the ADR figure)\n");
}

// 2) Cold activation — fresh, never-run activations, one message each, caches cold.
void bench_cold_activation() {
    constexpr std::uint64_t kActivations = 200'000;

    // Pre-construct all activations + actors up front (excluded from timing, per 023 "excl. user
    // ctor"). Processing them in order means the early ones are cache-cold by the time we reach them.
    std::vector<std::unique_ptr<Echo>> actors;
    std::vector<std::unique_ptr<Activation>> acts;
    actors.reserve(kActivations);
    acts.reserve(kActivations);
    for (std::uint64_t i = 0; i < kActivations; ++i) {
        actors.push_back(std::make_unique<Echo>());
        acts.push_back(std::make_unique<Activation>(actors.back().get(), Echo::dispatch_table()));
    }

    Ping msg{1};
    Descriptor d;
    stamp_ping(d, msg);

    std::vector<double> ns;
    ns.reserve(kActivations);
    std::uint64_t checksum = 0;
    for (std::uint64_t i = 0; i < kActivations; ++i) {
        Activation* a = acts[i].get();
        const auto t0 = pal::clock::now();
        a->post(&d);                 // activate edge: Idle → Scheduled
        (void)a->try_acquire();      // Scheduled → Running
        a->drain_step(64);           // dispatch the first message
        (void)a->close_out();        // deactivate: Running → Idle
        const auto t1 = pal::clock::now();
        checksum += a->state() == ExecState::Idle ? 1 : 0;
        ns.push_back(bench::ns_between(t0, t1));
    }

    bench::Stats st = bench::summarize(ns);
    std::printf("2) cold activation (fresh activation's first message: activate→dispatch→deactivate):\n");
    std::printf("   p50  = %8.1f ns  %s (023 goal ≤ 10000 / hard ≤ 50000)\n", st.p50,
                bench::lat_verdict(st.p50, 10'000.0, 50'000.0));
    std::printf("   p99  = %8.1f ns   p999 = %8.1f ns\n", st.p99, st.p999);
    std::printf("   mean = %8.1f ns   CoV = %.3f\n", st.mean, st.cov);
    std::printf("   (deactivated-to-Idle count = %llu / %llu)\n",
                static_cast<unsigned long long>(checksum), static_cast<unsigned long long>(kActivations));
}

// 3) Steady activate/deactivate cycle throughput — reused activation, full lifecycle per op.
void bench_cycle_throughput() {
    constexpr std::uint64_t kOps = 30'000'000;
    Echo actor;
    Activation act{&actor, Echo::dispatch_table()};
    Ping msg{1};
    Descriptor d;
    stamp_ping(d, msg);

    std::uint64_t idle_after = 0;
    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        act.post(&d);              // Idle → Scheduled (activate)
        (void)act.try_acquire();   // Scheduled → Running
        act.drain_step(64);        // dispatch
        (void)act.close_out();     // Running → Idle (deactivate)
        idle_after += (act.state() == ExecState::Idle);
    }
    const auto t1 = pal::clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(kOps) / secs / 1e6;
    std::printf("3) activate/deactivate cycle (post→acquire→drain→close_out→Idle, reused, single core):\n");
    std::printf("   %8.2f M cycles/s/core  %s (023 sustained goal ≥ 10 / floor ≥ 4)   (%.3fs)\n", mps,
                bench::thr_verdict(mps, bench::budget::tell_sustained_goal_mps,
                                   bench::budget::tell_sustained_floor_mps), secs);
    std::printf("   (Idle-after-close_out = %llu/%llu, actor.sum=%llu)\n",
                static_cast<unsigned long long>(idle_after), static_cast<unsigned long long>(kOps),
                static_cast<unsigned long long>(actor.sum));
}

}  // namespace

int main() {
    std::printf("== Quark 001/002 activation lifecycle bench (pin with taskset -c 0) ==\n");
    report_footprint();
    std::printf("\n");
    bench_cold_activation();
    std::printf("\n");
    bench_cycle_throughput();
    return 0;
}
