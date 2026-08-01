// Sharded MPSC benchmark: N producer threads, N actors, producer[i] only ever tells actor[i].
// Companion to quark_mpsc_bench.cpp (N producers -> 1 actor) — same total message volume, same
// per-thread pacing, but with mailbox contention removed: each actor has exactly one producer,
// so this measures the contention-free throughput ceiling that quark_mpsc_bench's single shared
// mailbox falls short of. Compare the two directly to see the cost of N-producers-on-1-mailbox
// contention vs N-producers-on-N-mailboxes.
//
// Build: see CMakeLists.txt
// Run:   quark_mpsc_sharded_bench.exe [num_producers]

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace quark;

namespace {

struct Stats {
    double p50, p99, p999, mean, min, max;
    size_t n;
};

Stats summarize(std::vector<double>& ns) {
    if (ns.empty()) return {};
    std::sort(ns.begin(), ns.end());
    Stats s;
    s.n = ns.size();
    s.min = ns.front();
    s.max = ns.back();
    auto p = [&](double q) { return ns[static_cast<size_t>(q * (ns.size() - 1))]; };
    s.p50 = p(0.50);
    s.p99 = p(0.99);
    s.p999 = p(0.999);
    double sum = 0;
    for (auto v : ns) sum += v;
    s.mean = sum / s.n;
    return s;
}

void print_stats(const char* label, const Stats& s) {
    std::printf("%s  (n=%zu)\n", label, s.n);
    std::printf("  p50  = %8.1f ns   p99  = %8.1f ns   p999 = %8.1f ns\n",
                s.p50, s.p99, s.p999);
    std::printf("  mean = %8.1f ns   min  = %8.1f ns   max  = %8.1f ns\n",
                s.mean, s.min, s.max);
}

// ---- Ping actor (fire-and-forget, counts messages) -----------------------

struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<int>;

    void handle(const int&) noexcept { /* just receive — mirrors quark_bench.cpp's throughput
                                           actor exactly; see quark_mpsc_bench.cpp's identical fix
                                           for why the original per-message atomic increment here
                                           was unread, unnecessary instrumentation tax */
    }
};

}  // namespace

int main(int argc, char** argv) {
    unsigned num_producers = 4;
    if (argc > 1)
        num_producers = static_cast<unsigned>(std::atoi(argv[1]));
    if (num_producers == 0)
        num_producers = std::thread::hardware_concurrency();
    if (num_producers > 64)
        num_producers = 64;

    // One actor per producer: same total volume as quark_mpsc_bench.cpp, but each producer's
    // messages land on its own dedicated actor/mailbox instead of a shared one.
    constexpr std::uint64_t kPerProducer = 3'000'000;
    // Warmup is wall-clock bounded, not a fixed op count — mirrors quark_mpsc_bench.cpp's fix.
    constexpr double kWarmupSeconds = 1.0;
    const std::uint64_t kTotal = kPerProducer * num_producers;

    // N shards / N workers: one actor and one worker "lane" per producer, so there's no
    // cross-producer mailbox contention and no shared-worker scheduling contention either.
    unsigned workers = num_producers;
    unsigned shards = num_producers;

    Engine eng(EngineConfig{workers, shards, 64, 1024});

    // Pool must exist before spawn() so pool.sink() can be wired in as each actor's ReclaimSink —
    // otherwise every tell() past the initial pool capacity cold-allocates a fresh Cell via
    // make_unique instead of recycling (mailbox_pool_partition_bench.cpp documents this exact bug).
    detail::MessagePool pool{4096, num_producers};

    // Spawn one PingActor per producer, keyed 0..num_producers-1 so the engine's key%shard_count
    // placement spreads them across distinct shards.
    std::vector<ActorId> aids;
    aids.reserve(num_producers);
    for (unsigned t = 0; t < num_producers; ++t)
        aids.push_back(eng.spawn<PingActor>(t, pool.sink()).value());

    LocalRouter router(eng.post_courier(), pool);
    std::vector<ActorRef<PingActor>> refs;
    refs.reserve(num_producers);
    for (unsigned t = 0; t < num_producers; ++t)
        refs.push_back(router.get<PingActor>(t));

    eng.start();

    // --- Warmup (wall-clock bounded, all producers stop together) ---
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(kWarmupSeconds);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < num_producers; ++t) {
            threads.emplace_back([&refs, t, deadline]() {
                std::uint64_t i = 0;
                for (;;) {
                    refs[t].tell(static_cast<int>(i & 0x7FFFFFFF));
                    ++i;
                    if ((i & 1023) == 0 && std::chrono::steady_clock::now() >= deadline) return;
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // --- Timed run ---
    std::vector<std::vector<double>> thread_samples(num_producers);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < num_producers; ++t) {
        threads.emplace_back([&, t]() {
            thread_samples[t].reserve(kPerProducer / 10);

            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                // See quark_mpsc_bench.cpp's identical fix: only pay the clock() cost on sampled
                // iterations, not every message.
                if ((i & 0xF) == 0) {
                    auto s = pal::clock::now();
                    refs[t].tell(static_cast<int>(t * kPerProducer + i));
                    auto e = pal::clock::now();
                    double ns = std::chrono::duration<double, std::nano>(e - s).count();
                    thread_samples[t].push_back(ns);
                } else {
                    refs[t].tell(static_cast<int>(t * kPerProducer + i));
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < num_producers) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& th : threads) th.join();

    auto t1 = std::chrono::steady_clock::now();

    eng.stop();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double total_mps = static_cast<double>(kTotal) / secs / 1e6;
    double per_thread_mps = static_cast<double>(kPerProducer) / secs / 1e6;

    std::printf("=== Quark Sharded MPSC Benchmark (N producers -> N actors) ===\n");
    std::printf("Producers/Actors: %u | Messages: %llu total, %llu each\n",
                num_producers,
                static_cast<unsigned long long>(kTotal),
                static_cast<unsigned long long>(kPerProducer));
    std::printf("Duration:  %.3f s\n\n", secs);

    std::printf("Throughput:\n");
    std::printf("  Total:      %8.2f M msg/s\n", total_mps);
    std::printf("  Per-thread: %8.2f M msg/s\n\n", per_thread_mps);

    std::vector<double> all;
    std::uint64_t total_samples = 0;
    for (auto& s : thread_samples) {
        total_samples += s.size();
        all.insert(all.end(), s.begin(), s.end());
    }
    auto stats = summarize(all);
    print_stats("enqueue latency (sampled, all producers):", stats);

    double min_pt = 1e9, max_pt = 0;
    for (auto& s : thread_samples) {
        auto st = summarize(s);
        if (st.mean < min_pt) min_pt = st.mean;
        if (st.mean > max_pt) max_pt = st.mean;
    }
    std::printf("  per-thread mean range: [%.0f, %.0f] ns\n\n", min_pt, max_pt);

    std::printf("Contention factor: %.2f (throughput vs 1-thread idealized)\n",
                total_mps / 3.55);  // normalized to Quark's 1-thread tell throughput

    return 0;
}
