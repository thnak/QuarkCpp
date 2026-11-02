// Cross-thread 1 producer -> 1 consumer mailbox steady-state floor (issue #32 baseline).
//
// Every other mailbox bench in this directory measures ONE thread (003 enqueue->dequeue, or the 002
// lifecycle on a single core). Issue #32 / ADR-064 are about the two-thread shape: one producer
// enqueues while one consumer drains, with a bounded in-flight window so neither side idles behind an
// unbounded backlog and descriptor cells are genuinely recycled. That floor (~158 ns/msg at ADR-064)
// had no checked-in bench, so a mailbox redesign (e.g. batched tell) could not be A/B'd reproducibly.
//
// This bench measures that shape and prints ns/msg + M msg/s. It deliberately prints NO
// [goal]/[hard]/[MISS] token: 023's throughput budgets are the single-thread peak/tight-loop budgets,
// and this cross-thread number is well below them by construction. It is an A/B baseline, not a
// budget-compliance claim.
//
// Machine safety (bench/README.md): two threads only. Pin them to two cores, never the whole box:
//   taskset -c 0-1 build/bench/mailbox_1p1c_bench [total] [window] [reps]
// (ci_bench_gate.sh pins to -c 0-3, still inside the repo rule, leaving the two threads room to
// spread.)
//
// Shape: a ring of `window` descriptors. The producer waits while `in_flight == window`, then enqueues
// descs[i % window]; the consumer recycles a descriptor as soon as it dequeues it (release/acquire on
// `consumed`). Each rep is timed from the shared `go` to the consumer's last dequeue, so the number is
// the whole producer+consumer pipeline, not a producer-side micro-latency.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// One steady-state rep. Returns elapsed seconds; fills `got` with the consumer's count.
double run_once(std::size_t window, std::uint64_t total, std::uint64_t& got) {
    Mailbox mb;
    std::vector<Descriptor> descs(window);
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> handled{0};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    std::thread producer([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        for (std::uint64_t i = 0; i < total; ++i) {
            while (i - consumed.load(std::memory_order_acquire) >= window) {
            }
            mb.enqueue(&descs[i % window]);
        }
    });
    std::thread consumer([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t n = 0;
        while (n < total) {
            const DrainResult r = mb.try_dequeue();
            if (r.status == DrainStatus::Message) {
                ++n;
                // Publish the dequeue so the producer may recycle that descriptor (acquire/release
                // pair with the producer's in-flight check). A descriptor is reused only after this
                // store, i.e. after the dequeue that took it out of the chain.
                consumed.store(n, std::memory_order_release);
            }
        }
        handled.store(n, std::memory_order_release);
    });

    while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    const auto t0 = pal::clock::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto t1 = pal::clock::now();
    got = handled.load(std::memory_order_relaxed);
    return bench::ns_between(t0, t1) / 1e9;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t total = 2'000'000;
    std::size_t window = 1024;
    unsigned reps = 5;
    if (argc > 1) {
        const unsigned long long v = std::strtoull(argv[1], nullptr, 10);
        if (v > 0) total = v;
    }
    if (argc > 2) {
        const unsigned long long v = std::strtoull(argv[2], nullptr, 10);
        if (v > 0) window = static_cast<std::size_t>(v);
    }
    if (argc > 3) {
        const int v = std::atoi(argv[3]);
        if (v > 0 && v <= 100) reps = static_cast<unsigned>(v);
    }
    if (window > total) window = static_cast<std::size_t>(total);

    std::printf("== Quark cross-thread 1P->1C mailbox floor (issue #32 baseline) ==\n");
    std::printf("window=%zu  total=%llu  reps=%u   (pin: taskset -c 0-1)\n", window,
                static_cast<unsigned long long>(total), reps);
    std::printf("informational: no 023 verdict — 023's budgets are the single-thread peak/tight loop;\n");
    std::printf("this is the cross-thread floor a mailbox redesign is A/B'd against.\n\n");

    // One discarded warmup rep: primes caches and the branch predictors (steady state only).
    std::uint64_t got = 0;
    (void)run_once(window, total, got);
    if (got != total) {
        std::fprintf(stderr, "warmup lost messages: got %llu / %llu\n",
                     static_cast<unsigned long long>(got), static_cast<unsigned long long>(total));
        return 1;
    }

    std::vector<double> ns_per_msg;
    ns_per_msg.reserve(reps);
    for (unsigned r = 0; r < reps; ++r) {
        const double secs = run_once(window, total, got);
        if (got != total) {
            std::fprintf(stderr, "lost messages: got %llu / %llu\n",
                         static_cast<unsigned long long>(got), static_cast<unsigned long long>(total));
            return 1;
        }
        const double ns = secs * 1e9 / static_cast<double>(total);
        ns_per_msg.push_back(ns);
        std::printf("  rep %-2u : %8.2f ns/msg   %6.2f M msg/s\n", r + 1, ns, 1000.0 / ns);
    }

    std::vector<double> sorted = ns_per_msg;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t mid = sorted.size() / 2;
    const double median =
        (sorted.size() % 2 != 0) ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2.0;
    const double spread = (sorted.back() - sorted.front()) / median * 100.0;
    std::printf("\nmedian : %8.2f ns/msg   %6.2f M msg/s   (min %.2f / max %.2f ns, spread %.1f%%)\n",
                median, 1000.0 / median, sorted.front(), sorted.back(), spread);
    return 0;
}
