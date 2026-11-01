// Tests ADR-069 claim C15 (red-team #47, finding N3): the close path for a denied device must not redial
// it. Implements 020 §1a as amended by ADR-069 "Red-team of the merged changes (#47)": TcpTransport::
// set_admission(authority) refuses a denied peer at HELLO, closes its connection when a snapshot denies it,
// never dials or redials it, and lifts all of that when a later snapshot no longer denies it.
//
// Real sockets, two bare TcpTransports (the transport's own behaviour; the session layer on top is exercised
// by admission_mobile_composition_test) that both know each other's endpoint (the LAN-discovery shape), one
// AdmissionAuthority per node.
//   T1 deny at node 2: node 2's connection to node 1 closes and STAYS closed (connections_open == 0 in every
//      sample, 0 reconnects, 0 frames delivered) while node 1 keeps talking (every dial of node 1 is refused
//      at HELLO and counted).
//   T2 a later snapshot admits node 1: traffic flows again.
//   T3 CONTROL: the previous recommendation (no transport admission; the application closes the denied
//      device's connection with reset_peer_connection) leaves node 2 holding or reopening a connection to the
//      denied device (red-team: OBSERVED).
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

void run(bool transport_admission) {
    const NodeId n1{1}, n2{2};
    AdmissionAuthority a2;  // declared first: it outlives the transport that listens to it
    net::TcpTransport t1(n1, pal::ipv4_loopback, 0), t2(n2, pal::ipv4_loopback, 0);
    t1.set_allow_insecure_plaintext(true);
    t2.set_allow_insecure_plaintext(true);
    Recorder at1, at2;
    t1.on_receive([&](MessageFrame f) { at1(std::move(f)); });
    t2.on_receive([&](MessageFrame f) { at2(std::move(f)); });
    if (transport_admission) t2.set_admission(a2);
    (void)a2.publish(snap(1, {n1}));
    if (!(t1.start() && t2.start())) {
        check(false, "transports start");
        return;
    }
    t1.add_peer(loopback_endpoint(n2, t2.listen_port()));
    t2.add_peer(loopback_endpoint(n1, t1.listen_port()));  // both know each other
    for (int i = 0; i < 300 && at2.count() < 5; ++i) {
        t1.send(n2, make_frame(n1, n2));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(at2.count() >= 5, "setup: traffic flows before the deny");

    (void)a2.publish(snap(2, {}));  // node 2's application removes node 1
    if (!transport_admission) t2.reset_peer_connection(n1);  // the old recommended close path
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::uint64_t r0 = t2.reconnects(), g0 = at2.count();
    std::uint64_t open_samples = 0, samples = 0;
    for (int ms = 0; ms < 3000; ms += 10) {
        t1.send(n2, make_frame(n1, n2));  // the removed device keeps talking
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++samples;
        if (t2.connections_open() > 0) ++open_samples;
    }
    std::printf("%s: 3 s after deny: node 2 connections_open>0 in %llu/%llu samples, reconnects(+)=%llu, delivered(+)=%llu, "
                "refused_by_transport=%llu\n",
                transport_admission ? "T1 transport admission" : "T3 CONTROL (reset close path)",
                static_cast<unsigned long long>(open_samples), static_cast<unsigned long long>(samples),
                static_cast<unsigned long long>(t2.reconnects() - r0),
                static_cast<unsigned long long>(at2.count() - g0),
                static_cast<unsigned long long>(t2.admission_refused()));
    if (transport_admission) {
        check(open_samples == 0, "T1: node 2 holds no connection to the denied device");
        check(t2.reconnects() == r0, "T1: node 2 never redials it");
        check(at2.count() == g0, "T1: nothing it sends is delivered");
        check(t2.admission_refused() > 0, "T1: its dials are refused at HELLO and counted");

        (void)a2.publish(snap(3, {n1}));  // T2: admitted again
        bool back = false;
        for (int i = 0; i < 800 && !back; ++i) {  // at most ~10 s of real time
            t1.send(n2, make_frame(n1, n2));
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
            back = at2.count() > g0 + 3;
        }
        std::printf("T2 re-admitted: traffic resumed=%d (delivered +%llu)\n", static_cast<int>(back),
                    static_cast<unsigned long long>(at2.count() - g0));
        check(back, "T2: a later admitting snapshot restores traffic");
        // and node 2 can talk to node 1 again too
        const std::uint64_t h0 = at1.count();
        for (int i = 0; i < 5; ++i) t2.send(n1, make_frame(n2, n1));
        check(wait_until([&] { return at1.count() >= h0 + 5; }, 3000), "T2: node 2 reaches node 1 again");
    } else {
        check(open_samples > 0, "T3 control: the old close path holds or reopens a connection to the denied device");
    }
    t1.stop();
    t2.stop();
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run(true);
    run(false);
    std::printf(g_ok ? "tcp_admission_redial_test: PASS\n" : "tcp_admission_redial_test: FAIL\n");
    return g_ok ? 0 : 1;
}
