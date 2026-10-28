// Tests 002-Scheduler §Priority (ADR-010, K-band run-queue). Two priority bands, one worker lane,
// small budget (⇒ many select turns). High-band and low-band actors are pre-loaded, then drained.
// Asserts:
//   * PRIORITY: high-band messages are, on average, dispatched much earlier than low-band ones
//     (strict top-band-first select) — high-band dispatch latency beats low-band;
//   * BOUNDED PROGRESS (anti-starvation): with RotatingReserve<M> every low-band message is still
//     dispatched, and the FIRST low-band service happens within the (d+1)·K·M select-turn bound
//     (here d=1,K=2,M=4 ⇒ ≤ 16 turns — early), NOT starved to the end of the high-band drain.
//   * TEETH: with M ~ effectively-infinite the reserve never fires, so the low band is starved until
//     the high band fully empties — the first low service jumps to ~the whole high depth.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

constexpr std::uint32_t kHiActors = 2;
constexpr std::uint32_t kLoActors = 2;
constexpr std::uint64_t kPerHi = 2'000;
constexpr std::uint64_t kPerLo = 500;

struct Job {
    std::uint32_t dummy;
};

struct Band : Actor<Band, Sequential> {
    using protocol = Protocol<Job>;
    bool low = false;
    std::atomic<std::uint64_t>* order = nullptr;
    std::atomic<std::uint64_t>* sum_hi = nullptr;
    std::atomic<std::uint64_t>* sum_lo = nullptr;
    std::atomic<std::uint64_t>* cnt_lo = nullptr;
    std::atomic<std::uint64_t>* first_lo = nullptr;
    std::atomic<std::uint64_t>* done = nullptr;

    void handle(const Job&) noexcept {
        const std::uint64_t ord = order->fetch_add(1, std::memory_order_acq_rel);
        if (low) {
            sum_lo->fetch_add(ord, std::memory_order_relaxed);
            cnt_lo->fetch_add(1, std::memory_order_relaxed);
            std::uint64_t prev = first_lo->load(std::memory_order_relaxed);
            while (ord < prev && !first_lo->compare_exchange_weak(prev, ord, std::memory_order_relaxed)) {
            }
        } else {
            sum_hi->fetch_add(ord, std::memory_order_relaxed);
        }
        done->fetch_add(1, std::memory_order_release);
    }
};

struct Result {
    double mean_hi = 0, mean_lo = 0;
    std::uint64_t lo_dispatched = 0, first_lo = 0;
};

