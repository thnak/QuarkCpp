// Tests 022 §Load shedding + 018 — deadline-aware shedding: doomed work (a message whose deadline
// has already passed) is shed FIRST and cheapest, at admission, before it consumes a cycle — dead-
// lettered with errc::timeout. An in-budget message is admitted and runs. The shed is gated "under
// overload" by a shed_threshold: below it a doomed message still runs (governance does not shed a
// lightly-loaded actor); at/above it doomed work is dropped.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

#include "pal/pal.hpp"
#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/dead_letter.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

struct Job {
    std::uint64_t id;
};
struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Job>;
    std::vector<std::uint64_t> ran;
    void handle(const Job& m) { ran.push_back(m.id); }
};

// Descriptor::deadline_ns lives in the pal::clock domain (activation.hpp's shed check compares it
// against pal::now(), NOT std::chrono::steady_clock) — using steady_clock here would silently compare
// two unrelated epochs. On Linux, CLOCK_MONOTONIC (steady_clock) and CLOCK_BOOTTIME (pal::clock)
// happen to share a near-boot origin so that mismatch is invisible; on Windows, steady_clock (QPC) and
// pal::clock (QueryUnbiasedInterruptTimePrecise) do NOT share an epoch at all, so it must be pal::now().
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch()).count();
}

void drain_all(Activation& act) {
    check(act.try_acquire(), "acquire");
    for (;;) {
        const auto out = act.drain_step(1024);
        if (out == Activation::DrainOutcome::DrainedEmpty) break;
        if (out == Activation::DrainOutcome::BudgetExhausted) continue;
        break;
    }
    (void)act.close_out();
}
}  // namespace

int main() {
    const std::int64_t now = steady_ns();
    const std::int64_t past = now - 1'000'000;       // 1 ms ago — already expired (doomed)
    const std::int64_t future = now + 60'000'000'000;  // 60 s ahead — comfortably in budget

    // ---- Under overload (shed_threshold = 0 ⇒ always active): doomed shed, in-budget admitted ----
    {
        Worker actor;
        Activation act{&actor, Worker::dispatch_table()};
        Activation::GovernanceConfig gc;
        gc.deadline_shed = true;
        gc.shed_threshold = 0;  // always shed a doomed message
        act.enable_governance(gc);
        DeadLetterRegistry dlq(64);
        act.set_dead_letter_sink(dlq.as_sink());

        // id 0: doomed (past deadline). id 1: in-budget (future). id 2: no deadline (0 ⇒ never shed).
        std::vector<Job> jobs = {{0}, {1}, {2}};
        std::vector<Descriptor> ds(3);
        const std::int64_t deadlines[3] = {past, future, 0};
        for (std::size_t i = 0; i < 3; ++i) {
            ds[i].payload = &jobs[i];
            ds[i].message_id = MessageId{jobs[i].id};
            ds[i].trace_id = jobs[i].id;
            ds[i].deadline_ns = deadlines[i];
            stamp<Worker, Job>(ds[i]);
            check(act.post_governed(&ds[i]).result == Activation::AdmitResult::Admitted,
                  "admission does not inspect the deadline (producer-cheap)");
        }

        drain_all(act);
        check(act.governance_sheds() == 1, "one doomed message shed");
        check(dlq.total() == 1, "the doomed message was dead-lettered");
        std::vector<DeadLetterRecord> recs;
        dlq.snapshot(recs);
        check(recs.size() == 1 && recs[0].err.code == errc::timeout,
              "deadline shed dead-letters with errc::timeout");
        check(recs.size() == 1 && recs[0].trace_id == 0, "the doomed (past-deadline) frame was shed");
        // The in-budget and the no-deadline frames ran.
        check(actor.ran.size() == 2, "the in-budget + no-deadline messages ran");
        check(actor.ran.size() == 2 && actor.ran[0] == 1 && actor.ran[1] == 2,
              "survivors are the in-budget messages, in FIFO order");
    }

    // ---- Below the shed threshold: a doomed message is NOT shed (not under overload) ------------
    {
        Worker actor;
        Activation act{&actor, Worker::dispatch_table()};
        Activation::GovernanceConfig gc;
        gc.deadline_shed = true;
        gc.shed_threshold = 8;  // only shed once resident depth reaches 8
        act.enable_governance(gc);
        DeadLetterRegistry dlq(64);
        act.set_dead_letter_sink(dlq.as_sink());

        Job j{99};
        Descriptor d;
        d.payload = &j;
        d.message_id = MessageId{99};
        d.deadline_ns = past;  // doomed, but the actor is not under overload
        stamp<Worker, Job>(d);
        check(act.post_governed(&d).result == Activation::AdmitResult::Admitted, "admitted");

        drain_all(act);
        check(act.governance_sheds() == 0, "below threshold: doomed work is NOT shed");
        check(dlq.total() == 0, "below threshold: nothing dead-lettered");
        check(actor.ran.size() == 1 && actor.ran[0] == 99, "below threshold: the message ran");
    }

    std::printf("governance_deadline_shed_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
