// Tests 002-Scheduler §Blocking/fiber adapter completion (ADR-015 minimal re-admit). An async handler
// suspends under the REAL scheduler: the worker lane parks the activation (Parked, sealed) and moves
// on. A carrier thread (here, main — standing in for the 015 admission gate) drives
// Engine::complete_parked(), which resumes the frame, reclaims, and re-admits Parked→Scheduled, then
// re-enqueues the activation and wakes exactly one lane on the distinct-StoreLoad-pair wake edge. The
// lane then drains the remaining FIFO message. Asserts the parked handler completes, the following
// message is dispatched AFTER it (FIFO, exactly once), and nothing is stranded.
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Slow {
    int tag;
};
struct Fast {
    int n;
};

struct Handler : Actor<Handler, Sequential> {
    using protocol = Protocol<Slow, Fast>;
    std::atomic<int> slow_ran{0};
    std::atomic<int> fast_ran{0};
    std::atomic<int> order_log{0};  // set if Fast runs before Slow completed (illegal advance)

    task<> handle(const Slow&) {
        co_await std::suspend_always{};
        slow_ran.fetch_add(1, std::memory_order_release);
        co_return;
    }
    void handle(const Fast& f) noexcept {
        if (slow_ran.load(std::memory_order_acquire) == 0)
            order_log.store(1, std::memory_order_release);
        fast_ran.fetch_add(f.n, std::memory_order_release);
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
    Handler actor;

    Engine<> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    auto act = std::make_unique<Activation>(&actor, Handler::dispatch_table());
    Schedulable* s = eng.register_activation(ActorId{TypeKey{1}, 0}, *act);

    Slow slow{1};
    Fast fast{9};
    Descriptor d_slow, d_fast;
    d_slow.payload = &slow;
    stamp<Handler, Slow>(d_slow);
    d_fast.payload = &fast;
    stamp<Handler, Fast>(d_fast);

    eng.start();
    eng.post(s, &d_slow);  // async — will suspend and park the activation
    eng.post(s, &d_fast);  // must NOT run until the parked Slow completes

    // Wait for the lane to park the activation on the suspended async handler.
    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (s->activation->state() != ExecState::Parked) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: activation never parked\n");
            eng.stop();
            return 1;
        }
    }
    check(actor.slow_ran.load() == 0, "Slow suspended before completing", ok);
    check(actor.fast_ran.load() == 0, "Fast NOT dispatched while Slow is parked (drain frozen)", ok);

    // Carrier completion: resume + reclaim + re-admit (Parked→Scheduled) + re-enqueue + wake.
    const bool wake = eng.complete_parked(s);
    check(wake, "re-admit signalled a wake (Parked→Scheduled edge)", ok);

    // The lane now drains Fast in FIFO order.
    spins = 0;
    while (actor.fast_ran.load(std::memory_order_acquire) == 0) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: Fast never dispatched after re-admit\n");
            eng.stop();
            return 1;
        }
    }
    eng.stop();

    check(actor.slow_ran.load() == 1, "Slow completed exactly once on re-admit", ok);
    check(actor.fast_ran.load() == 9, "Fast dispatched after Slow completed", ok);
    check(actor.order_log.load() == 0, "Fast never observed a mid-suspension state (no illegal advance)", ok);

    std::printf("sched_readmit_test: %s  (slow_ran=%d fast_ran=%d order_log=%d)\n", ok ? "OK" : "FAIL",
                actor.slow_ran.load(), actor.fast_ran.load(), actor.order_log.load());
    return ok ? 0 : 1;
}
