// Tests ADR-071 claim C7b (red-team #47, finding B1): a network switch must not lock the phone out WHOEVER
// dialled the old connection. Implements 021 §2 (dial dedup) as amended by ADR-071 "Red-team of the merged
// changes (#47)": a new connection from an already-identified peer supersedes the old one as a RECONNECT
// once the old one is older than a short race window, whichever side dialled; the node-id election is
// kept only for a genuine simultaneous dial. The superseded connection fires peer_down_hook so a
// SecureTransport drops the session negotiated on it.
//
// Real sockets, a black-hole proxy (tests/black_hole_proxy.hpp). The PC dials the phone THROUGH the proxy
// (the shape LAN discovery, ADR-070, produces for ~half of all pairs); the phone's Wi-Fi path then goes
// silently black and the phone reconnects directly. Matrix:
//   plaintext / secure (mock handshake, SecureTransport) x PC id lower / higher than the phone's.
// Secure + PC lower = the phone is the HANDSHAKE RESPONDER (server role) after the switch, a case the
// first red-team could not reach. CONTROL: the legacy election (TcpTestFaults::legacy_dial_election)
// must show the lock-out (0/50 both ways, a reconnect storm) for PC id < phone id and not for the swap.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

#include "admission_test_util.hpp"
#include "black_hole_proxy.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Result {
    std::uint64_t up = 0, down = 0, phone_reconnects = 0, pc_dedup_closed = 0, superseded = 0;
    bool connected_after = false;
};

