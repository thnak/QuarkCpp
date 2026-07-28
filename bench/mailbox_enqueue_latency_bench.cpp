// Pure producer-side enqueue latency benchmark — dimension 12 of the mailbox benchmark suite.
// Distinct from mailbox_bench.cpp's occupancy-1 latency (which times a full enqueue->dequeue round
// trip on ONE thread) and from mailbox_scaling_bench.cpp's aggregate-throughput sweep: this file
// isolates the PRODUCER's own enqueue() cost alone, both in isolation (occupancy-1, no contention)
// and under real contention (P concurrent producers), as its own p50/p99/p999 distribution.
//
//  A) Occupancy-1: single thread, enqueue() timed individually, a SEPARATE cheap drain step keeps
//     the mailbox from growing unbounded but is excluded from the timed window entirely (so this is
//     purely the producer-side cost, not a round trip).
//  B) Contended: P producer threads (pre-allocated per-producer descriptors, no shared pool) each
//     time their OWN enqueue() calls individually while a background thread drains continuously;
//     reports the per-producer latency distribution under real `tail_` contention.
//
// Informational only — no [MISS]/[goal] tokens (mirrors mailbox_scaling_bench.cpp / bench/
// mailbox_pool_partition_bench.cpp's convention); always exits 0. Machine safety: contended section
// defaults to P=4 (<=4 producer threads + 1 background drainer) — never wider by default.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// ---- A) Occupancy-1 pure enqueue latency ---------------------------------------------------------
void bench_enqueue_latency_solo() {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kSamples = 2'000'000;

    std::vector<Descriptor> descs(kWarmup + kSamples);
    for (std::uint64_t i = 0; i < descs.size(); ++i) descs[i].message_id = MessageId{i};

    Mailbox mb;
    std::vector<double> ns;
    ns.reserve(kSamples);

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        const auto t0 = pal::clock::now();
        mb.enqueue(&descs[i]);
        const auto t1 = pal::clock::now();
        // Drain immediately, OUTSIDE the timed window — keeps the mailbox from growing (this bench
        // measures pure enqueue cost, not backlog-buildup, which is dim 14's job) without that
        // drain call polluting the enqueue-only sample.
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) { std::fprintf(stderr, "drain miss\n"); return; }
        if (i >= kWarmup) ns.push_back(bench::ns_between(t0, t1));
    }

    bench::Stats s = bench::summarize(ns);
    std::printf("A) occupancy-1 pure enqueue() latency (no contention, drain excluded from timing):\n");
    bench::report_latency("  enqueue-only:", s, bench::budget::local_tell_goal_ns,
                          bench::budget::local_tell_hard_ns, bench::budget::tell_p999_goal_ns,
                          bench::budget::tell_p999_hard_ns);
}

// ---- B) Contended pure enqueue latency, P producers --------------------------------------------
void bench_enqueue_latency_contended(unsigned P) {
    constexpr std::uint64_t kPerProducer = 500'000;

    // Descriptor holds std::atomic members (non-movable, non-copyable), so each inner vector must
    // be constructed directly AT ITS TARGET SIZE (the (size_type) constructor's fill-initialize
    // path, which placement-news N elements with no move/copy) rather than default-constructed then
    // resize()'d (which would require Descriptor's deleted move constructor to even compile, even
    // though it would run over an empty range at n=0 — a known libstdc++ instantiation quirk). The
    // OUTER vector<vector<Descriptor>> is fine to reallocate on emplace_back: std::vector is always
    // movable regardless of its element type (its move ctor only steals the inner pointer/size).
    std::vector<std::vector<Descriptor>> arrays;
    arrays.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        arrays.emplace_back(kPerProducer);
        for (std::uint64_t i = 0; i < kPerProducer; ++i) arrays[p][i].message_id = MessageId{i};
    }

    Mailbox mb;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> ready{0};
    std::atomic<std::uint64_t> drained{0};

    std::thread drainer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            DrainResult r = mb.try_dequeue();
            if (r.status == DrainStatus::Message) drained.fetch_add(1, std::memory_order_relaxed);
        }
        // Final sweep after producers signal completion, so nothing is left stranded.
        for (;;) {
            DrainResult r = mb.try_dequeue();
            if (r.status == DrainStatus::Message) { drained.fetch_add(1, std::memory_order_relaxed); continue; }
            if (r.status == DrainStatus::Empty) break;
        }
    });

    std::vector<std::vector<double>> per_producer_ns(P);
    std::vector<std::thread> producers;
    producers.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        per_producer_ns[p].reserve(kPerProducer);
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                const auto t0 = pal::clock::now();
                mb.enqueue(&arrays[p][i]);
                const auto t1 = pal::clock::now();
                per_producer_ns[p].push_back(bench::ns_between(t0, t1));
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < P) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();
    stop.store(true, std::memory_order_release);
    drainer.join();

    std::printf("B) contended enqueue() latency, P=%u producers (per-producer distribution):\n", P);
    // Pool ALL producers' samples for one aggregate distribution (the contention cost every
    // producer pays, not who happened to pay it) plus one representative per-producer line.
    std::vector<double> pooled;
    pooled.reserve(static_cast<std::size_t>(P) * kPerProducer);
    for (unsigned p = 0; p < P; ++p) pooled.insert(pooled.end(), per_producer_ns[p].begin(), per_producer_ns[p].end());
    bench::Stats agg = bench::summarize(pooled);
    bench::report_latency("  pooled (all producers):", agg, bench::budget::local_tell_goal_ns,
                          bench::budget::local_tell_hard_ns, bench::budget::tell_p999_goal_ns,
                          bench::budget::tell_p999_hard_ns);
    std::printf("  (total drained=%llu, expected=%llu)\n",
                static_cast<unsigned long long>(drained.load()),
                static_cast<unsigned long long>(static_cast<std::uint64_t>(P) * kPerProducer));
}

}  // namespace

int main() {
    std::printf("== Quark mailbox pure enqueue-latency bench (dim 12; pin -c 0 for A, -c 0-4 for B) ==\n");
    bench_enqueue_latency_solo();
    std::printf("\n");
    bench_enqueue_latency_contended(4);  // machine-safety default cap; never wider without pinning
    return 0;
}
