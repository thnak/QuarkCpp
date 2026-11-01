// Tests ADR-069 / 020-Security §1a (Draft amendment): application-driven admission snapshots on
// SecureTransport. Each guarantee has a CONTROL that runs with that one guarantee removed
// (AdmissionTestFaults) and must FAIL the property, proving the test can see the difference.
//   T1  a live session denied by a new generation is closed by the time publish() returns; the reconnect
//       is refused; admitting it again in a later generation reconnects (positive control).
//   T2  handshake race: a generation published between a handshake's answer and its completion stops it.
//   T3  frames queued inbound from a peer at the moment it is denied are never delivered and are counted.
//   T4  every delivered message carries the generation it was admitted under (never taken from the wire).
//   T5  log-exchange-only: only the control lane passes; the next generation admitting the peer lifts it.
//   T0  snapshot/authority unit properties (closed world, monotonic generation, no reentrant publish).
#include <cstdio>

#include "admission_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct VClock {
    std::int64_t now = 1'000'000'000;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

void t0_snapshot_unit(bool& ok) {
    AdmissionAuthority a;
    check(a.generation() == 0 && a.verdict(NodeId{1}) == AdmissionVerdict::Deny,
          "T0: a fresh authority admits nobody (closed world)", ok);
    check(a.publish(snap(5, {NodeId{1}}, {NodeId{2}})), "T0: gen 5 accepted", ok);
    check(a.verdict(NodeId{1}) == AdmissionVerdict::Admit && a.verdict(NodeId{2}) == AdmissionVerdict::LogExchangeOnly &&
              a.verdict(NodeId{3}) == AdmissionVerdict::Deny,
          "T0: tri-state lookup, absent key is Deny", ok);
    check(a.admitted(NodeId{1}) && !a.admitted(NodeId{2}) && !a.admitted(NodeId{3}),
          "T0: NodeAuthority view: only Admit is admitted (placement/relay)", ok);
    check(!a.publish(snap(5, {NodeId{3}})) && !a.publish(snap(4, {NodeId{3}})) && a.publishes_refused() == 2 &&
              a.verdict(NodeId{3}) == AdmissionVerdict::Deny,
          "T0: equal and older generations are refused and counted (no replayed re-admission)", ok);
    const auto held = a.current();
    check(a.publish(snap(6, {})) && held->lookup(NodeId{1}) == AdmissionVerdict::Admit &&
              a.verdict(NodeId{1}) == AdmissionVerdict::Deny,
          "T0: a snapshot already handed out is immutable across a later publish", ok);
    bool reentrant_refused = false;
    AdmissionAuthority b;
    b.add_listener([&](const AdmissionSnapshot&) { reentrant_refused = !b.publish(snap(99, {NodeId{1}})); });
    check(b.publish(snap(1, {NodeId{1}})) && reentrant_refused && b.generation() == 1,
          "T0: publish from inside a listener is refused (no deadlock)", ok);
}