Result run(std::uint64_t pc_id, std::uint64_t phone_id, bool secure, bool legacy, int age_ms = 1500) {
    const NodeId PC{pc_id}, PHONE{phone_id};
    net::TcpTransport tcp_pc(PC, pal::ipv4_loopback, 0);
    net::TcpTransport tcp_phone(PHONE, pal::ipv4_loopback, 0);
    net::TcpTestFaults faults;
    faults.legacy_dial_election = legacy;
    tcp_pc.set_test_faults(faults);
    tcp_phone.set_test_faults(faults);
    Recorder at_pc, at_phone;
    MockCipher unused{0};
    SecureTransport s_pc{tcp_pc, unused, PC}, s_phone{tcp_phone, unused, PHONE};
    auto id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial i1{id_blob}, i2{id_blob};
    TrustStore r1{trust_blob}, r2{trust_blob};
    MockHandshakeFactory f1, f2;
    Transport* x_pc = &tcp_pc;
    Transport* x_phone = &tcp_phone;
    if (secure) {
        s_pc.enable_handshake(f1, ClusterId{555}, i1, r1);
        s_phone.enable_handshake(f2, ClusterId{555}, i2, r2);
        tcp_pc.set_peer_down_hook([&](NodeId p) { s_pc.on_peer_disconnected(p); });
        tcp_phone.set_peer_down_hook([&](NodeId p) { s_phone.on_peer_disconnected(p); });
        x_pc = &s_pc;
        x_phone = &s_phone;
    } else {
        tcp_pc.set_allow_insecure_plaintext(true);
        tcp_phone.set_allow_insecure_plaintext(true);
    }
    x_pc->on_receive([&](MessageFrame f) { at_pc(std::move(f)); });
    x_phone->on_receive([&](MessageFrame f) { at_phone(std::move(f)); });
    Result r;
    if (!(tcp_pc.start() && tcp_phone.start())) return r;
    BlackHoleProxy proxy(tcp_phone.listen_port());
    if (!proxy.start()) return r;
    tcp_pc.add_peer(loopback_endpoint(PHONE, proxy.port()));          // PC reaches the phone via the Wi-Fi path
    tcp_phone.add_peer(loopback_endpoint(PC, tcp_pc.listen_port()));  // the phone knows the PC (discovery)

    // Phase A: connectivity both ways (a secure session needs a handshake first; early frames may drop).
    auto flow_both_ways = [&](int budget_ms) {
        const std::uint64_t a0 = at_pc.count(), b0 = at_phone.count();
        for (int ms = 0; ms < budget_ms; ms += 20) {
            x_pc->send(PHONE, make_frame(PC, PHONE));
            x_phone->send(PC, make_frame(PHONE, PC));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (at_pc.count() > a0 + 2 && at_phone.count() > b0 + 2) return true;
        }
        return false;
    };
    if (!flow_both_ways(4000)) return r;  // baseline must work, or the test is void

    // The old connection must be older than the race window (1 s) for the supersede rule; age_ms = 0 is the
    // YOUNG case (a connection that dies within the window is still handled, after the window).
    std::this_thread::sleep_for(std::chrono::milliseconds(age_ms));
    proxy.black_hole_existing();                // the phone's old path vanishes silently
    tcp_phone.reset_peer_connection(PC);        // what MobileMember::network_changed() does per peer
    r.connected_after = flow_both_ways(4000);
    // Phase B: it must come back (above)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Phase C: once settled, a burst is delivered completely, both ways.
    const std::uint64_t a1 = at_pc.count(), b1 = at_phone.count();
    for (int i = 0; i < 50; ++i) {
        x_phone->send(PC, make_frame(PHONE, PC));
        x_pc->send(PHONE, make_frame(PC, PHONE));
    }
    wait_until([&] { return at_pc.count() >= a1 + 50 && at_phone.count() >= b1 + 50; }, 2500);
    r.up = at_pc.count() - a1;
    r.down = at_phone.count() - b1;
    r.phone_reconnects = tcp_phone.reconnects();
    r.pc_dedup_closed = tcp_pc.dedup_closed();
    r.superseded = tcp_pc.reconnects_superseded() + tcp_phone.reconnects_superseded();
    proxy.stop();
    tcp_phone.stop();
    tcp_pc.stop();
    return r;
}

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (const bool secure : {false, true}) {
        for (const auto ids : {std::pair<std::uint64_t, std::uint64_t>{1, 2}, {2, 1}}) {
            const Result r = run(ids.first, ids.second, secure, /*legacy=*/false);
            std::printf("%s PC=%llu PHONE=%llu: connected_after=%d pc<-phone %llu/50 phone<-pc %llu/50 phone_reconnects=%llu "
                        "pc_dedup_closed=%llu superseded=%llu\n",
                        secure ? "secure   " : "plaintext", (unsigned long long)ids.first, (unsigned long long)ids.second,
                        (int)r.connected_after, (unsigned long long)r.up, (unsigned long long)r.down,
                        (unsigned long long)r.phone_reconnects, (unsigned long long)r.pc_dedup_closed,
                        (unsigned long long)r.superseded);
            check(r.connected_after, "traffic flows again both ways after the network switch");
            check(r.up == 50 && r.down == 50, "a 50-frame burst after the switch is delivered 50/50 both ways");
            check(r.phone_reconnects <= 3, "reconnects are bounded (no storm)");
            check(r.pc_dedup_closed <= 3, "dedup closes are bounded (no storm)");
        }
    }
    // Young case: the old connection dies inside the race window. The new connection is refused by the
    // election until the window passes (backoff retries, bounded), then the phone is let in.
    {
        const Result y = run(1, 2, false, /*legacy=*/false, /*age_ms=*/0);
        std::printf("plaintext young PC=1 PHONE=2: connected_after=%d up=%llu down=%llu phone_reconnects=%llu\n",
                    (int)y.connected_after, (unsigned long long)y.up, (unsigned long long)y.down,
                    (unsigned long long)y.phone_reconnects);
        check(y.connected_after && y.up == 50 && y.down == 50, "young: the phone is let in once the race window passes");
        check(y.phone_reconnects <= 12, "young: reconnect attempts stay bounded by the window and the backoff");
    }
    // CONTROLS: the legacy election (the pre-fix rule) locks the phone out when the PC dialled and has
    // the lower id, and does not when the ids are swapped (the red-team's observed 0/50 vs 50/50).
    {
        const Result low = run(1, 2, false, /*legacy=*/true);
        const Result high = run(2, 1, false, /*legacy=*/true);
        std::printf("CONTROL legacy PC=1 PHONE=2: connected_after=%d up=%llu down=%llu phone_reconnects=%llu\n",
                    (int)low.connected_after, (unsigned long long)low.up, (unsigned long long)low.down,
                    (unsigned long long)low.phone_reconnects);
        std::printf("CONTROL legacy PC=2 PHONE=1: connected_after=%d up=%llu down=%llu phone_reconnects=%llu\n",
                    (int)high.connected_after, (unsigned long long)high.up, (unsigned long long)high.down,
                    (unsigned long long)high.phone_reconnects);
        check(!low.connected_after && low.up == 0 && low.down == 0,
              "control: legacy rule locks out when PC dialled with the lower id");
        check(low.phone_reconnects > 3, "control: legacy rule storms reconnects");
        check(high.up == 50 && high.down == 50, "control: the swapped ids never locked out");
    }
    std::printf(g_ok ? "tcp_reconnect_supersede_test: PASS\n" : "tcp_reconnect_supersede_test: FAIL\n");
    return g_ok ? 0 : 1;
}
