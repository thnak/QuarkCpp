// Quark sample 05 — Cooperative cancellation (015 quiesce(Cancel)).
//
// A reentrant actor can have several handler frames in flight at once (MaxConcurrency<N>). When one
// frame needs to reset the actor — e.g. a supervisor restart, or a "cancel everything" command — it
// calls `quiesce(QuiesceMode::Cancel)`. That SEALS admission, fires the `std::stop_token` on every
// in-flight sibling's MessageContext, and waits for them to unwind. Each sibling cooperatively checks
// `ctx.stop_requested()` at its next suspension point and `co_return`s WITHOUT running its effect.
// The cancelling frame's guard resolves only once every sibling has left — so state is reset on a
// quiescent lane, never mid-flight. This is the exact primitive 007 Restart is built on (ADR-009).
//
// This sample drives ONE activation lane directly with a tiny inlined run loop (the same loop the 002
// scheduler runs per lane) so the whole cascade is deterministic — no threads, no sleeps.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 05_cooperative_cancellation
// Run  :  taskset -c 0-3 build/samples/05_cooperative_cancellation
#include <cstdint>
#include <cstdio>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"

using namespace quark;

// Two messages: a long-running "Sibling" job, and a "CancelAll" that resets the actor.
struct Sibling {
    int id;
};
struct CancelAll {};

struct Job : Actor<Job, MaxConcurrency<8>> {  // reentrant: up to 8 frames in flight at once
    using protocol = Protocol<Sibling, CancelAll>;
    Activation* lane = nullptr;

    int cancelled = 0;  // siblings that observed cancellation and unwound cleanly
    int completed = 0;  // siblings that ran their effect (stays 0 — all were cancelled)
    bool did_reset = false;
    int cancelled_when_reset = -1;

    // A long-running job: it suspends (yields the lane), then — on resume — checks whether it was
    // asked to stop. Under CancelAll it observes the request and unwinds without doing its effect.
    task<> handle(const Sibling&, const MessageContext& ctx) {
        co_await lane->async_suspend();       // a real handler would await I/O, a timer, a child ask...
        if (ctx.stop_requested()) {           // cooperative cancellation point
            ++cancelled;
            co_return;                        // unwind WITHOUT running the effect
        }
        ++completed;                          // (not reached in this sample — all get cancelled)
        co_return;
    }

    // The cancelling frame: cancel every in-flight sibling, then reset state on the quiesced lane.
    task<> handle(const CancelAll&) {
        QuiescenceGuard g = co_await lane->quiesce(QuiesceMode::Cancel);
        did_reset = true;
        cancelled_when_reset = cancelled;     // proof: every sibling unwound BEFORE we got the guard
        // ... safe to reconstruct/reset actor state here, on a fully quiescent lane ...
        co_return;
    }
};

// The per-lane run loop the 002 scheduler executes — inlined here so the sample needs no engine and
// stays fully deterministic. Drives the lane until it goes Idle (nothing left) or Parks (in-flight
// async frames waiting on an external carrier — Cancel drives its own frames, so we reach Idle).
static void drive_lane(Activation& act) {
    if (!act.try_acquire()) return;
    std::uint64_t busy = 0;
    for (;;) {
        switch (act.drain_step(/*budget*/ 1u << 20)) {
            case Activation::DrainOutcome::DrainedEmpty:
                if (act.close_out()) { busy = 0; continue; }
                return;  // Idle
            case Activation::DrainOutcome::Busy:
                if (++busy > (1u << 24)) return;
                continue;
            case Activation::DrainOutcome::BudgetExhausted:
                act.yield_to_scheduled();
                if (act.try_acquire()) { busy = 0; continue; }
                return;
            case Activation::DrainOutcome::Suspended:
                return;  // Parked
        }
    }
}

int main() {
    Job actor;
    Activation act{&actor, Job::dispatch_table(), {}, max_concurrency_of<Job>()};
    actor.lane = &act;

    // Post 4 sibling jobs, then a CancelAll. All 5 land in the mailbox before we drive the lane.
    Sibling s0{0}, s1{1}, s2{2}, s3{3};
    CancelAll cancel{};
    Descriptor d0, d1, d2, d3, dc;
    d0.payload = &s0; stamp<Job, Sibling>(d0);
    d1.payload = &s1; stamp<Job, Sibling>(d1);
    d2.payload = &s2; stamp<Job, Sibling>(d2);
    d3.payload = &s3; stamp<Job, Sibling>(d3);
    dc.payload = &cancel; stamp<Job, CancelAll>(dc);
    act.post(&d0); act.post(&d1); act.post(&d2); act.post(&d3); act.post(&dc);

    // One drive cascades the whole thing: 4 siblings admit and suspend; CancelAll seals + fires their
    // stop_tokens + drives their resume; each observes the stop and unwinds; then the guard resolves.
    drive_lane(act);

    std::printf("siblings cancelled (unwound cleanly): %d  (expected 4)\n", actor.cancelled);
    std::printf("siblings that ran their effect:       %d  (expected 0 — all cancelled)\n",
                actor.completed);
    std::printf("state reset happened after reset:     %s, with %d siblings already unwound (expected 4)\n",
                actor.did_reset ? "yes" : "no", actor.cancelled_when_reset);
    std::printf("lane quiescent at end: in_flight=%d, %s\n", act.in_flight(),
                act.state() == ExecState::Idle ? "Idle" : "NOT Idle");

    const bool ok = actor.cancelled == 4 && actor.completed == 0 && actor.did_reset &&
                    actor.cancelled_when_reset == 4 && act.in_flight() == 0 &&
                    act.state() == ExecState::Idle && act.seal() == SealState::Open;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
