// Consumer/shard scaling benchmark — dimension 10 of the mailbox benchmark suite. Distinct from
// mailbox_scaling_bench.cpp's dim-9 "does ONE mailbox scale under many producers" (which holds the
// consumer side fixed at exactly one drain lane and adds producers): this file holds the PER-ACTOR
// load fixed and asks "does the ENGINE scale" by adding independent shards/workers, each draining
// its OWN population of actors' mailboxes in parallel — the question a real deployment with many
// actors actually faces. Mirrors this session's actor-population methodology (many actors spread
// across shards, not one hot mailbox) rather than mailbox_scaling_bench.cpp's single-mailbox one.
//
// A fixed pool of actors is spread across `shard_count` shards (ActorId hash placement, 002
// §Sharding); `worker_count` == `shard_count` so every shard gets its own dedicated drain lane in
// the common case (the realistic "one worker per shard" deployment shape). Producer threads
// round-robin `tell()` traffic across the WHOLE actor population, so load is genuinely spread, not
// concentrated on one mailbox.
//
// Machine safety: default sweep is shard/worker count in {1,2,4}; pin producers separately.
// `taskset -c 0-7 build/bench/mailbox_consumer_scaling_bench` gives producers + up to 4 workers
// headroom. Informational only — no [MISS]/[goal] tokens; always exits 0.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Ping { std::uint64_t v; };

struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<Ping>;
    std::uint64_t sum = 0;
    void handle(const Ping& p) noexcept { sum += p.v; }
};

constexpr unsigned kActorCount = 256;         // fixed actor population, spread across shards
constexpr unsigned kProducers = 4;            // machine-safety cap
constexpr std::uint64_t kPerProducer = 500'000;

double run_once(unsigned shard_workers) {
    Engine<> eng(EngineConfig{shard_workers, shard_workers, 64, 1024});
    detail::MessagePool pool(4096, /*num_partitions=*/kProducers);
    LocalRouter router(eng.post_courier(), pool);

    std::vector<ActorRef<PingActor>> refs;
    refs.reserve(kActorCount);
    for (unsigned key = 0; key < kActorCount; ++key) {
        auto spawned = eng.spawn<PingActor>(key, pool.sink());
        if (!spawned) { std::fprintf(stderr, "spawn failed for key=%u\n", key); return 0.0; }
        refs.push_back(router.get<PingActor>(key));
    }
    eng.start();

    // Warmup (discarded).
    {
        std::vector<std::thread> ths;
        ths.reserve(kProducers);
        for (unsigned t = 0; t < kProducers; ++t)
            ths.emplace_back([&] { for (int i = 0; i < 2000; ++i) refs[static_cast<unsigned>(i) % kActorCount].tell(Ping{1}); });
        for (auto& th : ths) th.join();
    }

    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (unsigned t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const unsigned actor_idx = static_cast<unsigned>((t * kPerProducer + i)) % kActorCount;
                refs[actor_idx].tell(Ping{1});
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < kProducers) std::this_thread::yield();
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();
    const auto t1 = std::chrono::steady_clock::now();
    eng.stop();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double total = static_cast<double>(kPerProducer) * kProducers;
    return secs > 0.0 ? total / secs / 1e6 : 0.0;
}

}  // namespace

int main() {
    std::printf("== Quark consumer/shard scaling bench (dim 10; %u actors, %u producer threads) ==\n",
                kActorCount, kProducers);
    std::printf("(pin e.g. `taskset -c 0-7`; sweep is shards==workers in {1,2,4} by default)\n\n");

    const std::vector<unsigned> sweep = {1, 2, 4};
    double base = 0.0;
    for (unsigned w : sweep) {
        double mps = run_once(w);
        if (w == 1) base = mps;
        std::printf("  shards=workers=%u  : %7.2f M msg/s   (scaling vs 1-shard = %.2fx)\n", w, mps,
                    base > 0 ? mps / base : 0.0);
    }
    std::printf(
        "\n[info] this is the ENGINE-scaling question (many independent actors/mailboxes, one\n"
        "drain lane per shard) — contrast with mailbox_scaling_bench.cpp's dim-9 producer-scaling\n"
        "sweep, which holds the consumer side fixed at ONE mailbox/one drain lane and adds\n"
        "producers instead. A healthy engine should scale materially better here than dim 9's\n"
        "single-mailbox curve, since shards/workers are actually independent drain lanes.\n");
    return 0;
}