// ---------------------------------------------------------------------------------------------------
void t1_live_session_closed(bool faulty, bool& ok) {
    const char* tag = faulty ? "T1 CONTROL(no sweep)" : "T1";
    TwoNodeRig r;
    VClock clk;
    r.s1.set_clock(&vclock_read, &clk);
    r.s2.set_clock(&vclock_read, &clk);
    if (faulty) r.s2.set_admission_test_faults({.skip_live_sweep = true});
    r.establish();
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.got2.size() == 1 && r.s2.session_count() == 1, "T1: live session carries traffic at gen 1", ok);
    check(r.s2.session_admission_generation(r.n1) == std::optional<std::uint64_t>(1),
          "T1: the session was answered under generation 1", ok);

    check(r.a2.publish(snap(2, {})), "T1: gen 2 denies n1", ok);  // returns => sessions already re-answered
    if (faulty) {
        // Old behaviour: nothing closes the session when the application changes its mind.
        check(r.s2.session_count() == 1 && r.closed2.empty(),
              (std::string(tag) + ": session survives publish (guarantee absent)").c_str(), ok);
        return;
    }
    check(r.s2.session_count() == 0 && !r.s2.has_session(r.n1), "T1: session closed by the time publish returns", ok);
    check(r.closed2.size() == 1 && r.closed2[0] == r.n1, "T1: the inner connection was told to close", ok);
    check(r.s2.admission_stats().sessions_closed == 1, "T1: counted", ok);

    // The peer (unaware) keeps sending on its old session: refused at the gate, never delivered.
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.got2.size() == 1, "T1: nothing delivered from the denied peer", ok);

    // Reconnect: n1 tears down its side and redials; n2 refuses the opening handshake frame.
    r.s1.on_peer_disconnected(r.n2);
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.s2.session_count() == 0 && r.s2.admission_stats().handshakes_refused >= 1,
          "T1: reconnect refused while denied", ok);

    // Positive control: a later generation admits it again and it reconnects (n1's refused handshake is
    // abandoned after the handshake timeout, then retried from scratch).
    check(r.a2.publish(snap(3, {r.n1})), "T1: gen 3 admits n1", ok);
    clk.now += 6'000'000'000;
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.s2.session_count() == 1, "T1 positive control: reconnect succeeds once re-admitted", ok);
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.got2.size() == 2 && r.got2.back().admission_generation == 3,
          "T1 positive control: traffic flows again, stamped generation 3", ok);
}

// ---------------------------------------------------------------------------------------------------
void t2_handshake_race(bool faulty, bool& ok) {
    TwoNodeRig r;
    if (faulty) r.s2.set_admission_test_faults({.skip_handshake_recheck = true});
    // n2's engine publishes generation 2 (denying n1) INSIDE its final advance(): the answer was read at
    // handshake start under generation 1, and the session is not installed yet.
    r.f2.hooks.on_server_done = [&] { (void)r.a2.publish(snap(2, {})); };
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    r.f2.hooks.on_server_done = nullptr;
    if (faulty) {
        check(r.s2.session_count() == 1,
              "T2 CONTROL(no recheck): the session installed under a stale answer survives", ok);
        return;
    }
    check(r.s2.session_count() == 0 && !r.s2.has_session(r.n1),
          "T2: a generation published mid-handshake stops the session", ok);
    const AdmissionStats st = r.s2.admission_stats();
    check(st.handshakes_denied_at_completion == 1 && st.handshakes_reanswered == 1,
          "T2: re-answered at completion (start generation 1 != current 2) and denied", ok);
    // n1 got no ServerHello, so it never completes either.
    check(r.s1.session_count() == 0, "T2: the denied peer is not told the handshake succeeded", ok);
}

// ---------------------------------------------------------------------------------------------------
void t3_queued_inbound_discarded(bool faulty, bool& ok) {
    constexpr std::size_t kN = 7;
    TwoNodeRig r;
    r.establish();
    for (std::size_t i = 0; i < kN; ++i) r.s1.send(r.n2, make_frame(r.n1, r.n2));
    if (faulty) r.s2.set_admission_test_faults({.skip_live_sweep = true, .skip_inbound_gate = true});
    check(r.fabric.queued() == kN && r.got2.empty(), "T3: N sealed frames sit queued inbound at n2", ok);
    check(r.a2.publish(snap(2, {})), "T3: n1 denied while its frames are queued", ok);
    check(r.got2.empty(), "T3: nothing delivered by the publish itself", ok);
    r.fabric.pump_all();  // the queued frames now reach n2's transport
    if (faulty) {
        check(r.got2.size() == kN, "T3 CONTROL(no gate): the queued frames are delivered after the deny", ok);
        return;
    }
    check(r.got2.empty(), "T3: 0 delivered after publish returns", ok);
    check(r.s2.admission_stats().inbound_discarded == kN, "T3: discard counter == N exactly", ok);
}

