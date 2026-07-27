// MPSC (multi-producer, single-consumer) benchmark: multiple threads fire messages
// at a single actor's mailbox. Mirrors quark_mpsc_bench.cpp exactly.
//
// Both benches: N producer threads, 1 actor, total M messages.
// Metrics: total throughput (M msg/s), per-thread throughput, p50/p99/p999 enqueue latency.
//
// Build: see CMakeLists.txt
// Run:   caf_mpsc_bench.exe [num_producers]

#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/anon_mail.hpp"
#include "caf/caf_main.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/scoped_actor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace caf;
using namespace std::chrono;

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

// Shared atomic counter for message tracking
std::atomic<uint64_t> g_count{0};

// ---- Ping actor (fire-and-forget) ---------------------------------------

behavior ping_actor(event_based_actor* self) {
    return {
        [=](int) {
            g_count.fetch_add(1, std::memory_order_relaxed);
        },
    };
}

}  // namespace

void caf_main(actor_system& sys) {
    unsigned num_producers = 4;
    // We get the arg from environment or default
    // (caf_main interface doesn't easily pass argv, so use the config)

    // For now, read from the system config
    auto& cfg = sys.config();
    auto workers = get_or(cfg, "caf.scheduler.max-threads", size_t{1});

    std::printf("=== CAF MPSC Benchmark ===\n");
    std::printf("Producers: %u | Scheduler threads: %zu\n\n", num_producers, workers);

    // The actual run happens in main()
}

// Manual main so we can pass CLI args
int main(int argc, char** argv) {
    unsigned num_producers = 4;
    if (argc > 1)
        num_producers = static_cast<unsigned>(std::atoi(argv[1]));
    if (num_producers == 0)
        num_producers = std::thread::hardware_concurrency();
    if (num_producers > 64)
        num_producers = 64;

    constexpr uint64_t kPerProducer = 500'000;
    constexpr uint64_t kWarmupPerProducer = 10'000;
    const uint64_t kTotal = kPerProducer * num_producers;

    actor_system_config cfg;
    cfg.set("caf.scheduler.max-threads", size_t{1});
    cfg.set("caf.scheduler.policy", "stealing");

    caf::core::init_global_meta_objects();
    actor_system sys{cfg};

    // Spawn ping actor
    auto pinger = sys.spawn(ping_actor);

    // --- Warmup ---
    {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < num_producers; ++t) {
            threads.emplace_back([&pinger, t]() {
                for (uint64_t i = 0; i < kWarmupPerProducer; ++i) {
                    anon_mail(static_cast<int>(t * kWarmupPerProducer + i)).send(pinger);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    g_count.store(0, std::memory_order_relaxed);

    // --- Timed run ---
    std::vector<std::vector<double>> thread_samples(num_producers);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    auto t0 = steady_clock::now();

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < num_producers; ++t) {
        threads.emplace_back([&, t]() {
            thread_samples[t].reserve(kPerProducer / 10);

            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (uint64_t i = 0; i < kPerProducer; ++i) {
                auto s = steady_clock::now();
                anon_mail(static_cast<int>(t * kPerProducer + i)).send(pinger);
                auto e = steady_clock::now();
                if ((i & 0xF) == 0) {
                    double ns = duration<double, std::nano>(e - s).count();
                    thread_samples[t].push_back(ns);
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < num_producers) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto& th : threads) th.join();

    auto t1 = steady_clock::now();

    double secs = duration<double>(t1 - t0).count();
    double total_mps = static_cast<double>(kTotal) / secs / 1e6;
    double per_thread_mps = static_cast<double>(kPerProducer) / secs / 1e6;

    std::printf("=== CAF MPSC Benchmark ===\n");
    std::printf("Producers: %u | Messages: %llu total, %llu each\n",
                num_producers,
                static_cast<unsigned long long>(kTotal),
                static_cast<unsigned long long>(kPerProducer));
    std::printf("Scheduler: %zu thread(s)\n\n", size_t{1});
    std::printf("Duration:  %.3f s\n\n", secs);

    std::printf("Throughput:\n");
    std::printf("  Total:      %8.2f M msg/s\n", total_mps);
    std::printf("  Per-thread: %8.2f M msg/s\n\n", per_thread_mps);

    std::vector<double> all;
    for (auto& s : thread_samples) {
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

    std::printf("Contention factor: %.2f (throughput vs CAF 1-thread tell)\n",
                total_mps / 5.14);

    return 0;
}
