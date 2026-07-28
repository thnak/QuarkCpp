// Mixed tell+ask workload benchmark, varying payload sizes — dimension 11 of the mailbox
// benchmark suite. Every other mailbox bench in this suite is homogeneous (one message type, one
// verb); this file is deliberately NOT: concurrent `tell` traffic (three payload sizes, round-
// robined) races concurrent `ask` traffic to the SAME actor, all through the real Engine/MessagePool
// path, so the numbers reflect what a mixed real workload — not a synthetic single-shape one —
// actually sees.
//
// Reports: aggregate tell throughput under concurrent ask load, and ask round-trip latency
// (p50/p99/p999) under concurrent tell load — each measures the OTHER traffic's interference.
//
// Pin it: `taskset -c 0-3 build/bench/mailbox_mixed_workload_bench` (2 tell threads + 2 ask threads
// + Engine worker lanes share <=4 cores by default — machine safety cap).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// Three payload sizes, all within MessagePool::kMaxPayload (192 B).
struct Small { std::uint64_t v; };                         // 8 B
struct Medium { std::uint64_t v; std::uint8_t pad[56]; };   // 64 B
struct Large { std::uint64_t v; std::uint8_t pad[168]; };   // 176 B
struct Query { std::uint64_t x; };

struct MixedActor : Actor<MixedActor, Sequential> {
    using protocol = Protocol<Small, Medium, Large, Ask<Query, std::uint64_t>>;
    std::atomic<std::uint64_t> small_count{0}, medium_count{0}, large_count{0};

    void handle(const Small&) noexcept { small_count.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Medium&) noexcept { medium_count.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Large&) noexcept { large_count.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Ask<Query, std::uint64_t>& m) noexcept { m.respond(m.query.x * 2 + 1); }
};

}  // namespace

int main() {
    std::printf("== Quark mixed tell+ask workload bench (dim 11; pin -c 0-3) ==\n");

    constexpr unsigned kTellThreads = 2;
    constexpr unsigned kAskThreads = 2;
    constexpr std::uint64_t kTellPerThread = 1'500'000;   // 3 payload shapes round-robined
    constexpr std::uint64_t kAskWarmup = 5'000;
    constexpr std::uint64_t kAskPerThread = 100'000;

    detail::MessagePool pool(8192, /*num_partitions=*/4);
    MixedActor actor;
    auto act = std::make_unique<Activation>(&actor, MixedActor::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{2, 1, 64, 64});
    eng.register_activation(actor_id_of<MixedActor>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<MixedActor> ref = router.get<MixedActor>(1);
    eng.start();

    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0};
    const unsigned total_threads = kTellThreads + kAskThreads;

    // --- Tell threads: round-robin Small/Medium/Large, timed as one block for aggregate mps. ------
    std::vector<std::thread> tellers;
    tellers.reserve(kTellThreads);
    std::vector<double> teller_secs(kTellThreads, 0.0);
    for (unsigned t = 0; t < kTellThreads; ++t) {
        tellers.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto t0 = pal::clock::now();
            for (std::uint64_t i = 0; i < kTellPerThread; ++i) {
                switch (i % 3) {
                    case 0: ref.tell(Small{i}); break;
                    case 1: ref.tell(Medium{i, {}}); break;
                    default: ref.tell(Large{i, {}}); break;
                }
            }
            const auto t1 = pal::clock::now();
            teller_secs[t] = std::chrono::duration<double>(t1 - t0).count();
        });
    }

    // --- Ask threads: block_on(ref.ask<>()) round trips, latency-sampled per thread. ---------------
    std::vector<std::thread> askers;
    askers.reserve(kAskThreads);
    std::vector<std::vector<double>> asker_ns(kAskThreads);
    std::vector<std::uint64_t> asker_failures(kAskThreads, 0);
    for (unsigned t = 0; t < kAskThreads; ++t) {
        asker_ns[t].reserve(kAskPerThread);
        askers.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < kAskWarmup + kAskPerThread; ++i) {
                const auto a0 = pal::clock::now();
                result<std::uint64_t> r = block_on(ref.ask<std::uint64_t>(Query{i}));
                const auto a1 = pal::clock::now();
                if (!r.has_value()) { ++asker_failures[t]; continue; }
                if (i >= kAskWarmup) asker_ns[t].push_back(bench::ns_between(a0, a1));
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < total_threads) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : tellers) th.join();
    for (auto& th : askers) th.join();
    eng.stop();

    double total_tell_secs = 0;
    for (double s : teller_secs) total_tell_secs = std::max(total_tell_secs, s);
    const std::uint64_t total_tells = static_cast<std::uint64_t>(kTellThreads) * kTellPerThread;
    const double tell_mps = total_tell_secs > 0.0 ? static_cast<double>(total_tells) / total_tell_secs / 1e6 : 0.0;

    std::printf("tell traffic (3 payload sizes, round-robined, concurrent with ask traffic):\n");
    std::printf("  aggregate %.2f M msg/s over %u threads (wall %.3fs)\n", tell_mps, kTellThreads,
                total_tell_secs);
    std::printf("  delivered: small=%llu medium=%llu large=%llu (expected %llu each of the 3 shapes"
                " combined = %llu)\n",
                static_cast<unsigned long long>(actor.small_count.load()),
                static_cast<unsigned long long>(actor.medium_count.load()),
                static_cast<unsigned long long>(actor.large_count.load()),
                static_cast<unsigned long long>(total_tells / 3),
                static_cast<unsigned long long>(total_tells));

    std::printf("\nask round-trip latency (concurrent with tell traffic above):\n");
    std::vector<double> pooled_ask_ns;
    std::uint64_t total_failures = 0;
    for (unsigned t = 0; t < kAskThreads; ++t) {
        pooled_ask_ns.insert(pooled_ask_ns.end(), asker_ns[t].begin(), asker_ns[t].end());
        total_failures += asker_failures[t];
    }
    bench::Stats s = bench::summarize(pooled_ask_ns);
    std::printf("  p50  = %8.1f ns\n", s.p50);
    std::printf("  p99  = %8.1f ns\n", s.p99);
    std::printf("  p999 = %8.1f ns\n", s.p999);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f   (n=%zu, failures=%llu)\n",
                s.mean, s.stddev, s.cov, s.n, static_cast<unsigned long long>(total_failures));
    std::printf("  (compare against ask_bench.cpp's quiescent block_on(ask) numbers — the delta is\n"
                "   the interference cost of concurrent tell traffic to the same actor/mailbox)\n");
    return 0;
}
