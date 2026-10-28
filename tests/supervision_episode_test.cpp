// Tests 007-Failure-and-Supervision / ADR-009 S3 — the RESTART-EPISODE MARKER on a reentrant lane.
// When a handler fault triggers Restart, `quiesce(Cancel)` seals admission and unwinds the in-flight
// siblings. A sibling that ALSO faults *during* that seal→quiescence window must be absorbed into the
// SAME episode: dead-lettered WITHOUT re-charging MaxRestarts or launching a nested restart — else
// two concurrently-faulting siblings double-charge the budget and double-restart the actor.
//
// TEETH (positive control): the SAME scenario compiled with -DQUARK_SUPERVISION_NO_EPISODE_MARKER
// drops the marker and DOUBLE-RESTARTS (restarts_total == 2 for one episode), exactly as the ADR-009
// D3-S3 control (double-restart 2946/2000) demonstrated. The delta (1 vs 2) is the proof.
//
// Deterministic single-thread drive: two async siblings suspend, then both fault on resume within
// one restart window.
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/supervision.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Work {};

struct Worker : Actor<Worker, MaxConcurrency<4>> {
    using protocol = Protocol<Work>;
    Activation* lane = nullptr;
    bool boom = false;

    task<> handle(const Work&) {
        co_await lane->async_suspend();   // the interleave point: both siblings park here
        if (boom) throw std::runtime_error("sibling faults during the seal window");
        co_return;
    }
};

void reconstruct(void* self, void*) noexcept { static_cast<Worker*>(self)->boom = false; }

}  // namespace

int main() {
    bool ok = true;

    Worker w;
    Activation act{&w, Worker::dispatch_table(), {}, max_concurrency_of<Worker>(),
                   SupervisionPolicy{SupervisionDirective::Restart}};
    w.lane = &act;
    act.set_reconstruct({&reconstruct, nullptr});

    // Two Work messages → two async frames that both suspend at co_await.
    std::vector<Descriptor> descs(2);
    std::vector<Work> jobs(2);
    for (std::size_t i = 0; i < 2; ++i) {
        descs[i].payload = &jobs[i];
        stamp<Worker, Work>(descs[i]);
        act.post(&descs[i]);
    }
    check(drive(act) == DriveEnd::Parked, "two async siblings suspended (lane parked)", ok);
    check(act.in_flight() == 2, "both siblings in flight", ok);

    // Arm the fault, then re-admit both frames on the lane; both fault within one restart window.
    w.boom = true;
    act.complete_one();  // hand frame 0 back through the 015 gate (Parked→Scheduled)
    drive(act);          // resume f0 → fault → episode; f1 is cancel-driven → also faults → absorbed

    check(act.faults() == 2, "both siblings' faults were contained (no abort)", ok);
    check(act.in_flight() == 0, "in-flight set fully drained", ok);
    check(act.seal() == SealState::Open, "seal released after the episode", ok);

#ifdef QUARK_SUPERVISION_NO_EPISODE_MARKER
    // CONTROL: without the marker, the second sibling's fault opens a SECOND restart episode.
    check(act.restarts_total() == 2,
          "CONTROL (no marker): two concurrent faults DOUBLE-restart", ok);
#else
    // With the marker, the two faults are ONE episode → exactly one restart, budget charged once.
    check(act.restarts_total() == 1,
          "marker: two concurrent faults are one episode (single restart)", ok);
#endif

    std::printf("supervision_episode_test: %s (%s)\n", ok ? "OK" : "FAIL",
#ifdef QUARK_SUPERVISION_NO_EPISODE_MARKER
                "no-marker control");
#else
                "marker");
#endif
    return ok ? 0 : 1;
}
