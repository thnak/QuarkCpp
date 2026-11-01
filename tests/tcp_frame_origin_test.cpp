// Tests ADR-069 claims C13/C14 (red-team #47, finding M1; also closes N5): a frame's `from` must equal the
// identity of the connection it arrived on, and a handshake this node ANSWERED that fails must never
// erase an existing session. Implements 020 §1a / 004 (frame identity) as amended by ADR-069 "Red-team of
// the merged changes (#47)".
//
// Attack (red-team rt_auth_spoof_test): host 99, DENIED by node 2's snapshot, sends Authenticate frames
// whose `from` claims admitted node 1, ten per second. Admission is asked about the claimed `from`, so the
// deny never applied, and the failed answered handshake erased node 1's live session at node 2.
//   T1 control (no attacker): node1 -> node2 delivery is complete.
//   T2 attack with both fixes: delivery is unchanged, 0 sessions lost, the spoofed frames are dropped at
//      the transport and counted (TcpTransport::spoofed_origin_dropped()).
//   T3 each layer alone is enough (defence in depth): origin check off -> the SecureTransport fix keeps
//      the session; answered-failure erase restored -> the origin check keeps the frames out.
//   T4 CONTROL: both fixes off (the pre-fix behaviour): delivery collapses (red-team observed 9%).
//   T5 a relayed frame (from != connection peer) is accepted only when the receiver opted in for that
//      relay (set_accept_relayed_frames), so shape-B relaying (ADR-048) keeps working.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "admission_test_util.hpp"
#include "tcp_test_util.hpp"

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

struct Out {
    int ratio = 0;                       // delivered * 100 / sent
    std::uint64_t spoofed_dropped = 0;   // at node 2's TCP layer
    std::uint64_t session_missing = 0;   // samples with no session for node 1 at node 2
};

