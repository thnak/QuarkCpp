// Hot-path microbenchmark for the 006 `ask` request/reply verb vs the 023 budgets (line 58 / the
// ADR-007 dispatch & ask block). An `ask` is a full round trip: a caller posts an Ask<Q,R> envelope
// carrying a pooled Responder, the lane drains + dispatches it, the handler `respond()`s through the
// shard-pooled ReplyCell, and the caller reads the resolved value.
//
// TWO NUMBERS, because they measure different things (023 is explicit that means lie and that the
// engine budgets its OWN overhead, not the OS):
//
//   A) ENGINE-OVERHEAD ask — the ADR-007 path the 1 µs budget actually governs: build envelope +
//      dispatch + pooled ReplyCell resolve, with NO cross-thread park. Measured by posting the ask
//      envelope DIRECTLY to the activation (the same MessagePool + stamp + ReplyCell building blocks
//      LocalRouter uses) and draining it inline on the SAME thread — so the number is the engine's
//      work, not the kernel's. Compare against 023 line 58: p50 ≤ 1 µs / p99 ≤ 5 µs / p99 hard ≤ 20 µs.
//      (ADR-007 proved p50 83.1 / p99 129.8 / p999 256.8 ns on the reference methodology.)
//
//   B) REALISTIC cross-thread `block_on(ref.ask())` over the running Engine — the true
//      developer-facing round trip: caller thread → futex park → woken worker lane → dispatch →
//      respond → wake caller. This ADDS the OS park/wake, inherently µs-class and a property of the
//      SCHEDULER/deployment, not the engine (as 023 excludes network RTT from the cross-node budget).
//      Reported for honesty; pinned `-c 0-1`, same NUMA node (this box: node0 = cores 0-15).
//
// Pin it: `taskset -c 0-1 build/bench/ask_bench` (section B needs the caller + one worker lane).
#include <cstdint>
#include <cstdio>
#include <memory>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/dispatch.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"
#include "quark/detail/reply_cell.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct GetSquare {
    std::uint64_t x;
};

// A real ask-handling actor: it responds through the Ask<Q,R> envelope's pooled Responder (ADR-007).
struct Squarer : Actor<Squarer, Sequential> {
    using protocol = Protocol<Ask<GetSquare, std::uint64_t>>;
    void handle(const Ask<GetSquare, std::uint64_t>& m) noexcept { m.respond(m.query.x * m.query.x); }
};

// ---- A) engine-overhead ask: build the envelope, post to the lane, drain inline (no futex). -----
void bench_ask_overhead() {
    using Env = Ask<GetSquare, std::uint64_t>;
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kSamples = 2'000'000;

    detail::MessagePool pool(64);
    detail::ReplyCellPool<std::uint64_t> cells(64);
    Squarer actor;
    Activation act{&actor, Squarer::dispatch_table(), pool.sink()};

    std::vector<double> ns;
    ns.reserve(kSamples);
    std::uint64_t checksum = 0;

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const auto t0 = pal::clock::now();
        // Acquire a pooled reply cell + build the Ask envelope into a pooled descriptor (exactly what
        // LocalRouter::make_descriptor does, minus the engine courier — no run-queue, no wake).
        auto lease = cells.acquire();
        detail::MessagePool::Slot slot = pool.acquire(&detail::destroy_payload<Env>);
        Descriptor* d = slot.desc;
        ::new (slot.payload) Env{GetSquare{i}, detail::Responder<std::uint64_t>{lease.cell, lease.gen}};
        d->payload = slot.payload;
        d->payload_size = static_cast<std::uint32_t>(sizeof(Env));
        d->trace_id = 0;
        d->deadline_ns = 0;
        stamp<Squarer, Env>(*d);

        act.post(d);
        (void)act.try_acquire();
        act.drain_step(64);           // dispatch → handler respond()s → cell resolved → desc reclaimed
        (void)act.close_out();

        result<std::uint64_t> r = lease.cell->take();  // resolved during drain_step
        cells.release(lease.cell);
        const auto t1 = pal::clock::now();
        if (!r.has_value()) { std::fprintf(stderr, "ask overhead: no value at i=%llu\n",
                                           static_cast<unsigned long long>(i)); return; }
        checksum += r.value();
        if (i >= kWarmup) ns.push_back(bench::ns_between(t0, t1));
    }

    bench::Stats st = bench::summarize(ns);
    std::printf("A) engine-overhead ask (build→post→dispatch→respond→resolve, same thread, NO futex):\n");
    std::printf("  p50  = %8.1f ns  %s (023 line-58: p50 ≤ 1000 / p99 ≤ 5000 / p99 hard ≤ 20000)\n",
                st.p50, bench::lat_verdict(st.p50, bench::budget::ask_p50_goal_ns, 5'000.0));
    std::printf("  p99  = %8.1f ns  %s\n", st.p99,
                bench::lat_verdict(st.p99, 5'000.0, bench::budget::ask_p99_hard_ns));
    std::printf("  p999 = %8.1f ns\n", st.p999);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f\n", st.mean, st.stddev, st.cov);
    std::printf("  (checksum=%llu)\n", static_cast<unsigned long long>(checksum));
}

// ---- B) realistic cross-thread ask over the running engine (block_on). --------------------------
void bench_ask_realistic() {
    constexpr std::uint64_t kWarmup = 5'000;
    constexpr std::uint64_t kSamples = 200'000;

    detail::MessagePool pool(4096);
    Squarer actor;
    auto act = std::make_unique<Activation>(&actor, Squarer::dispatch_table(), pool.sink());
    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Squarer>(9), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Squarer> ref = router.get<Squarer>(9);
    eng.start();

    std::vector<double> ns;
    ns.reserve(kSamples);
    std::uint64_t checksum = 0;
    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const auto t0 = pal::clock::now();
        result<std::uint64_t> r = block_on(ref.ask<std::uint64_t>(GetSquare{i}));
        const auto t1 = pal::clock::now();
        if (!r.has_value()) { std::fprintf(stderr, "ask realistic: no value\n"); eng.stop(); return; }
        checksum += r.value();
        if (i >= kWarmup) ns.push_back(bench::ns_between(t0, t1));
    }
    eng.stop();

    bench::Stats st = bench::summarize(ns);
    std::printf("B) realistic block_on(ask) over the running engine (adds OS futex park/wake):\n");
    std::printf("  p50  = %8.1f ns   p99 = %8.1f ns   p999 = %8.1f ns\n", st.p50, st.p99, st.p999);
    std::printf("  mean = %8.1f ns   CoV = %.3f   (futex-wake-bound; a scheduler/deployment cost,\n",
                st.mean, st.cov);
    std::printf("                                   NOT engine overhead — cf. 023 excluding network RTT)\n");
    std::printf("  (checksum=%llu)\n", static_cast<unsigned long long>(checksum));
}

}  // namespace

int main() {
    std::printf("== Quark 006/ADR-007 ask bench (pin with taskset -c 0-1) ==\n");
    bench_ask_overhead();
    std::printf("\n");
    bench_ask_realistic();
    return 0;
}
