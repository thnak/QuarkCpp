// Tests 015-Reentrancy-and-Quiescence §The quiescence primitive — quiesce(Drain). The primitive
// (1) SEALS admission (no new handler is admitted while draining), (2) AWAITS the in-flight set to
// empty, and (3) resolves to a QuiescenceGuard only AFTER every in-flight sibling has completed —
// then (4) guard release re-opens the gate and queued messages resume in FIFO order. This is the
// primitive 012 snapshot builds on (quiesce(Drain)); ADR-009 S2: every sibling completes (its reply
// cell resolved) BEFORE the guard grants exclusive state access.
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
struct Snapshot {
    int tag;
};
struct Newcomer {
    int tag;
};

struct Store : Actor<Store, MaxConcurrency<8>> {
    using protocol = Protocol<Sibling, Snapshot, Newcomer>;
    Activation* lane = nullptr;

    int siblings_finished = 0;
    bool guard_obtained = false;
    int siblings_at_guard = -1;   // in-flight siblings completed when the guard resolved
    int newcomer_ran = 0;         // must stay 0 until the seal is released

    // In-flight sibling: suspends, completed later by a carrier through the 015 gate.
    task<> handle(const Sibling&) {
        co_await lane->async_suspend();
        ++siblings_finished;
        co_return;
    }

    // The quiescing handler (stands in for a 012 snapshot). Seals, awaits siblings, takes the guard.
    task<> handle(const Snapshot&) {
        QuiescenceGuard g = co_await lane->quiesce(QuiesceMode::Drain);
        guard_obtained = true;
        siblings_at_guard = siblings_finished;  // must be "all siblings done" (015 step 3)
        // ... consistent point-in-time state work would happen here, under the guard ...
        co_return;  // g destroyed ⇒ seal released ⇒ admission re-opens
    }

    void handle(const Newcomer&) noexcept { ++newcomer_ran; }
};

}  // namespace

int main() {
    bool ok = true;
    Store actor;
    Activation act{&actor, Store::dispatch_table(), {}, max_concurrency_of<Store>()};
    actor.lane = &act;

    // FIFO: three in-flight siblings, then the quiescer, then a newcomer that must be sealed out.
    Sibling s0{0}, s1{1}, s2{2};
    Snapshot snap{9};
    Newcomer nc{0};
    Descriptor ds0, ds1, ds2, dsnap, dnc;
    ds0.payload = &s0; stamp<Store, Sibling>(ds0);
    ds1.payload = &s1; stamp<Store, Sibling>(ds1);
    ds2.payload = &s2; stamp<Store, Sibling>(ds2);
    dsnap.payload = &snap; stamp<Store, Snapshot>(dsnap);
    dnc.payload = &nc; stamp<Store, Newcomer>(dnc);
    act.post(&ds0);
    act.post(&ds1);
    act.post(&ds2);
    act.post(&dsnap);
    act.post(&dnc);

    // Drive: admit 3 siblings (suspend), admit the quiescer (seals Draining, suspends as the waiter),
    // then the newcomer is refused admission. The lane parks with 4 frames in flight.
    DriveEnd e = drive(act);
    check(e == DriveEnd::Parked, "lane parked while draining", ok);
    check(act.seal() == SealState::Draining, "admission is SEALED (Draining)", ok);
    check(act.in_flight() == 4, "3 siblings + the quiescing waiter are in flight", ok);
    check(actor.newcomer_ran == 0, "newcomer NOT admitted while sealed", ok);
    check(!actor.guard_obtained, "guard NOT resolved before siblings drained", ok);

    // Complete the siblings one at a time. The guard must not resolve until the LAST one is done.
    for (int i = 0; i < 3; ++i) {
        check(!actor.guard_obtained, "guard still pending while siblings remain", ok);
        check(actor.newcomer_ran == 0, "newcomer still sealed out mid-drain", ok);
        act.complete_one();
        drive(act);
    }

    check(actor.guard_obtained, "guard resolved after in-flight drained", ok);
    check(actor.siblings_at_guard == 3, "ALL siblings completed before the guard (in-flight==0)", ok);
    check(actor.newcomer_ran == 1, "newcomer admitted only AFTER the seal was released (FIFO)", ok);
    check(act.seal() == SealState::Open, "seal re-opened on guard release", ok);
    check(act.in_flight() == 0, "fully quiescent", ok);
    check(act.state() == ExecState::Idle, "lane returned to Idle", ok);

    std::printf("quiescence_test: %s  (siblings_at_guard=%d newcomer_ran=%d)\n", ok ? "OK" : "FAIL",
                actor.siblings_at_guard, actor.newcomer_ran);
    return ok ? 0 : 1;
}
