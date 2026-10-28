// Tests ADR-028 Phase 1 §control-flag recognition + close_out_retire() — using the REAL Activation/
// Descriptor machinery (no Engine needed, mirrors tests/exec_dispatch_test.cpp's bare-Activation
// style), single-threaded and fully deterministic (no race here — that's the separate
// activation_deactivate_close_out_dekker_test.cpp isolating litmus).
//
// Proves, end to end:
//   1. A hand-built Deactivate control descriptor (kControlFlagDeactivate set via set_flags()) is
//      recognized by drain_step() off the SAME flags word try_claim() already loads — it is reclaimed
//      WITHOUT ever reaching the dispatch table (no handler runs), and sets retire_requested_.
//   2. close_out() with retire_requested_ set commits the eviction (Running -> Dormant) when the
//      mailbox is genuinely empty at the probe.
//   3. The abort-eviction race: if a real message is already sitting in the mailbox by the time
//      close_out_retire()'s probe runs, the eviction ABORTS (Dormant -> Running) and that message
//      dispatches normally on the next drain_step() — no message loss, no actor-instance destruction
//      out from under an in-flight dispatch.
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
}  // namespace

int main() {
    bool ok = true;
    Pinger actor;
    Activation act{&actor, Pinger::dispatch_table()};

    // ---- Case A: clean evict (mailbox genuinely empty when the retire commits) -----------------
    {
        Descriptor dctrl;
        dctrl.payload = nullptr;               // never dereferenced — this descriptor never dispatches
        dctrl.set_flags(kControlFlagDeactivate);

        const bool wake = act.post(&dctrl);
        check(wake, "first post performs the Idle->Scheduled wake edge", ok);
        check(act.try_acquire(), "worker acquires Scheduled->Running", ok);

        const auto out = act.drain_step(64);
        check(out == Activation::DrainOutcome::DrainedEmpty, "drain claims the control descriptor, then empties", ok);
        check(actor.pings == 0, "the control descriptor never reached the dispatch table", ok);

        check(!act.close_out(), "close_out_retire commits the evict (mailbox empty at the probe)", ok);
        check(act.state() == ExecState::Dormant, "activation is Dormant after a clean evict", ok);
        check(act.went_dormant(), "went_dormant() observer agrees", ok);
    }

    // ---- Case B: abort-eviction race (a message is already queued when the retire probes) -------
    // Not a genuine data race here (single-threaded) — this deterministically exercises the SAME
    // code path a concurrent [Deactivate, M] race takes: drain_step() sees the queue empty (claims
    // only the control descriptor), retire_requested_ is set, but by the time close_out_retire()'s
    // probe runs, a message has already been posted — exactly like a producer racing in between the
    // close-out's release store and its re-probe. Uses a fresh actor/Activation (Case A's is now
    // Dormant, and reactivating it is Phase 4's not-yet-built seam).
    {
        Pinger actor2;
        Activation act2{&actor2, Pinger::dispatch_table()};

        Descriptor dctrl3;
        dctrl3.payload = nullptr;
        dctrl3.set_flags(kControlFlagDeactivate);
        act2.post(&dctrl3);
        check(act2.try_acquire(), "acquire Running for Case B", ok);

        const auto out = act2.drain_step(64);
        check(out == Activation::DrainOutcome::DrainedEmpty, "drain claims the control descriptor, then empties", ok);

        // Simulate a message racing in BETWEEN drain_step() seeing empty and close_out_retire()'s
        // probe: post a real Ping now, before calling close_out(). Since the worker still holds
        // Running, this post()'s notify_enqueued() correctly no-ops (no wake) — the message just
        // waits in the mailbox for the current owner's own close-out probe to find it.
        Ping ping{5};
        Descriptor dping;
        dping.payload = &ping;
        stamp<Pinger, Ping>(dping);
        const bool wake_from_running = act2.post(&dping);
        check(!wake_from_running, "post() while Running never wakes a second worker", ok);

        check(act2.close_out(), "close_out_retire ABORTS the evict (message found at the probe)", ok);
        check(act2.state() == ExecState::Running, "activation stays Running after the abort", ok);

        // The racing message now dispatches normally, exactly as if no eviction had been attempted.
        const auto out2 = act2.drain_step(64);
        check(out2 == Activation::DrainOutcome::DrainedEmpty, "the racing message drains to empty", ok);
        check(actor2.pings == 5, "the racing message WAS dispatched -- no message loss across the abort", ok);

        check(!act2.close_out(), "genuine close-out to Idle once truly empty", ok);
        check(act2.state() == ExecState::Idle, "Idle after the real close-out", ok);
    }

    std::printf("activation_deactivate_flag_test: %s  (case-A-pings=%d)\n", ok ? "OK" : "FAIL", actor.pings);
    return ok ? 0 : 1;
}
