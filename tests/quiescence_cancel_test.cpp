// Tests 015-Reentrancy-and-Quiescence §The quiescence primitive — quiesce(Cancel). Cancel seals
// admission, FIRES the std::stop_token on each in-flight handler's MessageContext (015 step 2), then
// awaits the in-flight set to unwind: each sibling observes its cooperative cancellation, unwinds
// (co_return), and the guard resolves only after all have left. This is the primitive 007 Restart
// builds on (quiesce(Cancel) before reconstructing suspect state); ADR-009 S2/S3: cancelled siblings
// complete before teardown, no caller hangs.
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Sibling {
    int id;
};
struct Restart {
    int tag;
};

struct Account : Actor<Account, MaxConcurrency<8>> {
    using protocol = Protocol<Sibling, Restart>;
    Activation* lane = nullptr;

    int cancelled = 0;   // siblings that observed cooperative cancellation and unwound
    int completed = 0;   // siblings that ran their effect (must stay 0 — they were cancelled)
    bool guard_obtained = false;
    int cancelled_at_guard = -1;

    task<> handle(const Sibling&, const MessageContext& ctx) {
        co_await lane->async_suspend();
        if (ctx.stop_requested()) {  // cooperative cancellation (007: operated on suspect state)
            ++cancelled;
            co_return;
        }
        ++completed;  // must NOT happen under quiesce(Cancel)
        co_return;
    }

    // The failure-quiesce handler (stands in for 007 Restart). Cancel fires sibling stop_tokens.
    task<> handle(const Restart&) {
        QuiescenceGuard g = co_await lane->quiesce(QuiesceMode::Cancel);
        guard_obtained = true;
        cancelled_at_guard = cancelled;  // all siblings unwound before state reconstruction
        // ... reconstruct suspect state here, under the guard ...
        co_return;
    }
};

}  // namespace

int main() {
    bool ok = true;
    Account actor;
    Activation act{&actor, Account::dispatch_table(), {}, max_concurrency_of<Account>()};
    actor.lane = &act;

    Sibling s0{0}, s1{1}, s2{2}, s3{3};
    Restart rst{7};
    Descriptor d0, d1, d2, d3, dr;
    d0.payload = &s0; stamp<Account, Sibling>(d0);
    d1.payload = &s1; stamp<Account, Sibling>(d1);
    d2.payload = &s2; stamp<Account, Sibling>(d2);
    d3.payload = &s3; stamp<Account, Sibling>(d3);
    dr.payload = &rst; stamp<Account, Restart>(dr);
    act.post(&d0);
    act.post(&d1);
    act.post(&d2);
    act.post(&d3);
    act.post(&dr);

    // A single drive cascades the whole cancellation: admit 4 siblings (suspend), admit the Restart
    // handler which seals Cancelling + fires all stop_tokens + drives the siblings' resume; each
    // observes stop and unwinds; the waiter then resolves to the guard. No external carrier needed —
    // Cancel drives the in-flight frames itself.
    drive(act);

    check(actor.guard_obtained, "guard resolved (in-flight fully unwound)", ok);
    check(actor.cancelled == 4, "all 4 siblings observed cooperative cancellation", ok);
    check(actor.completed == 0, "no sibling ran its effect (all were cancelled)", ok);
    check(actor.cancelled_at_guard == 4, "all siblings unwound BEFORE the guard (015 step 3)", ok);
    check(act.seal() == SealState::Open, "seal released after the guard", ok);
    check(act.in_flight() == 0, "fully quiescent", ok);
    check(act.state() == ExecState::Idle, "lane returned to Idle", ok);

    std::printf("quiescence_cancel_test: %s  (cancelled=%d completed=%d)\n", ok ? "OK" : "FAIL",
                actor.cancelled, actor.completed);
    return ok ? 0 : 1;
}
