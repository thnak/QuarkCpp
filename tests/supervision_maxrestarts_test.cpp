// Tests 007-Failure-and-Supervision / ADR-009 §Restart budget — MaxRestarts<N> bounds a poison
// loop: each restart within the window is charged; on exhaustion the actor ESCALATES (default node
// action: Stop) and the poison stops re-entering a fresh actor. Driven directly against one lane.
//
// A poison message that faults on EVERY dispatch would restart-loop forever without the bound; with
// MaxRestarts<3> the lane restarts exactly 3 times, then escalates once and stops — the survivors
// drain to dead-letter.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/supervision.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Boom {};

struct Poison : Actor<Poison, Sequential> {
    using protocol = Protocol<Boom>;
    void handle(const Boom&) { throw std::runtime_error("poison"); }
};

void reconstruct(void*, void*) noexcept {}  // fresh state is a no-op here (stateless poison target)

int escalate_calls = 0;
void on_escalate(void*, error, void*) noexcept { ++escalate_calls; }

}  // namespace

int main() {
    bool ok = true;
    escalate_calls = 0;

    Poison p;
    // MaxRestarts<3, no window> (window_ns = 0 ⇒ a hard lifetime cap, count never resets).
    Activation act{&p, Poison::dispatch_table(), {}, 1,
                   SupervisionPolicy{SupervisionDirective::Restart, /*max_restarts*/ 3, /*window*/ 0}};
    act.set_reconstruct({&reconstruct, nullptr});
    act.set_escalation_sink({&on_escalate, nullptr});

    // Five poison messages. Restarts 1,2,3 succeed; the 4th fault exhausts the budget → escalate →
    // Stop; the 5th message is then dead-lettered without dispatch.
    constexpr std::size_t kN = 5;
    std::vector<Descriptor> descs(kN);
    std::vector<Boom> booms(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        descs[i].payload = &booms[i];
        stamp<Poison, Boom>(descs[i]);
        act.post(&descs[i]);
    }
    drive(act);

    check(act.restarts_total() == 3, "MaxRestarts<3>: exactly 3 restarts before exhaustion", ok);
    check(act.escalations() == 1, "MaxRestarts: budget exhaustion escalated exactly once", ok);
    check(escalate_calls == 1, "MaxRestarts: the supervisor was told on exhaustion", ok);
    check(act.is_stopped(), "MaxRestarts: actor stopped after escalation", ok);
    check(act.faults() == 4, "MaxRestarts: 4 messages faulted (5th dead-lettered post-stop)", ok);
    check(act.dead_letters() == 5, "MaxRestarts: every message accounted for (4 poison + 1 shed)", ok);

    // A second scenario: a large window that never elapses behaves identically to no-window here.
    {
        escalate_calls = 0;
        Poison p2;
        Activation a2{&p2, Poison::dispatch_table(), {}, 1,
                      SupervisionPolicy{SupervisionDirective::Restart, 2,
                                        std::numeric_limits<std::int64_t>::max()}};
        a2.set_reconstruct({&reconstruct, nullptr});
        a2.set_escalation_sink({&on_escalate, nullptr});
        std::vector<Descriptor> d2(4);
        std::vector<Boom> b2(4);
        for (std::size_t i = 0; i < 4; ++i) {
            d2[i].payload = &b2[i];
            stamp<Poison, Boom>(d2[i]);
            a2.post(&d2[i]);
        }
        drive(a2);
        check(a2.restarts_total() == 2, "MaxRestarts<2, huge window>: 2 restarts then escalate", ok);
        check(a2.escalations() == 1, "MaxRestarts<2>: escalated on exhaustion", ok);
        check(a2.is_stopped(), "MaxRestarts<2>: stopped", ok);
    }

    std::printf("supervision_maxrestarts_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
