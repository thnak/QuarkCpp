// Tests 007-Failure-and-Supervision / ADR-009 — the four supervision DIRECTIVES applied at the
// handler-boundary guard, driven directly against one Activation lane (no Engine, deterministic):
//   * Resume   — keep actor state, drop the failed message, keep draining.
//   * Restart  — reconstruct actor state (a counter RESET proves fresh state); keep the mailbox.
//   * Restart (assert-intact, no reconstruct wired) — state is KEPT (zero-cost default).
//   * Stop     — deactivate; subsequent messages drain to dead-letter (handler never runs).
//   * Escalate — the escalation sink is told; the default node action Stops the actor.
// The throwing handler is CONTAINED at the boundary every time — the lane never aborts.
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

struct Bump {};
struct Boom {};

struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Bump, Boom>;
    int n = 0;
    void handle(const Bump&) noexcept { ++n; }
    void handle(const Boom&) { throw std::runtime_error("boom"); }  // faults at the boundary
};

void reset_counter(void* self, void*) noexcept { static_cast<Counter*>(self)->n = 0; }

// A tiny message source: distinct pooled descriptors + payloads posted to the lane in order.
struct Feeder {
    std::vector<Descriptor> descs;
    std::vector<Bump> bumps;
    std::vector<Boom> booms;
    explicit Feeder(std::size_t cap) : descs(cap), bumps(cap), booms(cap) {}

    void bump(Activation& act, std::size_t i) {
        descs[i].payload = &bumps[i];
        stamp<Counter, Bump>(descs[i]);
        act.post(&descs[i]);
    }
    void boom(Activation& act, std::size_t i) {
        descs[i].payload = &booms[i];
        stamp<Counter, Boom>(descs[i]);
        act.post(&descs[i]);
    }
};

int escalate_calls = 0;
void on_escalate(void*, error, void*) noexcept { ++escalate_calls; }

}  // namespace

int main() {
    bool ok = true;

    // ---- Resume: state kept across the fault, message dropped ---------------------------------
    {
        Counter c;
        Activation act{&c, Counter::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Resume}};
        Feeder f(4);
        f.bump(act, 0);
        f.bump(act, 1);
        f.boom(act, 2);
        f.bump(act, 3);
        drive(act);
        check(c.n == 3, "Resume: state kept (2 bumps + faulted Boom dropped + 1 bump == 3)", ok);
        check(act.faults() == 1, "Resume: exactly one fault contained", ok);
        check(act.restarts_total() == 0, "Resume: no restart", ok);
        check(!act.is_stopped(), "Resume: actor still live", ok);
    }

    // ---- Restart WITH reconstruct: fresh state (counter reset proves reconstruction) ----------
    {
        Counter c;
        Activation act{&c, Counter::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Restart}};
        act.set_reconstruct({&reset_counter, nullptr});
        Feeder f(4);
        f.bump(act, 0);
        f.bump(act, 1);
        f.boom(act, 2);  // fault → Restart → reconstruct resets n to 0
        f.bump(act, 3);
        drive(act);
        check(c.n == 1, "Restart+reconstruct: state RESET then one bump == 1", ok);
        check(act.restarts_total() == 1, "Restart+reconstruct: exactly one restart", ok);
        check(!act.is_stopped(), "Restart: actor re-activated (not stopped)", ok);
    }

    // ---- Restart assert-intact (no reconstruct wired): state KEPT (zero-cost default) ----------
    {
        Counter c;
        Activation act{&c, Counter::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Restart}};
        Feeder f(4);
        f.bump(act, 0);
        f.bump(act, 1);
        f.boom(act, 2);
        f.bump(act, 3);
        drive(act);
        check(c.n == 3, "Restart assert-intact: state kept (no reconstruct) == 3", ok);
        check(act.restarts_total() == 1, "Restart assert-intact: still counted a restart", ok);
    }

    // ---- Stop: actor halts; the following message drains to dead-letter (handler never runs) ---
    {
        Counter c;
        Activation act{&c, Counter::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Stop}};
        Feeder f(3);
        f.bump(act, 0);  // n → 1
        f.boom(act, 1);  // fault → Stop
        f.bump(act, 2);  // dead-lettered (actor stopped) → n unchanged
        drive(act);
        check(c.n == 1, "Stop: only the pre-fault bump ran; post-stop bump dead-lettered", ok);
        check(act.is_stopped(), "Stop: actor deactivated", ok);
        check(act.dead_letters() == 2, "Stop: poison + post-stop message both dead-lettered", ok);
    }

    // ---- Escalate: the supervisor sink is told; default node action Stops the actor -----------
    {
        escalate_calls = 0;
        Counter c;
        Activation act{&c, Counter::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Escalate}};
        act.set_escalation_sink({&on_escalate, nullptr});
        Feeder f(3);
        f.bump(act, 0);
        f.boom(act, 1);  // fault → Escalate (tell supervisor) → Stop
        f.bump(act, 2);
        drive(act);
        check(act.escalations() == 1, "Escalate: one escalation driven", ok);
        check(escalate_calls == 1, "Escalate: the supervisor sink was told (message hop seam)", ok);
        check(act.is_stopped(), "Escalate: default node action stopped the actor", ok);
        check(c.n == 1, "Escalate: post-escalation message dead-lettered", ok);
    }

    std::printf("supervision_decision_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