Out run(bool attack, bool origin_check, bool erase_on_answered_failure) {
    const NodeId n1{1}, n2{2}, evil{99};
    AdmissionAuthority a1, a2;
    net::TcpTransport t1(n1, pal::ipv4_loopback, 0), t2(n2, pal::ipv4_loopback, 0), tx(evil, pal::ipv4_loopback, 0);
    net::TcpTestFaults tf;
    tf.skip_origin_check = !origin_check;
    t2.set_test_faults(tf);
    tx.set_allow_insecure_plaintext(true);
    MockCipher unused{0};
    SecureTransport s1{t1, unused, n1}, s2{t2, unused, n2};
    SecureTransportTestFaults sf;
    sf.erase_session_on_answered_failure = erase_on_answered_failure;
    sf.no_renegotiate_bound = !origin_check && erase_on_answered_failure;  // the T4 CONTROL is the full pre-fix shape (no #48 bound)
    s2.set_test_faults(sf);
    auto id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial id1{id_blob}, id2{id_blob};
    TrustStore tr1{trust_blob}, tr2{trust_blob};
    MockHandshakeFactory f1, f2;
    s1.enable_handshake(f1, ClusterId{555}, id1, tr1);
    s2.enable_handshake(f2, ClusterId{555}, id2, tr2);
    std::atomic<std::uint64_t> got2{0};
    s1.on_receive([](MessageFrame) {});
    s2.on_receive([&](MessageFrame) { got2.fetch_add(1); });
    s1.set_admission(a1, [&](NodeId p) { t1.reset_peer_connection(p); });
    s2.set_admission(a2, [&](NodeId p) { t2.reset_peer_connection(p); });
    s1.set_reset_hook([&](NodeId p) { t1.reset_peer_connection(p); });
    s2.set_reset_hook([&](NodeId p) { t2.reset_peer_connection(p); });
    t1.set_peer_down_hook([&](NodeId p) { s1.on_peer_disconnected(p); });
    t2.set_peer_down_hook([&](NodeId p) { s2.on_peer_disconnected(p); });
    (void)a1.publish(snap(1, {n2}));
    (void)a2.publish(snap(1, {n1}));  // 99 absent == Deny
    Out o;
    if (!(t1.start() && t2.start() && tx.start())) return o;
    t1.add_peer(loopback_endpoint(n2, t2.listen_port()));
    tx.add_peer(loopback_endpoint(n2, t2.listen_port()));
    for (int i = 0; i < 300 && got2.load() < 5; ++i) {
        s1.send(n2, make_frame(n1, n2));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint64_t before = got2.load();
    std::uint64_t sent = 0, samples = 0;
    for (int ms = 0; ms < 3000; ms += 10) {
        if (attack && ms % 100 == 0) {
            MessageFrame f = make_frame(n1, n2, FrameKind::Authenticate);  // claims to be node 1
            f.payload.assign(24, std::byte{0});
            tx.send(n2, std::move(f));
        }
        s1.send(n2, make_frame(n1, n2));
        ++sent;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++samples;
        if (!s2.session_admission_generation(n1)) ++o.session_missing;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    o.ratio = static_cast<int>((got2.load() - before) * 100 / (sent ? sent : 1));
    o.spoofed_dropped = t2.spoofed_origin_dropped();
    t1.stop();
    t2.stop();
    tx.stop();
    return o;
}

void t5_relayed_frames() {
    const NodeId A{1}, R{2}, B{3};
    for (const bool opt_in : {false, true}) {
        net::TcpTransport ta(A, pal::ipv4_loopback, 0), tr(R, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
        for (auto* t : {&ta, &tr, &tb}) t->set_allow_insecure_plaintext(true);
        tr.set_relay_forwarding([](NodeId, NodeId) { return true; });
        if (opt_in) tb.set_accept_relayed_frames([&](NodeId relay, NodeId origin) { return relay == R && origin == A; });
        Recorder at_b;
        tb.on_receive([&](MessageFrame f) { at_b(std::move(f)); });
        if (!(ta.start() && tr.start() && tb.start())) return;
        ta.add_peer(loopback_endpoint(R, tr.listen_port()));
        tr.add_peer(loopback_endpoint(B, tb.listen_port()));
        ta.set_relay_via(B, {R});
        for (std::uint64_t s = 0; s < 5; ++s) ta.send(B, seq_frame(A, B, s));
        wait_until([&] { return at_b.count() >= 5; }, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("T5 relay opt_in=%d: B received %llu/5, spoofed_origin_dropped=%llu\n",
                    static_cast<int>(opt_in), static_cast<unsigned long long>(at_b.count()),
                    static_cast<unsigned long long>(tb.spoofed_origin_dropped()));
        if (opt_in) check(at_b.count() == 5, "T5: an opted-in relayed frame is delivered");
        else check(at_b.count() == 0 && tb.spoofed_origin_dropped() == 5, "T5: without opt-in a relayed frame is dropped and counted");
        ta.stop();
        tr.stop();
        tb.stop();
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Out control = run(false, true, false);
    std::printf("T1 control (no attacker): delivery %d%%\n", control.ratio);
    check(control.ratio >= 95, "T1: no attacker: delivery complete");

    const Out fixed = run(true, true, false);
    std::printf("T2 attack, both fixes: delivery %d%%, session missing in %llu samples, spoofed frames dropped=%llu\n",
                fixed.ratio, static_cast<unsigned long long>(fixed.session_missing),
                static_cast<unsigned long long>(fixed.spoofed_dropped));
    check(fixed.ratio >= 95, "T2: an attacker's spoofed Authenticate does not reduce delivery");
    check(fixed.session_missing == 0, "T2: node 1's session at node 2 is never lost");
    check(fixed.spoofed_dropped >= 25, "T2: the spoofed frames are dropped at the transport and counted");

    const Out only_secure = run(true, false, false);
    std::printf("T3a origin check OFF, SecureTransport fix ON: delivery %d%%, session missing %llu\n", only_secure.ratio,
                static_cast<unsigned long long>(only_secure.session_missing));
    check(only_secure.ratio >= 95 && only_secure.session_missing == 0, "T3a: the SecureTransport fix alone keeps the session");
    const Out only_tcp = run(true, true, true);
    std::printf("T3b origin check ON, answered-failure erase restored: delivery %d%%, session missing %llu\n",
                only_tcp.ratio, static_cast<unsigned long long>(only_tcp.session_missing));
    check(only_tcp.ratio >= 95 && only_tcp.session_missing == 0, "T3b: the origin check alone keeps the frames out");

    const Out old = run(true, false, true);
    std::printf("T4 CONTROL both fixes off (pre-fix): delivery %d%%, session missing in %llu samples\n", old.ratio,
                static_cast<unsigned long long>(old.session_missing));
    check(old.ratio < 50, "T4 control: the pre-fix behaviour collapses delivery");
    check(old.session_missing > 100, "T4 control: the pre-fix behaviour loses the session");

    t5_relayed_frames();
    std::printf(g_ok ? "tcp_frame_origin_test: PASS\n" : "tcp_frame_origin_test: FAIL\n");
    return g_ok ? 0 : 1;
}
