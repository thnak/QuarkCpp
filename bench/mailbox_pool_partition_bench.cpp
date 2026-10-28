// Sweeps MessagePool's num_partitions (message_pool.hpp) for GitHub issue #4 (Quark vs CAF): a
// single shared free-list serializes concurrent producers. Implements the measurement corrections of
// ADR-063 (send-pool partitions default to auto; supersedes the 6-8x headline of ADR-060).
//
// Corrected finding: the earlier 6-8x gain was measured with an UNDER-SIZED pool (cap 4096). When the
// producers outrun the consumer the backlog exceeds 4096 cells, the free list grows by heap allocation
// WHILE HOLDING the partition mutex, and at 1 partition that serialises every producer behind malloc.
// Pre-sized, the throughput gain of partitioning is only about 1.0-2.5x on the Linux Xeon of ADR-063 (and
// can be absent on other platforms, e.g. a 4-core Windows box); the tell() p99 latency gain is
// still large (4-35x on Linux). This bench therefore runs every config in two modes:
//   undersized : cap 4096 (the old default-sized behaviour; cold growth is visible as heap_allocs)
//   presized   : cap = producers * per_producer + 8192 (no cold growth; isolates lock contention)
// and reports, per config: median M msg/s, tell() p50/p99 in ns (every 128th tell timed with
// steady_clock inside the producer loop, samples merged and sorted), and heap allocations during the
// timed send window (a counting global operator new replacement).
//
// Informational only — no established floor/ceiling exists yet for this axis, so this prints plain
// numbers with NO [goal]/[hard]/[MISS] tokens. It is auto-discovered by bench/CMakeLists and therefore
// runs under `bench-gate` (ci_bench_gate.sh), which treats a non-zero exit as a HARD failure — this
// bench always exits 0 regardless of the numbers it measures.
//
// Run: mailbox_pool_partition_bench [producers=3] [workers=1]   (sweeps num_partitions 1..producers)
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