template <class Policy>
Result run_scenario() {
    std::atomic<std::uint64_t> order{0}, sum_hi{0}, sum_lo{0}, cnt_lo{0}, done{0};
    std::atomic<std::uint64_t> first_lo{~std::uint64_t{0}};
    const std::uint64_t hi_total = kPerHi * kHiActors;
    const std::uint64_t lo_total = kPerLo * kLoActors;
    const std::uint64_t total = hi_total + lo_total;

    std::vector<Band> his(kHiActors), los(kLoActors);
    auto wire = [&](Band& b, bool low) {
        b.low = low;
        b.order = &order;
        b.sum_hi = &sum_hi;
        b.sum_lo = &sum_lo;
        b.cnt_lo = &cnt_lo;
        b.first_lo = &first_lo;
        b.done = &done;
    };
    for (auto& b : his) wire(b, false);
    for (auto& b : los) wire(b, true);

    Engine<Policy> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 4, 64});
    std::vector<std::unique_ptr<Activation>> acts;
    std::vector<Schedulable*> hs(kHiActors), ls(kLoActors);
    for (std::uint32_t i = 0; i < kHiActors; ++i) {
        acts.push_back(std::make_unique<Activation>(&his[i], Band::dispatch_table()));
        hs[i] = eng.register_activation(ActorId{TypeKey{1}, i}, *acts.back(), /*band=*/0);
    }
    for (std::uint32_t i = 0; i < kLoActors; ++i) {
        acts.push_back(std::make_unique<Activation>(&los[i], Band::dispatch_table()));
        ls[i] = eng.register_activation(ActorId{TypeKey{2}, i}, *acts.back(), /*band=*/1);
    }

    std::vector<Job> msgs(total);
    std::vector<Descriptor> descs(total);
    for (std::uint64_t i = 0; i < total; ++i) {
        descs[i].payload = &msgs[i];
        stamp<Band, Job>(descs[i]);
    }

    // Interleave the pre-load so BOTH bands are non-empty from the first select turn.
    std::uint64_t di = 0;
    for (std::uint64_t r = 0; r < kPerHi || r < kPerLo; ++r) {
        if (r < kPerHi)
            for (std::uint32_t i = 0; i < kHiActors; ++i) eng.post(hs[i], &descs[di++]);
        if (r < kPerLo)
            for (std::uint32_t i = 0; i < kLoActors; ++i) eng.post(ls[i], &descs[di++]);
    }

    eng.start();
    constexpr std::uint64_t kStall = 20'000'000'000ULL;
    std::uint64_t spins = 0;
    while (done.load(std::memory_order_acquire) < total) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: done=%" PRIu64 "/%" PRIu64 "\n", done.load(), total);
            break;
        }
    }
    eng.stop();

    Result res;
    res.lo_dispatched = cnt_lo.load();
    res.first_lo = first_lo.load();
    res.mean_hi = static_cast<double>(sum_hi.load()) / static_cast<double>(hi_total);
    res.mean_lo = res.lo_dispatched ? static_cast<double>(sum_lo.load()) /
                                          static_cast<double>(res.lo_dispatched)
                                    : 0.0;
    return res;
}

}  // namespace

int main() {
    bool ok = true;
    const std::uint64_t hi_total = kPerHi * kHiActors;
    const std::uint64_t lo_total = kPerLo * kLoActors;

    // Fair: RotatingReserve<4> — low band bounded within (d+1)·K·M = 16 select turns.
    const Result fair = run_scenario<PriorityBands<2, RotatingReserve<4>>>();
    // Teeth: reserve effectively never fires ⇒ low band starves behind the entire high drain.
    const Result strict = run_scenario<PriorityBands<2, RotatingReserve<1'000'000'000>>>();

    const bool priority = fair.mean_hi < fair.mean_lo;                 // high dispatched earlier
    const bool progress = fair.lo_dispatched == lo_total;             // no low message lost
    const bool bounded = fair.first_lo < 500;                          // low served early (≪ hi depth)
    const bool starved_control = strict.first_lo >= hi_total - 1;      // control starves low to the end
    const bool decisive = fair.first_lo * 10 < strict.first_lo;        // reserve makes a decisive diff

    if (!priority) { std::fprintf(stderr, "  CHECK FAILED: high-band not dispatched earlier than low\n"); ok = false; }
    if (!progress) { std::fprintf(stderr, "  CHECK FAILED: low band did not fully drain (starved)\n"); ok = false; }
    if (!bounded)  { std::fprintf(stderr, "  CHECK FAILED: first low service not within the reserve bound\n"); ok = false; }
    if (!starved_control) { std::fprintf(stderr, "  CHECK FAILED: control did not starve low as expected\n"); ok = false; }
    if (!decisive) { std::fprintf(stderr, "  CHECK FAILED: RotatingReserve did not decisively change first-low\n"); ok = false; }

    std::printf("sched_priority_test: %s  (fair: mean_hi=%.0f mean_lo=%.0f first_lo=%" PRIu64
                " lo=%" PRIu64 "/%" PRIu64 "; control first_lo=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", fair.mean_hi, fair.mean_lo, fair.first_lo, fair.lo_dispatched,
                lo_total, strict.first_lo);
    return ok ? 0 : 1;
}
