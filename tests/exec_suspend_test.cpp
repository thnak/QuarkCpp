// Tests 001-Actor-Execution-Model §Hybrid handler execution — the split-executor / no-advance-past-
// suspended rule (ADR-002 caught a design whose loop advanced past a suspended handler and broke
// single-executor). An async handler that suspends FREEZES the drain: the next message is NOT
// dispatched, the actor is Parked (sealed — every admission CAS fails), and only when the 015 seam
// completes (simulated in-test) does the drain resume and dispatch the next message in FIFO order.
#include <coroutine>
#include <cstdio>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {
struct Slow {
    int tag;
};
struct Fast {
    int n;
};

struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Slow, Fast>;

    int slow_ran = 0;   // incremented AFTER the co_await resumes
    int fast_ran = 0;
    int order_log = 0;  // 1 if Fast ran while slow_ran==0 (an illegal advance-past-suspended)

    // Async: suspends at the first co_await. slow_ran must stay 0 until 015 re-admits.
    task<> handle(const Slow&) {
        co_await std::suspend_always{};
        ++slow_ran;
        co_return;
    }
    // Sync: must NOT run until the parked Slow completes.
    void handle(const Fast& f) noexcept {
        if (slow_ran == 0) order_log = 1;  // would mean we advanced past a suspended handler
        fast_ran += f.n;
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
    Worker actor;
    Activation act{&actor, Worker::dispatch_table()};

    Slow slow{1};
    Fast fast{9};
    Descriptor d_slow;
    d_slow.payload = &slow;
    stamp<Worker, Slow>(d_slow);
    Descriptor d_fast;
    d_fast.payload = &fast;
    stamp<Worker, Fast>(d_fast);

    // Enqueue Slow THEN Fast (mailbox FIFO). Acquire and drain.
    act.post(&d_slow);
    act.post(&d_fast);
    check(act.try_acquire(), "acquire Scheduled->Running", ok);

    const auto out = act.drain_step(64);
    check(out == Activation::DrainOutcome::Suspended, "drain suspends on the async Slow handler", ok);
    check(act.state() == ExecState::Parked, "activation is Parked (sealed) while suspended", ok);
    check(act.is_parked(), "parked frame is held", ok);
    check(actor.slow_ran == 0, "Slow handler suspended BEFORE completing", ok);
    check(actor.fast_ran == 0, "Fast NOT dispatched while Slow is suspended (drain frozen)", ok);

    // Parked seals every admission CAS: a worker cannot claim the actor.
    check(!act.try_acquire(), "Parked fails try_acquire (single-executor seal)", ok);
    check(act.state() == ExecState::Parked, "still Parked after a failed admission", ok);

    // Simulate the 015 completion of the awaited work: resume the frame, reclaim, re-admit.
    const bool wake = act.complete_parked();
    check(actor.slow_ran == 1, "Slow handler completed on 015 re-admit", ok);
    check(!act.is_parked(), "parked frame cleared after completion", ok);
    check(act.state() == ExecState::Scheduled, "re-admitted Parked->Scheduled", ok);
    check(wake, "re-admit signalled a wake", ok);

    // Now the drain resumes and dispatches Fast — in FIFO order, exactly once.
    check(act.try_acquire(), "re-acquire Scheduled->Running after re-admit", ok);
    const auto out2 = act.drain_step(64);
    check(out2 == Activation::DrainOutcome::DrainedEmpty, "drained empty after Fast", ok);
    check(!act.close_out(), "close-out to Idle", ok);
    check(actor.fast_ran == 9, "Fast dispatched AFTER Slow completed", ok);
    check(actor.order_log == 0, "Fast never observed a mid-suspension state (no illegal advance)", ok);

    std::printf("exec_suspend_test: %s  (slow_ran=%d fast_ran=%d order_log=%d)\n", ok ? "OK" : "FAIL",
                actor.slow_ran, actor.fast_ran, actor.order_log);
    return ok ? 0 : 1;
}
