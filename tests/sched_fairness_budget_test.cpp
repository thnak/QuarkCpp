// Tests 002-Scheduler §Fairness — the drain budget bounds how long one hot actor holds a lane. A
// hot actor is loaded with a huge mailbox; several competitors get one message each, enqueued AFTER
// it. With one worker lane and a small budget the hot actor must yield (Running→Scheduled) after its
// budget and re-enqueue BEHIND the competitors, so the competitors are all dispatched early — long
// before the hot actor finishes draining.
//
// TEETH (in-process control): the same scenario with an enormous budget (≥ the whole hot mailbox)
// lets the hot actor monopolize the lane, so the competitors are only dispatched at the very end.
// The test asserts the fair run services competitors an order of magnitude earlier than the control.
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

constexpr std::uint64_t kHot = 20'000;   // hot actor mailbox depth
constexpr std::uint32_t kComp = 8;        // competitor actors, one message each

struct Job {
    std::uint32_t dummy;
};

struct Task : Actor<Task, Sequential> {
    using protocol = Protocol<Job>;
    bool competitor = false;
    std::atomic<std::uint64_t>* order = nullptr;       // global dispatch order counter
    std::atomic<std::uint64_t>* last_comp = nullptr;   // max dispatch order among competitors
    std::atomic<std::uint64_t>* done = nullptr;         // total dispatched

    void handle(const Job&) noexcept {
        const std::uint64_t ord = order->fetch_add(1, std::memory_order_acq_rel);
        if (competitor) {
            std::uint64_t prev = last_comp->load(std::memory_order_relaxed);
            while (ord > prev && !last_comp->compare_exchange_weak(prev, ord, std::memory_order_relaxed)) {
            }
        }
        done->fetch_add(1, std::memory_order_release);
    }
};

// Returns the dispatch-order index at which the LAST competitor message was serviced.
std::uint64_t run_scenario(std::uint32_t budget) {
    std::atomic<std::uint64_t> order{0}, last_comp{0}, done{0};
    const std::uint64_t total = kHot + kComp;

    Task hot;
    hot.order = &order;
    hot.last_comp = &last_comp;
    hot.done = &done;
    std::vector<Task> comps(kComp);
    for (auto& c : comps) {
        c.competitor = true;
        c.order = &order;
        c.last_comp = &last_comp;
        c.done = &done;
    }

    Engine<> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, budget, 64});
    std::vector<std::unique_ptr<Activation>> acts;
    acts.push_back(std::make_unique<Activation>(&hot, Task::dispatch_table()));
    Schedulable* hs = eng.register_activation(ActorId{TypeKey{1}, 0}, *acts.back());
    std::vector<Schedulable*> cs(kComp);
    for (std::uint32_t i = 0; i < kComp; ++i) {
        acts.push_back(std::make_unique<Activation>(&comps[i], Task::dispatch_table()));
        cs[i] = eng.register_activation(ActorId{TypeKey{2}, i}, *acts.back());
    }

    std::vector<Job> msgs(total);
    std::vector<Descriptor> descs(total);
    for (std::uint64_t i = 0; i < total; ++i) {
        descs[i].payload = &msgs[i];
        stamp<Task, Job>(descs[i]);
    }

    // Enqueue the hot mailbox FIRST (so the hot activation is scheduled ahead), then the competitors.
    for (std::uint64_t i = 0; i < kHot; ++i) eng.post(hs, &descs[i]);
    for (std::uint32_t i = 0; i < kComp; ++i) eng.post(cs[i], &descs[kHot + i]);

    eng.start();
    constexpr std::uint64_t kStall = 20'000'000'000ULL;
    std::uint64_t spins = 0;
    while (done.load(std::memory_order_acquire) < total) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL in run_scenario(budget=%u): done=%" PRIu64 "/%" PRIu64 "\n",
                         budget, done.load(), total);
            break;
        }
    }
    eng.stop();
    return last_comp.load();
}

}  // namespace

int main() {
    bool ok = true;

    const std::uint64_t fair = run_scenario(/*budget*/ 16);           // hot actor must yield
    const std::uint64_t monopoly = run_scenario(/*budget*/ kHot + 1);  // hot actor monopolizes (control)

    // Fair: every competitor serviced within a few budget-bursts (well under the hot depth).
    const bool fair_progress = fair < kHot / 4;
    // Control: hot actor drains almost entirely before competitors → last competitor near the end.
    const bool monopoly_late = monopoly >= kHot - 1;
    // The budget must make a decisive difference.
    const bool decisive = fair * 8 < monopoly;

    if (!fair_progress) {
        std::fprintf(stderr, "  CHECK FAILED: competitors did not make early progress under budget\n");
        ok = false;
    }
    if (!monopoly_late) {
        std::fprintf(stderr, "  CHECK FAILED: control (huge budget) did not monopolize as expected\n");
        ok = false;
    }
    if (!decisive) {
        std::fprintf(stderr, "  CHECK FAILED: budget did not decisively change fairness\n");
        ok = false;
    }

    std::printf("sched_fairness_budget_test: %s  (fair last-competitor order=%" PRIu64
                ", monopoly control order=%" PRIu64 ", hot depth=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", fair, monopoly, kHot);
    return ok ? 0 : 1;
}