// ---- Counting heap allocator (global replacement) ---------------------------------------------
// Only the unaligned overloads are replaced; the aligned overloads keep the standard-library pairing,
// so nothing here touches std::aligned_alloc and the Windows path is unaffected.
namespace {
std::atomic<std::uint64_t> g_bench_heap_allocs{0};

void* counted_alloc(std::size_t n) {
    g_bench_heap_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace quark;

namespace {

constexpr std::uint64_t kSampleEvery = 128;  // time 1 in 128 tell() calls
constexpr int kReps = 3;                     // runs per config; median throughput / allocs

struct PingActor : Actor<PingActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.bench.mailbox_pool_partition_bench.PingActor";
    using protocol = Protocol<int>;
    void handle(const int&) noexcept {}
};

struct RunStats {
    double mps = 0.0;
    std::uint64_t allocs = 0;
};

// One run: `producers` threads each `tell()` `per_producer` messages to one actor, pool with `capacity`
// cells partitioned `num_partitions` ways. tell() latency samples (ns) are appended to `latency_ns`.
// Throughput is send-side wall time from release to join, matching quark_mpsc_bench.cpp's method.
// Correctly wires pool.sink() as the reclaim sink (an unwired ReclaimSink cold-allocates forever
// instead of reusing the free-list) so this bench isolates ONLY the partitioning/sizing effect.
RunStats run_once(unsigned producers, unsigned workers, std::size_t num_partitions, std::size_t capacity,
                  std::uint64_t per_producer, std::vector<std::uint64_t>& latency_ns) {
    Engine eng(EngineConfig{workers, workers, 64, 1024});
    detail::MessagePool pool(capacity, num_partitions);
    auto spawned = eng.spawn<PingActor>(1, pool.sink());
    if (!spawned) {
        std::fprintf(stderr, "mailbox_pool_partition_bench: spawn failed\n");
        return {};
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

    // Per-producer sample buffers are reserved up front so sampling itself never allocates.
    std::vector<std::vector<std::uint64_t>> per_thread(producers);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ths;
    ths.reserve(producers);
    for (unsigned t = 0; t < producers; ++t) {
        per_thread[t].reserve(per_producer / kSampleEvery + 1);
        ths.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto& samples = per_thread[t];
            for (std::uint64_t i = 0; i < per_producer; ++i) {
                const int msg = static_cast<int>(i);
                if (i % kSampleEvery == 0) {
                    const auto a = std::chrono::steady_clock::now();
                    ref.tell(msg);
                    const auto b = std::chrono::steady_clock::now();
                    samples.push_back(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
                } else {
                    ref.tell(msg);
                }
            }
        });
    }
    // Thread creation happens before the window opens, so it is excluded from both time and allocs.
    while (ready.load(std::memory_order_acquire) < producers) std::this_thread::yield();
    const std::uint64_t allocs_before = g_bench_heap_allocs.load(std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ths) th.join();
    const auto t1 = std::chrono::steady_clock::now();
    const std::uint64_t allocs_after = g_bench_heap_allocs.load(std::memory_order_relaxed);

    eng.stop();

    for (auto& v : per_thread) latency_ns.insert(latency_ns.end(), v.begin(), v.end());

    RunStats out;
    out.allocs = allocs_after - allocs_before;
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double total = static_cast<double>(per_producer) * static_cast<double>(producers);
    out.mps = secs > 0.0 ? total / secs / 1e6 : 0.0;
    return out;
}

// Percentile over a sorted, non-empty sample set (nearest-rank).
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    auto idx = static_cast<std::size_t>(pct / 100.0 * static_cast<double>(sorted.size()));
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

template <typename T>
T median(std::vector<T> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? T{} : v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    unsigned producers = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 3;
    unsigned workers = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1;
    if (producers == 0) producers = 1;
    if (workers == 0) workers = 1;
    constexpr std::uint64_t kPerProducer = 100'000;
    const std::size_t presized_cap = static_cast<std::size_t>(producers) * kPerProducer + 8192;

    struct Mode {
        const char* name;
        bool presized;
    };
    constexpr Mode kModes[] = {{"undersized", false}, {"presized", true}};

    std::printf("=== mailbox_pool_partition_bench (informational — no ci-gate budget tokens) ===\n");
    std::printf("producers=%u workers=%u per_producer=%llu reps=%d sample_every=%llu\n", producers, workers,
                static_cast<unsigned long long>(kPerProducer), kReps,
                static_cast<unsigned long long>(kSampleEvery));
    std::printf("undersized: pool cap 4096 cells   presized: cap = producers*per_producer + 8192 = %zu\n",
                presized_cap);
    std::printf("M msg/s = median of %d runs; tell p50/p99 over merged samples; heap_allocs = median count\n"
                "during the timed send window.\n\n",
                kReps);

    for (std::size_t parts = 1; parts <= producers; ++parts) {
        for (const Mode& mode : kModes) {
            const std::size_t capacity = mode.presized ? presized_cap : 4096;
            std::vector<double> mps;
            std::vector<std::uint64_t> allocs;
            std::vector<std::uint64_t> latency;
            for (int r = 0; r < kReps; ++r) {
                const RunStats s = run_once(producers, workers, parts, capacity, kPerProducer, latency);
                mps.push_back(s.mps);
                allocs.push_back(s.allocs);
            }
            std::sort(latency.begin(), latency.end());
            std::printf("  num_partitions=%-3zu %-10s : %7.2f M msg/s  tell p50=%7llu ns  p99=%9llu ns  "
                        "heap_allocs=%llu\n",
                        parts, mode.name, median(mps),
                        static_cast<unsigned long long>(percentile(latency, 50.0)),
                        static_cast<unsigned long long>(percentile(latency, 99.0)),
                        static_cast<unsigned long long>(median(allocs)));
        }
    }

    std::printf(
        "\n[info] The undersized pool (cap 4096) inflates the num_partitions=1 baseline: the free list grows by "
        "heap allocation under the partition mutex (see heap_allocs). Pre-sized, the throughput gain from "
        "partitioning is much smaller on Linux (about 1.0-2.5x throughput, tell() p99 down 4-35x; ADR-063). "
        "Results are platform-dependent: a Windows run showed no throughput gain pre-sized, only a lower p99. "
        "Compare the presized rows' p99 and heap_allocs, not just M msg/s.\n");
    return 0;
}
