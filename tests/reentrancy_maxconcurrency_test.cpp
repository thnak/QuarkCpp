// Tests 015-Reentrancy-and-Quiescence §MaxConcurrency<N> — a `MaxConcurrency<N>` actor admits the
// next message while an async handler is suspended, up to N handlers in flight CONCURRENTLY, and
// never more. Deterministic single-thread drive: post many async messages, let the lane admit up to
// the cap (all suspend), assert the observed high-water mark of the in-flight set == N (not N+1),
// then complete them through the 015 gate and confirm the lane keeps the cap as slots free up.
#include <cassert>
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Job {
    int id;
};

struct Pipe : Actor<Pipe, MaxConcurrency<3>> {
    using protocol = Protocol<Job>;
    Activation* lane = nullptr;
    int started = 0;    // synchronous prologue count (lane-exclusive)
    int finished = 0;   // synchronous epilogue count (lane-exclusive)

    task<> handle(const Job&) {
        ++started;
        co_await lane->async_suspend();  // yield the frame to the in-flight set
        ++finished;
        co_return;
    }
};

}  // namespace

int main() {
    bool ok = true;
    static_assert(is_reentrant_v<Pipe>, "MaxConcurrency<3> is reentrant intent (005)");
    static_assert(max_concurrency_of<Pipe>() == 3, "cap resolved from the policy pack");

    Pipe actor;
    Activation act{&actor, Pipe::dispatch_table(), {}, max_concurrency_of<Pipe>()};
    actor.lane = &act;
    check(act.is_reentrant(), "activation carries the reentrant core", ok);

    // Post 8 async jobs; only 3 may be in flight at once.
    constexpr int kJobs = 8;
    Job jobs[kJobs];
    Descriptor descs[kJobs];
    for (int i = 0; i < kJobs; ++i) {
        jobs[i].id = i;
        descs[i].payload = &jobs[i];
        stamp<Pipe, Job>(descs[i]);
        act.post(&descs[i]);
    }

    // Drive: the lane admits until the cap is reached, then parks with 3 suspended frames.
    DriveEnd e = drive(act);
    check(e == DriveEnd::Parked, "lane parks with in-flight frames (not Idle)", ok);
    check(act.in_flight() == 3, "exactly 3 handlers in flight (the cap)", ok);
    check(act.max_in_flight() == 3, "observed max in flight == 3 (never admitted a 4th)", ok);
    check(actor.started == 3, "only 3 handlers started their synchronous region", ok);
    check(actor.finished == 0, "no handler finished yet (all suspended)", ok);

    // Complete frames one at a time through the 015 gate; each freed slot admits exactly one more,
    // so the in-flight set never exceeds the cap and never drops below it while work remains.
    int completions = 0;
    while (act.in_flight() > 0) {
        const bool woke = act.complete_one();  // carrier re-admits the oldest suspended frame
        (void)woke;
        drive(act);
        ++completions;
        check(act.in_flight() <= 3, "in-flight never exceeds the cap during draining", ok);
    }

    check(completions == kJobs, "one gate completion per job", ok);
    check(actor.started == kJobs, "every job's prologue ran", ok);
    check(actor.finished == kJobs, "every job's epilogue ran (all completed)", ok);
    check(act.max_in_flight() == 3, "high-water mark stayed exactly at the cap N=3", ok);
    check(act.state() == ExecState::Idle, "lane returned to Idle once quiescent", ok);

    std::printf("reentrancy_maxconcurrency_test: %s  (max_in_flight=%zu started=%d finished=%d)\n",
                ok ? "OK" : "FAIL", act.max_in_flight(), actor.started, actor.finished);
    return ok ? 0 : 1;
}
