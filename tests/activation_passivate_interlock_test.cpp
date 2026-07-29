// Tests ADR-028 Phase 8 §The interlock — using the REAL Activation machinery (no Engine needed,
// mirrors tests/activation_deactivate_flag_test.cpp's bare-Activation style), single-threaded and
// fully deterministic.
//
// Proves, end to end:
//   1. try_claim_deactivate_token() succeeds exactly once while a claim is pending — a second
//      concurrent claim attempt (from either trigger: the automatic wheel or on-demand passivate())
//      fails, exactly the interlock that prevents deactivate_descriptor_ from being enqueued twice
//      while still linked from a prior enqueue.
//   2. clear_deactivate_token() (called at the exact point drain_step()/drain_step_governed_seq()
//      claims the control descriptor off the mailbox) re-opens the interlock — a claim attempted
//      after that point succeeds again.
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

    // ---- Case A: claim once, second concurrent claim fails (idempotent no-op) --------------------
    {
        check(act.try_claim_deactivate_token(), "first claim (e.g. the wheel firing) succeeds", ok);
        check(!act.try_claim_deactivate_token(),
              "a second concurrent claim (e.g. an on-demand passivate() racing the wheel) fails -- "
              "already pending, never a double-post",
              ok);
        check(!act.try_claim_deactivate_token(), "repeated claims keep failing while still pending", ok);
    }

    // ---- Case B: clear re-opens the interlock (the drain-claim point) -----------------------------
    {
        act.clear_deactivate_token();  // simulates drain_step() dequeuing the control descriptor
        check(act.try_claim_deactivate_token(),
              "claim succeeds again once the prior post was dequeued/unlinked", ok);
        check(!act.try_claim_deactivate_token(), "and is exclusive again while this one is pending", ok);
        act.clear_deactivate_token();
        check(act.try_claim_deactivate_token(), "the cycle repeats: clear always re-opens the claim", ok);
    }

    std::printf("activation_passivate_interlock_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
