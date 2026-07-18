// Tests 005-Developer-Model §Metadata registration path (ADR-010 seam closure): `register_actor<A>`
// RESOLVES the actor's `Priority<P>` → band and `DrainBudget<N>` → budget from the policy pack and
// registers it with the engine — the caller no longer passes band/budget by hand.
//
// Observable checks:
//   * the resolved band/budget land in the engine's Schedulable (an actor with Priority<0> lands in
//     the high band; Priority<1> in the low band; DrainBudget<N> sets its budget; an un-annotated
//     actor inherits the engine default budget);
//   * an end-to-end run under PriorityBands<2> dispatches the high-band actor's work ahead of the
//     low-band actor's — proving the resolved band actually drives scheduling.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

namespace {

struct Job {
    std::uint32_t n;
};

// High-priority, small explicit budget.
struct Hi : Actor<Hi, Priority<0>, DrainBudget<32>> {
    using protocol = Protocol<Job>;
    std::atomic<std::uint64_t>* order = nullptr;
    std::atomic<std::uint64_t>* sum = nullptr;
    std::atomic<std::uint64_t>* done = nullptr;
    void handle(const Job&) noexcept {
        const std::uint64_t o = order->fetch_add(1, std::memory_order_acq_rel);
        sum->fetch_add(o, std::memory_order_relaxed);
        done->fetch_add(1, std::memory_order_release);
    }
};

// Low-priority, no explicit budget ⇒ inherits the engine default.
struct Lo : Actor<Lo, Priority<1>> {
    using protocol = Protocol<Job>;
    std::atomic<std::uint64_t>* order = nullptr;
    std::atomic<std::uint64_t>* sum = nullptr;
    std::atomic<std::uint64_t>* done = nullptr;
    void handle(const Job&) noexcept {
        const std::uint64_t o = order->fetch_add(1, std::memory_order_acq_rel);
        sum->fetch_add(o, std::memory_order_relaxed);
        done->fetch_add(1, std::memory_order_release);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    std::atomic<std::uint64_t> order{0}, sum_hi{0}, sum_lo{0}, done{0};

    constexpr std::uint32_t kEngineDefaultBudget = 256;
    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1,
                                              /*default budget*/ kEngineDefaultBudget, 64});

    Hi hi;
    Lo lo;
    hi.order = &order; hi.sum = &sum_hi; hi.done = &done;
    lo.order = &order; lo.sum = &sum_lo; lo.done = &done;

    auto act_hi = std::make_unique<Activation>(&hi, Hi::dispatch_table());
    auto act_lo = std::make_unique<Activation>(&lo, Lo::dispatch_table());

    // The 005 registration path — NO explicit band/budget; resolved from the policy pack.
    Schedulable* s_hi = register_actor<Hi>(eng, /*key*/ 1, *act_hi);
    Schedulable* s_lo = register_actor<Lo>(eng, /*key*/ 2, *act_lo);

    // --- Band/budget resolved into the engine's Schedulable (directly observable). ---------------
    check(s_hi->band == 0, "Priority<0> resolved to the high band (0)", ok);
    check(s_lo->band == 1, "Priority<1> resolved to the low band (1)", ok);
    check(s_hi->budget == 32, "DrainBudget<32> resolved to budget 32", ok);
    check(s_lo->budget == kEngineDefaultBudget, "no DrainBudget ⇒ engine default budget", ok);

    // --- End-to-end: the resolved band actually drives scheduling. -------------------------------
    constexpr std::uint64_t kPer = 2000;
    const std::uint64_t total = 2 * kPer;
    std::vector<Job> msgs(total);
    std::vector<Descriptor> descs(total);
    for (std::uint64_t i = 0; i < total; ++i) {
        descs[i].payload = &msgs[i];
        stamp<Hi, Job>(descs[i]);  // Hi and Lo share the same dense slot for Job (slot 0)
    }
    // Interleave the pre-load so both bands are non-empty from the first select turn.
    std::uint64_t di = 0;
    for (std::uint64_t r = 0; r < kPer; ++r) {
        eng.post(s_hi, &descs[di++]);
        eng.post(s_lo, &descs[di++]);
    }

    eng.start();
    while (done.load(std::memory_order_acquire) < total) { /* spin until drained */ }
    eng.stop();

    const double mean_hi = static_cast<double>(sum_hi.load()) / static_cast<double>(kPer);
    const double mean_lo = static_cast<double>(sum_lo.load()) / static_cast<double>(kPer);
    check(mean_hi < mean_lo, "high-band actor dispatched earlier than low-band (resolved band works)", ok);
    check(done.load() == total, "every message dispatched (anti-starvation)", ok);

    std::printf("policy_register_test: %s  (mean_hi=%.0f mean_lo=%.0f)\n", ok ? "OK" : "FAIL",
                mean_hi, mean_lo);
    return ok ? 0 : 1;
}
