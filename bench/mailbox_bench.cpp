// Hot-path microbenchmark for the 003 Mailbox vs the 023 budgets.
//
//  - Latency: single-thread enqueue->dequeue round-trip, p50/p99/p999 ns
//    (023 goal <= 100 ns / hard <= 250 ns; p999 goal <= 5 us / hard <= 50 us).
//  - Throughput: sustained enqueue->dequeue, msg/s/core, SINGLE consumer
//    (023 sustained goal >= 10 M/s / floor >= 4 M; peak tight-loop goal >= 50 M / floor >= 20 M).
//
// Pin it: `taskset -c 0 build/bench/mailbox_bench` (latency) — never saturate the machine.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bench/bench_harness.hpp"   // 023 shared stats + budget table (percentiles + variance)
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// Latency: time each enqueue->dequeue round-trip individually (occupancy-1 sequential path).
void bench_latency() {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kSamples = 2'000'000;

    DescriptorPool pool(64);
    Mailbox mb;
    Descriptor* d = pool.acquire();

    std::vector<double> ns;
    ns.reserve(kSamples);
    std::uint64_t checksum = 0;

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const auto t0 = pal::clock::now();
        d->message_id = MessageId{i};
        mb.enqueue(d);
        DrainResult r = mb.try_dequeue();
        const auto t1 = pal::clock::now();
        if (r.status != DrainStatus::Message) { std::fprintf(stderr, "bench drain miss\n"); return; }
        checksum += r.desc->message_id.value;
        pool.release(r.desc);
        d = pool.acquire();
        if (d == nullptr) { std::fprintf(stderr, "bench pool exhausted\n"); return; }
        if (i >= kWarmup) {
            ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
    }

    bench::Stats s = bench::summarize(ns);
    bench::report_latency("latency (enqueue->dequeue, occ-1 sequential):", s,
                          bench::budget::local_tell_goal_ns, bench::budget::local_tell_hard_ns,
                          bench::budget::tell_p999_goal_ns, bench::budget::tell_p999_hard_ns);
    std::printf("  (checksum=%llu)\n", static_cast<unsigned long long>(checksum));
}

// Throughput: tight enqueue->dequeue loop (single consumer). Reports peak msg/s/core.
void bench_throughput() {
    constexpr std::uint64_t kOps = 50'000'000;

    DescriptorPool pool(64);
    Mailbox mb;
    Descriptor* d = pool.acquire();
    if (d == nullptr) { std::fprintf(stderr, "bench pool exhausted\n"); return; }
    std::uint64_t checksum = 0;

    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        mb.enqueue(d);
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) { std::fprintf(stderr, "bench drain miss\n"); return; }
        checksum += reinterpret_cast<std::uintptr_t>(r.desc) & 1U;
        d = r.desc;  // reuse the same descriptor (0 alloc)
    }
    const auto t1 = pal::clock::now();
    pool.release(d);

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(kOps) / secs / 1e6;
    bench::report_throughput("throughput (tight enqueue->dequeue, single consumer, PEAK):", mps,
                             bench::budget::tell_peak_goal_mps, bench::budget::tell_peak_floor_mps, secs);
    std::printf("  (checksum=%llu)\n", static_cast<unsigned long long>(checksum));
}

}  // namespace

int main() {
    std::printf("== Quark 003 Mailbox bench (pin with taskset -c 0) ==\n");
    bench_throughput();
    bench_latency();
    return 0;
}