// ---------------------------------------------------------------------------------------------------
void t4_generation_stamp(bool control, bool& ok) {
    TwoNodeRig r;
    if (control) r.s2.set_admission_test_faults({.skip_inbound_gate = true});  // stamping removed
    r.establish();
    MessageFrame lie = make_frame(r.n1, r.n2);
    lie.admission_generation = 999;  // a sender cannot forge the stamp: it is never taken from the wire
    r.s1.send(r.n2, std::move(lie));
    r.fabric.pump_all();
    if (control) {
        check(!r.got2.empty() && r.got2.back().admission_generation == 0,
              "T4 CONTROL(no stamp): the delivered frame carries generation 0, not the one it was admitted under", ok);
        return;
    }
    check(r.got2.size() == 1 && r.got2[0].admission_generation == 1 && r.got2[0].from == r.n1,
          "T4: delivered under gen 1, stamped 1 with the peer identity", ok);
    (void)r.a2.publish(snap(4, {r.n1}));  // still admitted, newer generation
    r.s1.send(r.n2, make_frame(r.n1, r.n2));
    r.fabric.pump_all();
    check(r.got2.size() == 2 && r.got2[1].admission_generation == 4, "T4: next frame stamped with generation 4", ok);
    check(r.s2.session_admission_generation(r.n1) == std::optional<std::uint64_t>(4),
          "T4: the surviving session was re-answered under generation 4", ok);
}

// ---------------------------------------------------------------------------------------------------
void t5_log_exchange_only(bool faulty, bool& ok) {
    TwoNodeRig r;
    if (faulty) r.s2.set_admission_test_faults({.skip_inbound_gate = true});
    r.establish();
    check(r.a2.publish(snap(2, {}, {r.n1})), "T5: gen 2 restricts n1 to log exchange", ok);
    r.s1.send(r.n2, make_frame(r.n1, r.n2, FrameKind::Data));
    r.s1.send(r.n2, make_frame(r.n1, r.n2, FrameKind::Control));
    r.fabric.pump_all();
    if (faulty) {
        check(r.got2.size() == 2, "T5 CONTROL(no gate): the data lane is NOT restricted", ok);
        return;
    }
    check(r.got2.size() == 1 && r.got2[0].kind == FrameKind::Control, "T5: only the control lane passes", ok);
    check(r.s2.admission_stats().lane_refused == 1, "T5: the data frame is refused and counted", ok);
    check(r.s2.session_count() == 1 && r.closed2.empty(), "T5: log-only does not close the session", ok);
    // A later generation re-asks the question: admitting n1 lifts the restriction.
    check(r.a2.publish(snap(3, {r.n1})), "T5: gen 3 admits n1", ok);
    r.s1.send(r.n2, make_frame(r.n1, r.n2, FrameKind::Data));
    r.fabric.pump_all();
    check(r.got2.size() == 2 && r.got2[1].kind == FrameKind::Data && r.got2[1].admission_generation == 3,
          "T5: data lane flows again under generation 3", ok);
    // A handshake while log-only is allowed (the log exchange needs a session), and a later Deny is not.
    check(r.a2.publish(snap(4, {}, {r.n1})), "T5: gen 4 log-only again", ok);
    r.s2.on_peer_disconnected(r.n1);
    r.s1.on_peer_disconnected(r.n2);
    r.s1.send(r.n2, make_frame(r.n1, r.n2, FrameKind::Control));
    r.fabric.pump_all();
    check(r.s2.session_count() == 1, "T5: a log-exchange-only peer may still handshake", ok);
}

}  // namespace

int main() {
    bool ok = true;
    t0_snapshot_unit(ok);
    t1_live_session_closed(false, ok);
    t1_live_session_closed(true, ok);
    t2_handshake_race(false, ok);
    t2_handshake_race(true, ok);
    t3_queued_inbound_discarded(false, ok);
    t3_queued_inbound_discarded(true, ok);
    t4_generation_stamp(false, ok);
    t4_generation_stamp(true, ok);
    t5_log_exchange_only(false, ok);
    t5_log_exchange_only(true, ok);
    std::printf("admission_secure_transport_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
