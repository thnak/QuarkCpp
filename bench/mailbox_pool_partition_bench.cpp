// Sweeps MessagePool's num_partitions (message_pool.hpp) to reproduce, in-repo, the finding from
// investigating GitHub issue #4 (Quark vs CAF): a single shared free-list serializes every
// concurrent producer regardless of target actor population (measured: throughput flatlines
// whether messages spread across 100 or 100,000 actors); per-thread partitioning removes that
// (measured 6-7x gain). This closes the gap ADR-020 flagged as unproven ("MessagePool::acquire()'s
// allocation behavior under producer/consumer imbalance").
//
// Informational only — no established floor/ceiling exists yet for this axis, so this prints plain
// M msg/s per config with NO [goal]/[hard]/[MISS] tokens. It is auto-discovered by bench/CMakeLists
// and therefore runs under `bench-gate` (ci_bench_gate.sh), which treats a non-zero exit as a HARD
// failure — this bench always exits 0 regardless of the numbers it measures.
//
// Run: mailbox_pool_partition_bench [producers=3] [workers=1]   (sweeps num_partitions 1..producers)
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace quark;

namespace {

struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<int>;
    void handle(const int&) noexcept {}
};

// One full config: `producers` threads each `tell()` `per_producer` messages to one actor, pool
// partitioned `num_partitions` ways. Returns aggregate M msg/s (send-side wall time, matching the
// quark_mpsc_bench.cpp methodology this mirrors). Correctly wires pool.sink() as the reclaim sink
// (unlike the original quark_mpsc_bench.cpp, which omitted it — a separate, already-identified
// bug: an unwired ReclaimSink cold-allocates forever instead of reusing the free-list) so this
// bench isolates ONLY the partitioning effect, not that unrelated defect.
double run_once(unsigned producers, unsigned workers, std::size_t num_partitions,
                 std::uint64_t per_producer) {
    Engine eng(EngineConfig{workers, workers, 64, 1024});
    detail::MessagePool pool(4096, num_partitions);
    auto spawned = eng.spawn<PingActor>(1, pool.sink());
    if (!spawned) {
        std::fprintf(stderr, "mailbox_pool_partition_bench: spawn failed\n");
        return 0.0;
    }
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<PingActor> ref = router.get<PingActor>(1);
    eng.start();

    // Warmup (discarded from the timed measurement).
    {
        std::vector<std::thread> ths;
        ths.reserve(producers);
        for (unsigned t = 0; t < producers; ++t) {
            ths.emplace_back([&] {
                for (int i = 0; i < 2000; ++i) ref.tell(i);
            });
        }
        for (auto& th : ths) th.join();
    }

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ths;
    ths.reserve(producers);
    const auto t0 = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < producers; ++t) {
        ths.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < per_producer; ++i)
                ref.tell(static_cast<int>(i));
        });
    }
    while (ready.load(std::memory_order_acquire) < producers) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : ths) th.join();
    const auto t1 = std::chrono::steady_clock::now();

    eng.stop();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double total = static_cast<double>(per_producer) * static_cast<double>(producers);
    return secs > 0.0 ? total / secs / 1e6 : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned producers = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 3;
    unsigned workers = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1;
    if (producers == 0) producers = 1;
    if (workers == 0) workers = 1;
    constexpr std::uint64_t kPerProducer = 200'000;

    std::printf("=== mailbox_pool_partition_bench (informational — no ci-gate budget tokens) ===\n");
    std::printf("producers=%u workers=%u per_producer=%llu\n\n", producers, workers,
                static_cast<unsigned long long>(kPerProducer));

    for (std::size_t parts = 1; parts <= producers; ++parts) {
        const double mps = run_once(producers, workers, parts, kPerProducer);
        std::printf("  num_partitions=%-3zu : %7.2f M msg/s\n", parts, mps);
    }

    std::printf(
        "\n[info] num_partitions=1 reproduces the pre-fix shared-pool flatline (issue #4); "
        "num_partitions=producers reproduces the measured throughput gain from per-thread "
        "partitioning.\n");
    return 0;
}
