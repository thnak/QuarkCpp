// MPSC (multi-producer, single-consumer) benchmark: multiple threads fire messages
// at a single actor's mailbox. Measures how each framework scales under contention.
//
// Both benches: N producer threads, 1 actor, total M messages.
// Metrics: total throughput (M msg/s), per-thread throughput, p50/p99/p999 enqueue latency.
//
// Build: see CMakeLists.txt
// Run:   quark_mpsc_bench.exe [num_producers]

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
    std::atomic<std::uint64_t> count{0};

    void handle(const int&) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
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

    // Total messages: scale with producers so each sends enough
    constexpr std::uint64_t kPerProducer = 500'000;
    constexpr std::uint64_t kWarmupPerProducer = 10'000;
    const std::uint64_t kTotal = kPerProducer * num_producers;
    const std::uint64_t kWarmupTotal = kWarmupPerProducer * num_producers;

    // Engine with 1 worker (the single consumer) + enough shards
    unsigned workers = 1;
    unsigned shards = 1;

    Engine eng(EngineConfig{workers, shards, 64, 1024});
    auto aid = eng.spawn<PingActor>(42).value();

    // Get a reference for each producer thread
    detail::MessagePool pool{4096};
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<PingActor> ref = router.get<PingActor>(42);
    eng.start();

    // --- Warmup ---
    {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < num_producers; ++t) {
            threads.emplace_back([&ref, t, kWarmupPerProducer]() {
                for (std::uint64_t i = 0; i < kWarmupPerProducer; ++i) {
                    ref.tell(static_cast<int>(t * kWarmupPerProducer + i));
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // --- Timed run ---
    // Each thread records its own latencies (sampled)
    std::vector<std::vector<double>> thread_samples(num_producers);
    std::atomic<std::uint64_t> start_flag{0};
    std::atomic<bool> running{true};

    // Use a barrier for synchronized start
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < num_producers; ++t) {
        threads.emplace_back([&, t]() {
            thread_samples[t].reserve(kPerProducer / 10);  // sample ~10%

            // Spin barrier
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                auto s = pal::clock::now();
                ref.tell(static_cast<int>(t * kPerProducer + i));
                auto e = pal::clock::now();
                // Sample every 10th message to keep vector size manageable
                if ((i & 0xF) == 0) {
                    double ns = std::chrono::duration<double, std::nano>(e - s).count();
                    thread_samples[t].push_back(ns);
                }
            }
        });
    }

    // Wait for all threads to be ready, then start
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

    std::printf("=== Quark MPSC Benchmark ===\n");
    std::printf("Producers: %u | Messages: %llu total, %llu each\n",
                num_producers,
                static_cast<unsigned long long>(kTotal),
                static_cast<unsigned long long>(kPerProducer));
    std::printf("Duration:  %.3f s\n\n", secs);

    std::printf("Throughput:\n");
    std::printf("  Total:      %8.2f M msg/s\n", total_mps);
    std::printf("  Per-thread: %8.2f M msg/s\n\n", per_thread_mps);

    // Aggregate all thread latencies
    std::vector<double> all;
    std::uint64_t total_samples = 0;
    for (auto& s : thread_samples) {
        total_samples += s.size();
        all.insert(all.end(), s.begin(), s.end());
    }
    auto stats = summarize(all);
    print_stats("enqueue latency (sampled, all producers):", stats);

    // Per-thread min/max
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
