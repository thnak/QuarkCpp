// Quark benchmark: message latency & throughput (mirrors caf_bench.cpp exactly)
// Measures: spawn overhead, tell latency, ask latency, bulk throughput
// Runs at 1 worker (1:1 fair) and max workers (all cores).
//
// Build: see CMakeLists.txt
// Run:   quark_bench.exe [workers]

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace quark;

namespace {

// ---- utility ------------------------------------------------------------

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

// ---- Ping actor (fire-and-forget) --------------------------------------

struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<int>;
    void handle(const int&) noexcept { /* just receive */ }
};

// ---- Echo actor (responds to requests) ---------------------------------

struct EchoActor : Actor<EchoActor, Sequential> {
    using protocol = Protocol<Ask<int, int>>;
    void handle(const Ask<int, int>& m) noexcept {
        m.respond(m.query);
    }
};

// ---- Spawn overhead ----------------------------------------------------

double bench_spawn(unsigned workers) {
    constexpr std::uint64_t kSpawns = 10'000;

    Engine eng(EngineConfig{workers, workers, 64, 1024});
    auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kSpawns; ++i) {
        auto aid = eng.spawn<PingActor>(i).value();
    }
    auto t1 = pal::clock::now();
    double per = std::chrono::duration<double, std::nano>(t1 - t0).count() / kSpawns;
    std::printf("spawn overhead (%llu spawns):\n",
                static_cast<unsigned long long>(kSpawns));
    std::printf("  %8.1f ns/spawn\n", per);
    return per;
}

// ---- Tell latency: ActorRef -> Actor, fire-and-forget -------------------

double bench_tell_latency(unsigned workers) {
    constexpr std::uint64_t kWarmup = 10'000;
    constexpr std::uint64_t kSamples = 100'000;

    detail::MessagePool pool{256};
    Engine eng(EngineConfig{workers, workers, 64, 1024});
    auto aid = eng.spawn<PingActor>(42, pool.sink()).value();
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<PingActor> ref = router.get<PingActor>(42);
    eng.start();

    std::vector<double> samples;
    samples.reserve(kSamples);

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        auto t0 = pal::clock::now();
        ref.tell(static_cast<int>(i));
        auto t1 = pal::clock::now();
        if (i >= kWarmup)
            samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    eng.stop();
    auto stats = summarize(samples);
    print_stats("tell latency (ActorRef -> Actor, fire-and-forget):", stats);
    return stats.p50;
}

// ---- Ask latency: ActorRef -> Actor, request-response ------------------

double bench_ask_latency(unsigned workers) {
    constexpr std::uint64_t kWarmup = 5'000;
    constexpr std::uint64_t kSamples = 50'000;

    detail::MessagePool ask_pool{256};
    Engine eng2(EngineConfig{workers, workers, 64, 1024});
    auto aid2 = eng2.spawn<EchoActor>(42, ask_pool.sink()).value();
    LocalRouter router2(eng2.post_courier(), ask_pool);
    ActorRef<EchoActor> ref2 = router2.get<EchoActor>(42);
    eng2.start();

    std::vector<double> samples;
    samples.reserve(kSamples);

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        auto t0 = pal::clock::now();
        auto res = block_on(ref2.ask<int>(static_cast<int>(i)));
        auto t1 = pal::clock::now();
        if (!res.has_value()) {
            std::fprintf(stderr, "ask failed at i=%llu\n",
                         static_cast<unsigned long long>(i));
            eng2.stop();
            return 0;
        }
        if (res.value() != static_cast<int>(i))
            std::fprintf(stderr, "value mismatch at i=%llu\n",
                         static_cast<unsigned long long>(i));
        if (i >= kWarmup)
            samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    eng2.stop();
    auto stats = summarize(samples);
    print_stats("ask latency (ActorRef -> Actor, request-response):", stats);
    return stats.p50;
}

// ---- Throughput: fire-and-forget, bulk ---------------------------------

double bench_throughput(unsigned workers) {
    constexpr std::uint64_t kOps = 1'000'000;

    detail::MessagePool thr_pool{256};
    Engine eng3(EngineConfig{workers, workers, 64, 1024});
    auto aid3 = eng3.spawn<PingActor>(42, thr_pool.sink()).value();
    LocalRouter router3(eng3.post_courier(), thr_pool);
    ActorRef<PingActor> ref3 = router3.get<PingActor>(42);
    eng3.start();

    auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        ref3.tell(static_cast<int>(i));
    }
    auto t1 = pal::clock::now();

    eng3.stop();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    double mps = static_cast<double>(kOps) / secs / 1e6;
    std::printf("throughput (fire-and-forget, %llu msgs):\n",
                static_cast<unsigned long long>(kOps));
    std::printf("  %8.2f M msg/s   (%.3fs)\n", mps, secs);
    return mps;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned workers = 1;
    if (argc > 1)
        workers = static_cast<unsigned>(std::atoi(argv[1]));
    if (workers == 0)
        workers = static_cast<unsigned>(std::thread::hardware_concurrency());

    unsigned max_workers = static_cast<unsigned>(std::thread::hardware_concurrency());

    std::printf("=== Quark v0.1.0 Benchmarks (Ryzen 5 4600H, Windows) ===\n");
    std::printf("Compiler: clang++ 22.1.5\n");
    std::printf("Workers:  %u (max %u)\n\n", workers, max_workers);

    // Spawn is single-threaded by nature (happens before start)
    bench_spawn(workers);
    std::printf("\n");

    bench_tell_latency(workers);
    std::printf("\n");
    bench_ask_latency(workers);
    std::printf("\n");
    bench_throughput(workers);

    return 0;
}
