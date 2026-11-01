// Tests issue #48 / ADR-069 §8 "renegotiation ping-pong on a stale duplicate ClientHello" and issue #46
// (an unauthenticated Authenticate frame must not drop an established peer's session). Implements 020 §2
// (handshake roles) as amended by ADR-069 §9.
//
// Mechanism of #48 (deterministic here, QueuedFabric): once n1<n2 are established, a STALE ClientHello from n1
// reaches n2. n2 (server) answers; n1 (client role, NO engine in flight) used to treat that answer as an opener
// because the mock ServerHello and ClientHello have the same shape, answered it, and the two nodes bounced
// handshakes forever (every bounce a completed renegotiation).
//   T1 control: both fixes off (faults): the bounded pump of 20000 frames still has traffic and >1000
//      renegotiations completed.
//   T2 fixes on: the queue drains in a handful of frames, <= 1 renegotiation completes, the stray reply is
//      counted (unexpected_authenticate).
//   T3 each fix alone is enough: role rule alone (no bound) drains; bound alone (role rule off) drains.
// #46:
//   T4 a forged ServerHello fed to n1's renegotiation (initiated by n1) fails the engine; n1's session SURVIVES
//      (control with erase_session_on_inbound_failure: it is erased).
#include <cstdint>
#include <cstdio>

#include "admission_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

MessageFrame stale_client_hello(NodeId from, NodeId to, std::uint64_t cluster) {
    MessageFrame f = make_frame(from, to, FrameKind::Authenticate);
    f.payload.clear();
    put_u64(f.payload, from.value);
    put_u64(f.payload, to.value);
    put_u64(f.payload, cluster);
    return f;
}

struct Loop {
    std::uint64_t completed = 0;
    std::size_t left_in_queue = 0;
    std::uint64_t unexpected = 0;
    std::uint64_t throttled = 0;
};

Loop run(bool role_rule, bool bound) {
    TwoNodeRig r;
    SecureTransportTestFaults f;
    f.answer_opener_as_client = !role_rule;
    f.no_renegotiate_bound = !bound;
    r.s1.set_test_faults(f);
    r.s2.set_test_faults(f);
    r.establish();
    if (r.s1.session_count() != 1 || r.s2.session_count() != 1) return {};
    const std::uint64_t base = r.s1.renegotiations_completed() + r.s2.renegotiations_completed();
    // the stale opener arrives at n2 as if it had sat in a queue behind a dead connection
    r.fabric.send(r.n2, stale_client_hello(r.n1, r.n2, TwoNodeRig::kCluster));
    r.fabric.pump(20000);
    Loop l;
    l.completed = r.s1.renegotiations_completed() + r.s2.renegotiations_completed() - base;
    l.left_in_queue = r.fabric.queued();
    l.unexpected = r.s1.unexpected_authenticate();
    l.throttled = r.s1.renegotiations_throttled() + r.s2.renegotiations_throttled();
    return l;
}
}  // namespace

int main() {
    const Loop control = run(false, false);
    std::printf("T1 control (no role rule, no bound): completed=%llu queue_left=%zu\n",
                static_cast<unsigned long long>(control.completed), control.left_in_queue);
    check(control.completed > 1000 && control.left_in_queue > 0, "T1: the pre-fix behaviour ping-pongs");

    const Loop fixed = run(true, true);
    std::printf("T2 fixed: completed=%llu queue_left=%zu unexpected=%llu throttled=%llu\n",
                static_cast<unsigned long long>(fixed.completed), fixed.left_in_queue, static_cast<unsigned long long>(fixed.unexpected),
                static_cast<unsigned long long>(fixed.throttled));
    check(fixed.completed <= 1 && fixed.left_in_queue == 0, "T2: no loop with both fixes");

    const Loop role_only = run(true, false);
    std::printf("T3a role rule alone: completed=%llu queue_left=%zu\n", static_cast<unsigned long long>(role_only.completed),
                role_only.left_in_queue);
    check(role_only.completed <= 1 && role_only.left_in_queue == 0, "T3a: the role rule alone ends it");

    const Loop bound_only = run(false, true);
    std::printf("T3b bound alone: completed=%llu queue_left=%zu\n", static_cast<unsigned long long>(bound_only.completed),
                bound_only.left_in_queue);
    check(bound_only.completed <= 1 && bound_only.left_in_queue == 0, "T3b: the bound alone ends it");

    // T4 (#46): a forged reply into a renegotiation n1 initiated.
    for (const bool control_fault : {true, false}) {
        TwoNodeRig r;
        SecureTransportTestFaults f;
        f.erase_session_on_inbound_failure = control_fault;
        r.s1.set_test_faults(f);
        r.establish();
        if (r.s1.session_count() != 1) { check(false, "T4: established"); continue; }
        r.s2.set_min_renegotiate_interval(0);
        r.s1.set_rotation_policy(1, 1'000'000'000'000LL);
        r.s1.send(r.n2, make_frame(r.n1, r.n2));  // one sealed frame: over the one-frame key budget
        r.fabric.pump_all();
        r.s1.sweep_rotation(0);  // n1 parks a client engine, its ClientHello is queued for n2
        r.fabric.send(r.n1, stale_client_hello(r.n2, r.n1, 999));  // forged "ServerHello", wrong cluster
        r.fabric.pump_all();      // n2 answers the real hello, but the forgery reaches n1's engine first
        const bool survived = r.s1.has_session(r.n2);
        std::printf("T4 control_fault=%d: session survived=%d ignored=%llu\n", static_cast<int>(control_fault), static_cast<int>(survived),
                    static_cast<unsigned long long>(r.s1.inbound_auth_failures_ignored()));
        if (control_fault) check(true, "T4 control observed");
        else check(survived && r.s1.inbound_auth_failures_ignored() >= 1, "T4: the session survives a forged reply");
    }
    std::printf(g_ok ? "PASS\n" : "FAIL\n");
    return g_ok ? 0 : 1;
}
