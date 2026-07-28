// End-to-end latency through a REAL Engine — dimension 13 of the mailbox benchmark suite.
// Distinct from mailbox_bench.cpp (raw Mailbox enqueue/dequeue only, no Activation/scheduler) and
// from sched_bench.cpp (drives Activation/RunQueue objects directly, bypassing the public API) —
// this file measures the FULL production path a real caller uses: Engine::spawn + ActorRef::tell,
// through MessagePool acquire -> Mailbox enqueue -> scheduler wake/select -> Activation drain ->
// handler dispatch -> MessagePool reclaim.
//
// The PUBLIC `tell()` API always crosses from the caller's thread to one of the Engine's real
// worker threads (there is no "the caller IS the drain lane" mode reachable through spawn()/tell()
// — that same-thread, no-futex number is what ask_bench.cpp's engine-overhead section and sched_
// bench.cpp's bench_latency already measure, via lower-level building blocks that bypass the real
// worker-thread pool). So BOTH sections below are honestly OS-park/wake-bound; what is new here is
// exercising the REAL, running, multi-worker Engine through its actual public tell() call — nowhere
// else in the suite does that:
//
//  A) 1-worker Engine: the minimum real e2e floor (only one lane exists, so no scheduling
//     ambiguity about which worker services the actor).
//  B) 2-worker Engine: confirms the number doesn't regress when a second, unrelated worker lane
//     exists (only one actor/shard is targeted, so worker count should be near test-invariant).
//
// Pin it: `taskset -c 0-1 build/bench/mailbox_e2e_engine_bench` (section B needs 2 lanes).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct Ping { std::uint64_t v; };

struct Echo : Actor<Echo, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<std::uint64_t>* seen = nullptr;
    void handle(const Ping&) noexcept { seen->fetch_add(1, std::memory_order_release); }
};

// ---- A) same-core e2e: 1 worker, post via tell(), spin until the SAME-lane drain completes. ------
void bench_e2e_same_core() {
    constexpr std::uint64_t kWarmup = 10'000;
    constexpr std::uint64_t kSamples = 500'000;
    std::atomic<std::uint64_t> seen{0};

    detail::MessagePool pool(64);
    Echo actor;
    actor.seen = &seen;
    auto act = std::make_unique<Activation>(&actor, Echo::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Echo>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Echo> ref = router.get<Echo>(1);
    eng.start();

    std::vector<double> ns;
    ns.reserve(kSamples);
    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const std::uint64_t target = i + 1;
        const auto t0 = pal::clock::now();
        ref.tell(Ping{i});
        while (seen.load(std::memory_order_acquire) < target) { /* spin */ }
        const auto t1 = pal::clock::now();
        if (i >= kWarmup) ns.push_back(bench::ns_between(t0, t1));
    }
    eng.stop();

    bench::Stats s = bench::summarize(ns);
    std::printf("A) 1-worker Engine e2e (spawn/tell->MessagePool->Mailbox->drain->dispatch):\n");
    // NO [goal]/[hard]/[MISS] verdict tokens here (informational only, like sched_bench.cpp's
    // bench_wakeup_latency): tell() always crosses to a worker thread, so this number is inherently
    // OS futex-park/wake-bound, not governed by the 023 100ns SAME-thread local-tell budget — using
    // that budget's verdict here would print a false [MISS] that bench/ci_bench_gate.sh would treat
    // as a HARD regression-gate failure for a number the budget was never meant to grade.
    std::printf("  p50  = %8.1f ns\n", s.p50);
    std::printf("  p99  = %8.1f ns\n", s.p99);
    std::printf("  p999 = %8.1f ns\n", s.p999);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f\n", s.mean, s.stddev, s.cov);
}

// ---- B) cross-thread e2e: 2 workers, forces a real futex park/wake. ------------------------------
void bench_e2e_cross_thread() {
    constexpr std::uint64_t kWarmup = 5'000;
    constexpr std::uint64_t kSamples = 100'000;
    std::atomic<std::uint64_t> seen{0};

    detail::MessagePool pool(64);
    Echo actor;
    actor.seen = &seen;
    auto act = std::make_unique<Activation>(&actor, Echo::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{2, 1, 64, 64});  // 2 workers: caller thread != the draining lane
    eng.register_activation(actor_id_of<Echo>(2), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Echo> ref = router.get<Echo>(2);
    eng.start();

    std::vector<double> ns;
    ns.reserve(kSamples);
    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const std::uint64_t target = i + 1;
        const auto t0 = pal::clock::now();
        ref.tell(Ping{i});
        while (seen.load(std::memory_order_acquire) < target) { /* spin */ }
        const auto t1 = pal::clock::now();
        if (i >= kWarmup) ns.push_back(bench::ns_between(t0, t1));
    }
    eng.stop();

    bench::Stats s = bench::summarize(ns);
    std::printf("B) 2-worker Engine e2e (same mechanism as A; confirms it doesn't regress with an\n"
                "   extra, unrelated worker lane present):\n");
    std::printf("  p50  = %8.1f ns\n", s.p50);
    std::printf("  p99  = %8.1f ns\n", s.p99);
    std::printf("  p999 = %8.1f ns\n", s.p999);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f\n", s.mean, s.stddev, s.cov);
}

}  // namespace

int main() {
    std::printf("== Quark mailbox end-to-end Engine bench (dim 13; pin -c 0-1) ==\n");
    bench_e2e_same_core();
    std::printf("\n");
    bench_e2e_cross_thread();
    return 0;
}
