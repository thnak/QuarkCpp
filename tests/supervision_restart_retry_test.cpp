// Tests 007 §"ask reply on Restart" (ADR-009 residual risk #6) — the
// `OnRestartAsk<Retry<N,IdempotencyKey>>` knob, driven directly against one Activation lane (no
// Engine, mirrors supervision_decision_test.cpp's style):
//   * a faulting message that SUCCEEDS within the retry budget is resolved normally (no dead-letter),
//     and each attempt charges the SAME MaxRestarts budget do_restart() uses;
//   * a faulting message that EXHAUSTS the retry budget falls back to ordinary Fail semantics
//     (dead-lettered exactly once, actor left on fresh post-restart state);
//   * a retry budget that OUTRUNS the actor's own MaxRestarts window escalates instead of retrying
//     forever — retries cannot out-run the actor's own restart budget.
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/supervision.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Flaky {};
struct RetryKey {};

// Global attempt counter (not an actor member: each restart placement-news a fresh instance, so a
// cross-restart counter must live outside it — mirrors how real idempotency keys work).
int g_attempts = 0;
int g_succeed_on_attempt = 0;  // 0 == never succeeds

struct RetryActor : Actor<RetryActor, Sequential, OnFailure<Restart>,
                          OnRestartAsk<Retry<3, IdempotencyKey<RetryKey>>>> {
    using protocol = Protocol<Flaky>;
    int n = 0;
    void handle(const Flaky&) {
        ++g_attempts;
        if (g_attempts < g_succeed_on_attempt) throw std::runtime_error("flaky");
        ++n;
    }
};
void reset_retry_actor(void* self, void*) noexcept { static_cast<RetryActor*>(self)->n = 0; }

// MaxRestarts<1> caps this actor's restart budget BELOW its declared Retry<3> — proves retries are
// bounded by the actor's OWN restart budget, not just the Retry<N> count.
struct BudgetedRetryActor : Actor<BudgetedRetryActor, Sequential, OnFailure<Restart, MaxRestarts<1>>,
                                  OnRestartAsk<Retry<3, IdempotencyKey<RetryKey>>>> {
    using protocol = Protocol<Flaky>;
    void handle(const Flaky&) {
        ++g_attempts;
        throw std::runtime_error("always flaky");
    }
};
void reset_budgeted_actor(void*, void*) noexcept {}

int escalate_calls = 0;
void on_escalate(void*, error, void*) noexcept { ++escalate_calls; }

struct Feeder {
    std::vector<Descriptor> descs;
    std::vector<Flaky> flakies;
    explicit Feeder(std::size_t cap) : descs(cap), flakies(cap) {}
    template <class A>
    void flaky(Activation& act, std::size_t i) {
        descs[i].payload = &flakies[i];
        stamp<A, Flaky>(descs[i]);
        act.post(&descs[i]);
    }
};

void capture_dead_letter(Descriptor*, error e, void* ctx) noexcept {
    *static_cast<error*>(ctx) = e;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Retry succeeds within budget: message resolved normally, never dead-lettered -----
    {
        g_attempts = 0;
        g_succeed_on_attempt = 2;  // fails once (attempt 1), succeeds on the retry (attempt 2)
        RetryActor a;
        Activation act{&a, RetryActor::dispatch_table(), {}, 1, supervision_of<RetryActor>()};
        act.set_reconstruct({&reset_retry_actor, nullptr});
        Feeder f(1);
        f.flaky<RetryActor>(act, 0);
        drive(act);

        check(a.n == 1, "Retry success: the retried attempt completed the handler (n==1)", ok);
        check(act.restarts_total() == 1, "Retry success: exactly one restart charged (one failed attempt)", ok);
        check(act.faults() == 1, "Retry success: exactly one fault recorded (the initial failure)", ok);
        check(act.dead_letters() == 0, "Retry success: the message was NEVER dead-lettered", ok);
        check(!act.is_stopped(), "Retry success: the actor stayed live", ok);
    }

    // ---- Retry exhausts its budget: falls back to ordinary Fail semantics ------------------
    {
        g_attempts = 0;
        g_succeed_on_attempt = 1'000'000;  // sentinel: never succeeds within the retry budget
        RetryActor b;
        Activation act{&b, RetryActor::dispatch_table(), {}, 1, supervision_of<RetryActor>()};
        act.set_reconstruct({&reset_retry_actor, nullptr});
        error last_dl{};
        act.set_dead_letter_sink({&capture_dead_letter, &last_dl});
        Feeder f(1);
        f.flaky<RetryActor>(act, 0);
        drive(act);

        check(act.restarts_total() == 3,
              "Retry exhaustion: exactly Retry<3>'s budget of restarts were charged", ok);
        check(act.faults() == 4,
              "Retry exhaustion: 1 initial + 3 failed retry attempts == 4 faults", ok);
        check(act.dead_letters() == 1, "Retry exhaustion: the message dead-lettered exactly once", ok);
        check(last_dl.code == errc::supervised_stop,
              "Retry exhaustion: falls back to the ordinary Restart dead-letter error", ok);
        check(b.n == 0, "Retry exhaustion: the handler never succeeded", ok);
    }

    // ---- Retry cannot out-run the actor's own MaxRestarts budget: escalates instead --------
    {
        g_attempts = 0;
        escalate_calls = 0;
        BudgetedRetryActor c;
        Activation act{&c, BudgetedRetryActor::dispatch_table(), {}, 1, supervision_of<BudgetedRetryActor>()};
        act.set_reconstruct({&reset_budgeted_actor, nullptr});
        act.set_escalation_sink({&on_escalate, nullptr});
        Feeder f(1);
        f.flaky<BudgetedRetryActor>(act, 0);
        drive(act);

        check(act.restarts_total() == 1,
              "Budget-bounded retry: only MaxRestarts<1>'s ONE restart was charged, not Retry<3>'s three", ok);
        check(act.escalations() == 1,
              "Budget-bounded retry: the SECOND retry attempt's charge_restart() failure escalated", ok);
        check(escalate_calls == 1, "Budget-bounded retry: the escalation sink was told", ok);
        check(act.is_stopped(), "Budget-bounded retry: escalation's default node action stopped the actor", ok);
        check(act.dead_letters() == 1, "Budget-bounded retry: the message dead-lettered exactly once", ok);
    }

    std::printf("supervision_restart_retry_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
