// CAF benchmark: message latency & throughput comparison vs Quark
// Uses built-in int types to avoid custom type registration complexity.
//
// Runs at max-threads=1 (1:1 fair) or auto (all cores).
//
// Build: see CMakeLists.txt
// Run:   caf_bench.exe [workers]

#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/anon_mail.hpp"
#include "caf/blocking_actor.hpp"
#include "caf/caf_main.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/scoped_actor.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace caf;
using namespace std::chrono;
using namespace std::literals;

// ---- utility ------------------------------------------------------------

struct Stats {
    double p50, p99, p999, mean, min, max;
    size_t n;
};

static Stats summarize(std::vector<double>& ns) {
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

static void print_stats(const char* label, const Stats& s) {
    std::printf("%s  (n=%zu)\n", label, s.n);
    std::printf("  p50  = %8.1f ns   p99  = %8.1f ns   p999 = %8.1f ns\n",
                s.p50, s.p99, s.p999);
    std::printf("  mean = %8.1f ns   min  = %8.1f ns   max  = %8.1f ns\n",
                s.mean, s.min, s.max);
}

// ---- Ping actor (event-based, fire-and-forget) -------------------------

static behavior ping_actor(event_based_actor* self) {
    return {
        [=](int) {
            // just receive, do nothing
        },
    };
}

// ---- Echo actor (event-based, responds to requests) --------------------

static behavior echo_actor(event_based_actor* self) {
    return {
        [=](int v) -> int {
            return v;
        },
    };
}

// ---- Tell latency: scoped_actor --> remote actor, fire-and-forget --------

static double bench_tell_latency(actor_system& sys) {
    constexpr uint64_t kWarmup = 10'000;
    constexpr uint64_t kSamples = 100'000;

    auto pinger = sys.spawn(ping_actor);
    scoped_actor self{sys};

    std::vector<double> samples;
    samples.reserve(kSamples);

    for (uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        auto t0 = steady_clock::now();
        anon_mail(static_cast<int>(i)).send(pinger);
        auto t1 = steady_clock::now();
        if (i >= kWarmup)
            samples.push_back(duration<double, std::nano>(t1 - t0).count());
    }

    self->mail(0).send(pinger);  // ensure delivery before destroy
    std::this_thread::sleep_for(10ms);
    auto stats = summarize(samples);
    print_stats("tell latency (scoped_actor -> event_actor, fire-and-forget):", stats);
    return stats.p50;
}

// ---- Ask latency: scoped_actor --> event_actor, request-response ---------

static double bench_ask_latency(actor_system& sys) {
    constexpr uint64_t kWarmup = 5'000;
    constexpr uint64_t kSamples = 50'000;

    auto echoer = sys.spawn(echo_actor);
    scoped_actor self{sys};

    std::vector<double> samples;
    samples.reserve(kSamples);

    for (uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        auto t0 = steady_clock::now();
        self->mail(static_cast<int>(i))
            .request(echoer, 10s)
            .receive(
                [&](int v) {
                    if (v != static_cast<int>(i))
                        std::fprintf(stderr, "value mismatch at i=%llu\n",
                                     static_cast<unsigned long long>(i));
                },
                [&](const error&) {
                    std::fprintf(stderr, "error at i=%llu\n",
                                 static_cast<unsigned long long>(i));
                });
        auto t1 = steady_clock::now();
        if (i >= kWarmup)
            samples.push_back(duration<double, std::nano>(t1 - t0).count());
    }

    auto stats = summarize(samples);
    print_stats("ask latency (scoped_actor -> event_actor, request-response):", stats);
    return stats.p50;
}

// ---- Throughput: fire-and-forget, bulk ---------------------------------

static double bench_throughput(actor_system& sys) {
    constexpr uint64_t kOps = 1'000'000;
    auto pinger = sys.spawn(ping_actor);
    scoped_actor self{sys};

    auto t0 = steady_clock::now();
    for (uint64_t i = 0; i < kOps; ++i) {
        anon_mail(static_cast<int>(i)).send(pinger);
    }
    self->mail(0).send(pinger);  // flush
    std::this_thread::sleep_for(50ms);
    auto t1 = steady_clock::now();

    double secs = duration<double>(t1 - t0).count();
    double mps = static_cast<double>(kOps) / secs / 1e6;
    std::printf("throughput (fire-and-forget, %llu msgs):\n",
                static_cast<unsigned long long>(kOps));
    std::printf("  %8.2f M msg/s   (%.3fs)\n", mps, secs);
    return mps;
}

// ---- Spawn overhead ----------------------------------------------------

static double bench_spawn(actor_system& sys) {
    constexpr uint64_t kSpawns = 10'000;
    auto t0 = steady_clock::now();
    for (uint64_t i = 0; i < kSpawns; ++i) {
        auto h = sys.spawn(ping_actor);
    }
    auto t1 = steady_clock::now();
    double per = duration<double, std::nano>(t1 - t0).count() / kSpawns;
    std::printf("spawn overhead (%llu spawns):\n",
                static_cast<unsigned long long>(kSpawns));
    std::printf("  %8.1f ns/spawn\n", per);
    return per;
}

// ---- Main ----------------------------------------------------------------

void caf_main(actor_system& sys) {
    // Detect thread count from config
    size_t threads = get_or(sys.config(), "caf.scheduler.max-threads", size_t{1});

    unsigned max_workers = static_cast<unsigned>(std::thread::hardware_concurrency());

    std::printf("=== CAF v1.1.0 Benchmarks (Ryzen 5 4600H, Windows) ===\n");
    std::printf("Compiler: clang++ 22.1.5\n");
    std::printf("Workers:  %zu (max %u)\n\n", threads, max_workers);

    bench_spawn(sys);
    std::printf("\n");
    bench_tell_latency(sys);
    std::printf("\n");
    bench_ask_latency(sys);
    std::printf("\n");
    bench_throughput(sys);
}

int main(int argc, char** argv) {
    // Parse worker count from CLI
    size_t workers = 1;
    if (argc > 1) {
        int w = std::atoi(argv[1]);
        if (w > 0)
            workers = static_cast<size_t>(w);
        else
            workers = std::thread::hardware_concurrency();
    }

    actor_system_config cfg;
    cfg.set("caf.scheduler.max-threads", workers);
    cfg.set("caf.scheduler.policy", "stealing");

    // Init host system for CAF_MAIN-less mode
    caf::core::init_global_meta_objects();

    actor_system sys{cfg};

    // Detect thread count from config
    size_t actual_threads = get_or(sys.config(), "caf.scheduler.max-threads", workers);

    unsigned max_workers = static_cast<unsigned>(std::thread::hardware_concurrency());

    std::printf("=== CAF v1.1.0 Benchmarks (Ryzen 5 4600H, Windows) ===\n");
    std::printf("Compiler: clang++ 22.1.5\n");
    std::printf("Workers:  %zu (max %u)\n\n", actual_threads, max_workers);

    bench_spawn(sys);
    std::printf("\n");
    bench_tell_latency(sys);
    std::printf("\n");
    bench_ask_latency(sys);
    std::printf("\n");
    bench_throughput(sys);

    return 0;
}
