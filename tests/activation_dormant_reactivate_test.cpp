// Tests ADR-028 Phase 2 §producer-side reactivation — `Activation::post()`/`notify_enqueued()`/
// `post_governed()` all route through the new `wake_after_enqueue()` (activation.hpp), which extends
// the ordinary Idle->Scheduled wake CAS with a Dormant->Scheduled fallback (`readmit_from_dormant()`)
// when the ordinary CAS misses because the activation was evicted. Single-threaded and deterministic
// (no race here — the concurrent close-out abort race is the separate
// activation_deactivate_close_out_dekker_test.cpp isolating litmus; this proves the OTHER side: a
// producer posting to an ALREADY-Dormant activation, after the close-out has fully committed).
//
// Proves, end to end, using the real Activation/Descriptor machinery (no Engine needed):
//   1. An activation driven all the way to a clean Dormant evict (mirrors
//      activation_deactivate_flag_test.cpp Case A) can be reactivated by a plain `post()` of a real
//      message: the wake edge fires (Dormant->Scheduled), and the message is NOT lost — it drains
//      normally once the (simulated) engine re-acquires and runs the activation.
//   2. `notify_enqueued()` (the "caller already enqueued through the mailbox directly" producer path,
//      engine.hpp's Engine::notify) has the identical Dormant fallback.
#include <cstdio>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {
struct Ping {
    int n;
};

struct Pinger : Actor<Pinger, Sequential> {
    using protocol = Protocol<Ping>;
    int pings = 0;
    void handle(const Ping& p) noexcept { pings += p.n; }
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Drive `act` all the way to a clean Dormant evict via a hand-built Deactivate control descriptor —
// the exact Case-A sequence from activation_deactivate_flag_test.cpp. Each call needs its OWN fresh
// descriptor (a real engine gives every Activation its own dedicated, never-shared control descriptor
// — see the ADR-028 Phase 2 member comment on Activation::deactivate_descriptor_ — so a leaked/reused
// descriptor across activations here would be a test bug, not a scenario the real engine ever hits).
void evict_to_dormant(Activation& act, Descriptor& dctrl, bool& ok) {
    dctrl.payload = nullptr;
    dctrl.set_flags(kControlFlagDeactivate);

    check(act.post(&dctrl), "Deactivate post wakes Idle->Scheduled", ok);
    check(act.try_acquire(), "acquire Scheduled->Running", ok);
    check(act.drain_step(64) == Activation::DrainOutcome::DrainedEmpty,
          "drain claims the control descriptor, then empties", ok);
    check(!act.close_out(), "close_out_retire commits the evict (mailbox empty at the probe)", ok);
    check(act.state() == ExecState::Dormant, "activation is Dormant", ok);
}
}  // namespace

int main() {
    bool ok = true;

    // ---- Case 1: post() reactivates a Dormant activation, message is NOT lost -------------------
    {
        Pinger actor;
        Activation act{&actor, Pinger::dispatch_table()};
        Descriptor dctrl1;
        evict_to_dormant(act, dctrl1, ok);

        Ping ping{7};
        Descriptor dping;
        dping.payload = &ping;
        stamp<Pinger, Ping>(dping);

        const bool wake = act.post(&dping);
        check(wake, "post() to a Dormant activation wakes via readmit_from_dormant()", ok);
        check(act.state() == ExecState::Scheduled, "Dormant->Scheduled after readmit", ok);

        check(act.try_acquire(), "acquire Scheduled->Running (the reactivated lane)", ok);
        const auto out = act.drain_step(64);
        check(out == Activation::DrainOutcome::DrainedEmpty, "the reactivating message drains to empty", ok);
        check(actor.pings == 7, "the reactivating message WAS dispatched — no loss across reactivation", ok);

        check(!act.close_out(), "genuine close-out to Idle once truly empty", ok);
        check(act.state() == ExecState::Idle, "Idle after the real close-out", ok);
    }

    // ---- Case 2: notify_enqueued() (the "caller already enqueued" producer path) has the same
    //              Dormant fallback -----------------------------------------------------------------
    {
        Pinger actor2;
        Activation act2{&actor2, Pinger::dispatch_table()};
        Descriptor dctrl2;
        evict_to_dormant(act2, dctrl2, ok);

        Ping ping{3};
        Descriptor dping2;
        dping2.payload = &ping;
        stamp<Pinger, Ping>(dping2);

        act2.mailbox().enqueue(&dping2);  // caller enqueues directly (mirrors Engine::notify's contract)
        const bool wake = act2.notify_enqueued();
        check(wake, "notify_enqueued() on a Dormant activation wakes via readmit_from_dormant()", ok);
        check(act2.state() == ExecState::Scheduled, "Dormant->Scheduled after readmit", ok);

        check(act2.try_acquire(), "acquire the reactivated lane", ok);
        check(act2.drain_step(64) == Activation::DrainOutcome::DrainedEmpty, "drains to empty", ok);
        check(actor2.pings == 3, "message dispatched — no loss", ok);
    }

    // ---- Case 3: a second, redundant readmit attempt (state already Scheduled/Running) never
    //              double-wakes ----------------------------------------------------------------------
    {
        Pinger actor3;
        Activation act3{&actor3, Pinger::dispatch_table()};
        Descriptor dctrl3;
        evict_to_dormant(act3, dctrl3, ok);

        Ping p1{1}, p2{2};
        Descriptor d1, d2;
        d1.payload = &p1;
        stamp<Pinger, Ping>(d1);
        d2.payload = &p2;
        stamp<Pinger, Ping>(d2);

        check(act3.post(&d1), "first post reactivates (Dormant->Scheduled)", ok);
        check(!act3.post(&d2), "second post while already Scheduled never double-wakes", ok);
        check(act3.state() == ExecState::Scheduled, "still Scheduled (one wake, not two)", ok);

        check(act3.try_acquire(), "acquire the reactivated lane", ok);
        check(act3.drain_step(64) == Activation::DrainOutcome::DrainedEmpty, "both messages drain", ok);
        check(actor3.pings == 3, "both reactivating messages dispatched exactly once each", ok);
    }

    std::printf("activation_dormant_reactivate_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
