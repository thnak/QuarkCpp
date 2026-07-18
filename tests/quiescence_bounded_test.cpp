// Tests 015-Reentrancy-and-Quiescence §Bounded quiescence — a stuck/slow in-flight handler must not
// stall quiesce(Drain) forever. quiesce(Drain, deadline) arms a watchdog; on expiry it ESCALATES to
// Cancel (fires the in-flight stop_tokens), so quiescence always terminates. Here a sibling's carrier
// NEVER fires (the handler is stuck at its co_await); Drain would hang indefinitely, but the watchdog
// escalation resumes+unwinds it within the bound and the guard resolves. The bound is the
// caller-provided deadline (a second-deadline escalation to the node supervisor, 007, is a seam).
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Stuck {
    int id;
};
struct Drain {
    int tag;
};

struct Service : Actor<Service, MaxConcurrency<8>> {
    using protocol = Protocol<Stuck, Drain>;
    Activation* lane = nullptr;

    int unwound = 0;      // stuck handlers that unwound via escalated cancellation
    int completed = 0;    // must stay 0 — the carrier never fires, only the watchdog frees them
    bool guard_obtained = false;

    task<> handle(const Stuck&, const MessageContext& ctx) {
        co_await lane->async_suspend();  // stuck here: NO carrier will ever complete this
        if (ctx.stop_requested()) {
            ++unwound;
            co_return;
        }
        ++completed;
        co_return;
    }

    task<> handle(const Drain&) {
        // Drain with a deadline (the bound). Escalates to Cancel on watchdog expiry.
        QuiescenceGuard g = co_await lane->quiesce(QuiesceMode::Drain, /*deadline_ns=*/1000);
        guard_obtained = true;
        co_return;
    }
};

}  // namespace

int main() {
    bool ok = true;
    Service actor;
    Activation act{&actor, Service::dispatch_table(), {}, max_concurrency_of<Service>()};
    actor.lane = &act;

    Stuck s0{0}, s1{1};
    Drain dr{5};
    Descriptor d0, d1, dd;
    d0.payload = &s0; stamp<Service, Stuck>(d0);
    d1.payload = &s1; stamp<Service, Stuck>(d1);
    dd.payload = &dr; stamp<Service, Drain>(dd);
    act.post(&d0);
    act.post(&d1);
    act.post(&dd);

    // Drive: two stuck siblings suspend, the quiescer seals Draining and parks as the waiter. Because
    // the siblings' carriers never fire, the in-flight set does NOT drain — the lane stays parked.
    DriveEnd e = drive(act);
    check(e == DriveEnd::Parked, "lane parked, draining", ok);
    check(act.seal() == SealState::Draining, "sealed Draining, awaiting stuck siblings", ok);
    check(act.in_flight() == 3, "2 stuck siblings + the quiescing waiter in flight", ok);
    check(!actor.guard_obtained, "guard cannot resolve — siblings are stuck", ok);

    // A pre-deadline poll must NOT escalate (the bound has not elapsed).
    check(!act.poll_quiesce_watchdog(/*now_ns=*/500), "no escalation before the deadline", ok);
    check(act.seal() == SealState::Draining, "still Draining before the bound", ok);
    check(!actor.guard_obtained, "still no guard before the bound", ok);

    // Watchdog expiry: signal Drain→Cancel and wake the parked lane. The escalation is DEFERRED and
    // applied ON-LANE (audit Finding 1): the off-lane watchdog never mutates seal/live itself — it
    // raises kEscalateCancel and re-admits (Parked→Scheduled), and the lane performs the
    // seal→Cancelling + stop_token fire on its next drive. This is what makes the watchdog race-free.
    const bool escalated = act.poll_quiesce_watchdog(/*now_ns=*/2000);
    check(escalated, "watchdog fired at the bound (signaled Drain→Cancel + woke the parked lane)", ok);
    check(act.seal() == SealState::Draining, "escalation deferred — still Draining until the lane runs", ok);
    drive(act);  // lane applies the escalation: seal→Cancelling, fire stop_tokens, unwind, resolve guard

    check(actor.unwound == 2, "both stuck siblings unwound via escalated cancellation", ok);
    check(actor.completed == 0, "no sibling completed normally (carrier never fired)", ok);
    check(actor.guard_obtained, "guard resolved after the escalation drained the in-flight set", ok);
    check(act.in_flight() == 0, "fully quiescent after bounded quiescence", ok);
    check(act.state() == ExecState::Idle, "lane returned to Idle", ok);

    std::printf("quiescence_bounded_test: %s  (unwound=%d completed=%d escalated=%d)\n",
                ok ? "OK" : "FAIL", actor.unwound, actor.completed, static_cast<int>(escalated));
    return ok ? 0 : 1;
}
