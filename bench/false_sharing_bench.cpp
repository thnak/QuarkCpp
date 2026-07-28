// False-sharing benchmark — dimension 18 of the mailbox benchmark suite.
//
// The STATIC proof already exists and is stronger than any timing-based one (per the commissioning
// brief's preference, mirroring tests/stream_drain_rmw_check.sh's objdump-proof style): mailbox.hpp
// has a compile-time layout guard,
//
//     static_assert(offsetof(Mailbox, stub_) - offsetof(Mailbox, head_) >= quark::cache_line_size,
//                   "stub must sit on its own cache line, off consumer-private head_ (ADR-004)");
//
// (include/quark/core/mailbox.hpp, `Mailbox::assert_layout()`). Because that assertion lives in a
// non-template member function body, its containing class is compiled whenever ANY translation
// unit includes mailbox.hpp — which every test and bench in this suite does — so the guard is
// re-proven on every single build of this repository, not just here. `tail_` and `stub_` are each
// `QUARK_CACHE_ALIGNED` (mailbox.hpp), so both sit on their own 64 B line, separated from the
// consumer-private `head_`. That static proof is cited, not reproduced, by this file.
//
// What THIS file adds is the empirical complement the brief also asks for: a controlled A/B pair
// quantifying the COST of false sharing in the abstract (a deliberately co-located-on-one-cache-
// line control vs a properly padded layout), so the mailbox's design rationale ("stub co-located
// with head_ taxes the dequeue hot path, ADR-004, perf-c2c confirmed") has a concrete number behind
// it, not just the structural guarantee.
//
// Informational only — no [MISS]/[goal] tokens; always exits 0. Pin `-c 0-1` (2 threads).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>

#include "quark/core/config.hpp"
#include "quark/core/mailbox.hpp"

using namespace quark;

namespace {

// Control A: two hammered atomics DELIBERATELY on the same cache line (false-shared).
struct FalseShared {
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};  // same 64 B line as `a` unless padded — no padding here
};
static_assert(offsetof(FalseShared, b) < quark::cache_line_size,
              "control requires a and b to share a cache line");

// Control B: the same two atomics, each on its OWN cache line (mirrors mailbox.hpp's real layout).
struct Padded {
    QUARK_CACHE_ALIGNED std::atomic<std::uint64_t> a{0};
    QUARK_CACHE_ALIGNED std::atomic<std::uint64_t> b{0};
};
static_assert(offsetof(Padded, b) - offsetof(Padded, a) >= quark::cache_line_size,
              "control requires a and b on separate cache lines (mirrors Mailbox's tail_/stub_)");

template <class Layout>
double run(std::uint64_t iters_per_thread) {
    Layout l;
    std::atomic<bool> go{false};
    auto worker = [&](std::atomic<std::uint64_t>& mine) {
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        for (std::uint64_t i = 0; i < iters_per_thread; ++i)
            mine.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread t1(worker, std::ref(l.a));
    std::thread t2(worker, std::ref(l.b));
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    t1.join();
    t2.join();
    const auto t1_end = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1_end - t0).count();
    const double total_ops = static_cast<double>(iters_per_thread) * 2;
    return secs > 0.0 ? total_ops / secs / 1e6 : 0.0;  // M ops/s aggregate
}

}  // namespace

int main() {
    std::printf("== Quark false-sharing bench (dim 18; pin -c 0-1) ==\n\n");
    std::printf("[static proof, cited not reproduced] include/quark/core/mailbox.hpp's\n"
                "Mailbox::assert_layout(): static_assert(offsetof(stub_) - offsetof(head_) >=\n"
                "cache_line_size) — compiled (and re-proven) by every TU in this repo that includes\n"
                "mailbox.hpp. tail_/stub_ are each QUARK_CACHE_ALIGNED, so both already sit on their\n"
                "own 64 B line, isolated from the consumer-private head_ (ADR-004, perf-c2c confirmed).\n\n");

    constexpr std::uint64_t kIters = 100'000'000;
    double false_shared_mops = run<FalseShared>(kIters);
    double padded_mops = run<Padded>(kIters);

    std::printf("[empirical A/B: cost of the pathology the layout guard prevents]\n");
    std::printf("  false-shared (2 atomics, 1 cache line, 2 threads hammering)  : %8.2f M ops/s\n",
                false_shared_mops);
    std::printf("  padded       (2 atomics, 2 cache lines, same workload)      : %8.2f M ops/s\n",
                padded_mops);
    std::printf("  speedup from separating the lines: %.2fx\n",
                false_shared_mops > 0 ? padded_mops / false_shared_mops : 0.0);
    std::printf("  [info] this quantifies the abstract false-sharing cost the mailbox's real layout\n"
                "  (tail_ / stub_ each on their own line) is already structurally immune to, per the\n"
                "  static_assert cited above — not a live defect in the shipped Mailbox.\n");
    return 0;
}
