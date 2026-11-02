// Issue #68 (for #58, ADR-071 §5/§10, ADR-072 item 4): a peer that reconnects from a NEW SOURCE IP on the pinned wire (a phone moving
// Wi-Fi to cellular, the loopback half). The dial's source address is set through the #68 seam (pal::tcp_connect source_addr,
// TcpTransport::set_dial_source_addr).
//   R1  reset-and-redial: a (lower id, dials from 127.0.0.1) and b exchange 50 messages each way; a's connection is reset, a's dials
//       now leave from 127.0.0.2, and 50 messages each way flow again. Pinned wire: no lock-out (the new session is up inside 5 s),
//       b holds exactly one session for a, every payload arrives exactly once.
//   R2  the half-open case, which R1 cannot show (a reset closes b's old connection, so nothing is left to displace): a SECOND stack
//       for a's identity dials b from 127.0.0.2 while a's first connection is still live and older than the supersede window.
//       Pinned wire: b supersedes exactly once and delivery resumes over the new connection.
//       CONTROL: on the HELLO wire with strict address binding the same dial is REFUSED (no supersede, the old session keeps working),
//       i.e. the new address really is what b sees and the binding is what the pinned wire removes.
// Implements 021 §"HELLO and dial dedup in pinned mode"; ADR-071 §5/§10 (reconnect supersedes), ADR-072 item 4.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "pinned_tcp_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster = kPinnedTcpCluster;
constexpr std::uint64_t kAddr2 = pal::ipv4(127, 0, 0, 2);
constexpr int kN = 50;

// Send `kN` distinct payloads "<tag><i>" tx -> rx and report whether every one arrived exactly once. Frames are matched by payload.
bool exchange(Stack& tx, Stack& rx, NodeId to, const std::string& tag) {
    std::size_t base = 0;
    {
        std::lock_guard g(rx.mu);
        base = rx.frames.size();
    }
    for (int i = 0; i < kN; ++i) {
        tx.sec.send(to, data(tx.n.id, to, tag + std::to_string(i)));
        sleep_ms(2);
    }
    wait_for([&] {
        std::lock_guard g(rx.mu);
        std::set<std::string> seen;
        for (std::size_t i = base; i < rx.frames.size(); ++i)
            seen.insert(std::string(reinterpret_cast<const char*>(rx.frames[i].payload.data()), rx.frames[i].payload.size()));
        return seen.size() >= static_cast<std::size_t>(kN);
    }, 5000);
    std::lock_guard g(rx.mu);
    std::set<std::string> seen;
    std::size_t total = 0;
    for (std::size_t i = base; i < rx.frames.size(); ++i) {
        const auto& p = rx.frames[i].payload;
        if (std::string_view(reinterpret_cast<const char*>(p.data()), p.size()).rfind(tag, 0) != 0) continue;
        ++total;
        seen.insert(std::string(reinterpret_cast<const char*>(p.data()), p.size()));
    }
    return seen.size() == static_cast<std::size_t>(kN) && total == static_cast<std::size_t>(kN);  // all, and no duplicate
}
}  // namespace

int main() {
    bool ok = true;
    if (!adapters::noise::init()) return 1;
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 2; ++i) nodes.push_back(std::make_unique<Node>());
    std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
    Node& a = *nodes[0];  // dialer (lower id)
    Node& b = *nodes[1];  // responder
    pair_up(a, b, kCluster);

    // ---- R1: reset and redial from a new source IP (pinned wire) ----
    {
        Stack sb(b, true);
        check(sb.start(), "R1: responder starts", ok);
        Stack sa(a, true);
        check(sa.start(), "R1: dialer starts", ok);
        sa.dial_via(b.id, sb.tcp.listen_port());
        check(deliver(sa, sb, b.id, 1, "up"), "R1: the first session comes up from 127.0.0.1", ok);
        check(exchange(sa, sb, b.id, "A1-"), "R1: phase 1 a->b 50/50, no duplicate", ok);
        check(exchange(sb, sa, a.id, "B1-"), "R1: phase 1 b->a 50/50, no duplicate", ok);
        const std::uint64_t sessions_before = sb.sec.session_count();
        sa.tcp.set_dial_source_addr(kAddr2);
        sa.tcp.reset_peer_connection(b.id);
        const auto t0 = std::chrono::steady_clock::now();
        const bool up = deliver(sa, sb, b.id, 1, "again", 5000);
        const auto took = std::chrono::steady_clock::now() - t0;
        check(up && took < std::chrono::seconds(5), "R1: no lock-out: the session from 127.0.0.2 is up within 5 s", ok);
        check(sb.sec.session_count() == sessions_before, "R1: b still holds exactly one session for a", ok);
        check(exchange(sa, sb, b.id, "A2-"), "R1: phase 2 a->b 50/50, no duplicate", ok);
        check(exchange(sb, sa, a.id, "B2-"), "R1: phase 2 b->a 50/50, no duplicate", ok);
    }

    // ---- R2 (pinned) and its CONTROL (HELLO wire + strict binding): a second connection from a new IP while the old one is live ----
    for (const bool pinned : {true, false}) {
        const char* wire = pinned ? "pinned" : "HELLO";
        Stack sb(b, pinned);
        sb.tcp.set_strict_peer_addr_binding(true);
        check(sb.start(), "R2: responder starts", ok);
        auto sa = std::make_unique<Stack>(a, pinned);
        check(sa->start(), "R2: first dialer starts", ok);
        sa->dial_via(b.id, sb.tcp.listen_port());
        check(deliver(*sa, sb, b.id, 2, "live"), (std::string("R2 ") + wire + ": a's first connection is live").c_str(), ok);
        sleep_ms(1300);  // older than the supersede window (1 s): a replacement now counts as a reconnect
        const std::uint64_t superseded_before = sb.tcp.reconnects_superseded();

        Stack sa2(a, pinned);  // the same identity, now on another network
        sa2.tcp.set_dial_source_addr(kAddr2);
        check(sa2.start(), "R2: second dialer starts", ok);
        sa2.dial_via(b.id, sb.tcp.listen_port());
        for (int i = 0; i < 100; ++i) {  // a dialer only connects once it has something to send
            sa2.sec.send(b.id, data(a.id, b.id, "probe"));
            sleep_ms(15);
        }
        const std::uint64_t delta = sb.tcp.reconnects_superseded() - superseded_before;
        if (pinned) {
            check(delta == 1, "R2: on the pinned wire b supersedes exactly once", ok);
            sa.reset();  // the old stack must not redial from 127.0.0.1 and fight over the slot
            check(exchange(sa2, sb, b.id, "A3-"), "R2: delivery a2->b resumes over the new connection, 50/50, no duplicate", ok);
            check(exchange(sb, sa2, a.id, "B3-"), "R2: delivery b->a2 resumes over the new connection, 50/50, no duplicate", ok);
        } else {
            check(delta == 0, "R2 CONTROL: on the HELLO wire with strict binding the new address is refused (no supersede)", ok);
            check(deliver(*sa, sb, b.id, 2, "still the old one"), "R2 CONTROL: the old connection keeps working", ok);
            check(sa2.sec.session_count() == 0, "R2 CONTROL: the connection from the new address got no session", ok);
        }
    }

    std::printf(ok ? "pinned_reconnect_newip_check: OK\n" : "pinned_reconnect_newip_check: FAILED\n");
    return ok ? 0 : 1;
}
