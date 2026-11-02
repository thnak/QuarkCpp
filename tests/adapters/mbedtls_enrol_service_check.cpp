// Conformance check for ADR-074 D4 / issue #36 (D4c step 4): enrolment over a REAL TcpTransport listener and real sockets. The transport hands a
// connection that starts with a TLS record header to `EnrolService` (its own worker thread, mbedtls_enrol_service.hpp); everything else stays on the member
// path. Real crypto, real `Enrollment`, a simulated custodian (see enrol_test_pki.hpp).
//
//   V1  a joiner enrols over TCP on the member listener; the socket left the member pending pool (it never counts against max_pending_inbound); a second use of the
//       code is Refused
//   V2  while no ticket exists the connection is closed with ZERO bytes sent, and nothing is handed off (CONTROL: with a ticket the same bytes enrol)
//   V3  caps: `max_concurrent` and `max_per_ip` refuse the surplus (closed, zero bytes), the admitted ones are untouched, and a freed slot is reusable
//   V4  selectivity: bytes that are not a TLS record header never reach the service (a bad HELLO still dies as before); an enrol client does not disturb a
//       member HELLO connection
//   V5  a flood of enrol hellos does not stop a member connection from delivering frames (the crypto is off the transport's I/O thread)
//   V6  an enrolment that stalls is closed at the service's deadline and frees its slot
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "adapters/enrol_test_pki.hpp"
#include "quark/adapters/mbedtls/mbedtls_enrol_service.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

namespace {
using namespace enrol_test;
using quark::test::wait_until;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

enrol::Ticket new_ticket(World& w, enrol::Role role = enrol::Role::Member) {
    auto t = w.enr->issue_ticket(role, 60'000'000'000, w.clock.load());
    if (!t) {
        std::fprintf(stderr, "ticket failed\n");
        std::exit(2);
    }
    return *t;
}

// A raw client socket with a deadline-bounded recv. Sends/receives on the loopback.
struct Client {
    pal::fd_t fd = pal::invalid_fd;
    explicit Client(std::uint16_t port) {
        auto r = pal::tcp_connect(pal::ipv4_loopback, port);
        if (r) fd = *r;
    }
    ~Client() {
        if (fd != pal::invalid_fd) pal::close_fd(fd);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool send_all(const Bytes& b) {
        std::size_t off = 0;
        for (int spins = 0; off < b.size() && spins < 5000; ++spins) {
            auto r = pal::send_some(fd, b.data() + off, b.size() - off);
            if (r) {
                off += *r;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // connect still completing / would_block
        }
        return off == b.size();
    }
    // Waits up to `ms` for data or EOF. Returns what arrived in `got`; `eof` is set on an orderly close or a reset.
    void recv_for(int ms, Bytes& got, bool& eof) {
        eof = false;
        std::array<std::byte, 8192> buf;
        for (int i = 0; i < ms; ++i) {
            auto r = pal::recv_some(fd, buf.data(), buf.size());
            if (r && *r > 0) {
                got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*r));
                return;
            }
            if (r && *r == 0) {
                eof = true;
                return;
            }
            if (!r && r.error() != pal::would_block()) {
                eof = true;  // reset
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

// Runs a joiner session over a socket to completion (or until `ms` elapse).
EnrolJoinerSession::State drive(EnrolJoinerSession& j, Client& c, int ms) {
    Bytes out;
    j.start(out);
    for (int i = 0; i < ms && !(j.state() == EnrolJoinerSession::State::Joined || j.state() == EnrolJoinerSession::State::Rejected ||
                                 j.state() == EnrolJoinerSession::State::Failed);
         ++i) {
        if (!out.empty()) {
            if (!c.send_all(out)) break;
            out.clear();
        }
        Bytes got;
        bool eof = false;
        c.recv_for(20, got, eof);
        if (!got.empty()) j.feed(got, out);
        if (eof && got.empty()) break;
    }
    return j.state();
}

// The anchor: a member-mode TcpTransport whose enrol handoff feeds an EnrolService.
struct Anchor {
    World w;
    net::TcpTransport t{NodeId{kAnchor}, pal::ipv4_loopback, 0};
    std::unique_ptr<EnrolService> svc;
    explicit Anchor(EnrolServiceConfig cfg = {}, bool pinned = false) {
        t.set_pinned_wire(pinned);  // #45 pinned wire: no HELLO either way, the first bytes are a u32 LE frame length
        svc = std::make_unique<EnrolService>(w.anchor_id, *w.enr, cfg, [this] { return w.clock.load(); });
        t.set_enrol_handoff([this](net::TcpTransport::EnrolHandoff& h) { return svc->handoff(h.fd, h.peer_addr, std::move(h.initial)); });
        t.on_receive([this](MessageFrame f) { rec(std::move(f)); });
        if (!t.start()) {
            std::fprintf(stderr, "transport start failed\n");
            std::exit(2);
        }
    }
    quark::test::Recorder rec;
};

}  // namespace

int main() {
    using JS = EnrolJoinerSession::State;

    // ---------------------------------------------------------------------------------------------- V1
    {
        Anchor a;
        const auto t = new_ticket(a.w);
        (void)new_ticket(a.w);  // a spare, so a live ticket still exists after the first is consumed (otherwise the hello is refused outright, see V2)
        EnrolJoinerSession j(a.w.code(t));
        Client c(a.t.listen_port());
        check(drive(j, c, 5000) == JS::Joined, "V1 the joiner enrols over TCP on the member listener");
        check(a.t.enrol_handoffs() == 1, "V1 exactly one connection was handed off");
        check(wait_until([&] { return a.svc->replied() == 1 && a.svc->active() == 0; }), "V1 the service answered and released its slot");
        check(a.t.pending_inbound_count() == 0, "V1 the member pending pool holds nothing");
        auto id = j.take_identity();
        check(id && id->cluster.value == kCluster, "V1 the joiner holds a cluster identity");

        EnrolJoinerSession j2(a.w.code(t));
        Client c2(a.t.listen_port());
        check(drive(j2, c2, 5000) == JS::Rejected && j2.rejection() == enrol::WireStatus::Refused, "V1 a second use of the join code is Refused");
    }

    // ---------------------------------------------------------------------------------------------- V2
    {
        Anchor a;
        EnrolJoinerSession probe(a.w.code(enrol::Ticket{}));  // no ticket exists on the anchor
        Client c(a.t.listen_port());
        Bytes out;
        probe.start(out);
        check(c.send_all(out), "V2 setup: a ClientHello is sent");
        Bytes got;
        bool eof = false;
        c.recv_for(3000, got, eof);
        check(eof && got.empty(), "V2 with no live ticket the connection is closed and ZERO bytes were sent");
        check(a.svc->refused_no_ticket() == 1 && a.t.enrol_handoffs() == 0, "V2 counted as a refused hello, not a handoff");
        check(wait_until([&] { return a.t.pending_inbound_count() == 0; }), "V2 and it left the pending pool");

        const auto t = new_ticket(a.w);  // CONTROL: the same path with a live ticket
        EnrolJoinerSession j(a.w.code(t));
        Client c2(a.t.listen_port());
        check(drive(j, c2, 5000) == JS::Joined, "V2 CONTROL: with a ticket the same client enrols");
    }

    // ---------------------------------------------------------------------------------------------- V3
    {
        EnrolServiceConfig cfg;
        cfg.max_concurrent = 2;
        cfg.max_per_ip = 8;
        Anchor a(cfg);
        (void)new_ticket(a.w);
        std::vector<std::unique_ptr<Client>> held;
        std::vector<std::unique_ptr<EnrolJoinerSession>> js;
        const auto t = new_ticket(a.w);
        for (int i = 0; i < 2; ++i) {  // two enrolments that start and then stall (ClientHello only)
            js.push_back(std::make_unique<EnrolJoinerSession>(a.w.code(t)));
            held.push_back(std::make_unique<Client>(a.t.listen_port()));
            Bytes out;
            js.back()->start(out);
            check(held.back()->send_all(out), "V3 setup: ClientHello sent");
        }
        check(wait_until([&] { return a.svc->active() == 2; }), "V3 two enrolments are running");
        Client extra(a.t.listen_port());
        EnrolJoinerSession je(a.w.code(t));
        Bytes out;
        je.start(out);
        (void)extra.send_all(out);
        Bytes got;
        bool eof = false;
        extra.recv_for(3000, got, eof);
        check(eof && got.empty() && a.svc->refused_caps() == 1, "V3 the third is closed with zero bytes (max_concurrent)");
        check(a.svc->active() == 2, "V3 the two admitted ones are untouched");
        // Free a slot: close one held client; the service notices EOF and releases.
        held[0].reset();
        check(wait_until([&] { return a.svc->active() == 1; }), "V3 a closed enrolment frees its slot");
        EnrolJoinerSession j4(a.w.code(t));
        Client c4(a.t.listen_port());
        check(drive(j4, c4, 5000) == JS::Joined, "V3 CONTROL: the freed slot admits a genuine joiner");
    }
    {
        EnrolServiceConfig cfg;
        cfg.max_concurrent = 8;
        cfg.max_per_ip = 1;
        Anchor a(cfg);
        const auto t = new_ticket(a.w);
        EnrolJoinerSession j1(a.w.code(t));
        Client c1(a.t.listen_port());
        Bytes out;
        j1.start(out);
        check(c1.send_all(out), "V3b setup");
        check(wait_until([&] { return a.svc->active() == 1; }), "V3b one enrolment from this address");
        EnrolJoinerSession j2(a.w.code(t));
        Client c2(a.t.listen_port());
        out.clear();
        j2.start(out);
        (void)c2.send_all(out);
        Bytes got;
        bool eof = false;
        c2.recv_for(3000, got, eof);
        check(eof && got.empty() && a.svc->refused_caps() == 1, "V3b a second from the same address is refused (max_per_ip)");
    }

    // ---------------------------------------------------------------------------------------------- V4
    {
        Anchor a;
        (void)new_ticket(a.w);
        Client junk(a.t.listen_port());
        const char* http = "GET / HTTP/1.1\r\n\r\n";
        Bytes b;
        for (const char* p = http; *p; ++p) b.push_back(static_cast<std::byte>(*p));
        check(junk.send_all(b), "V4 setup: junk sent");
        Bytes got;
        bool eof = false;
        junk.recv_for(3000, got, eof);
        check(a.t.enrol_handoffs() == 0 && a.svc->refused_no_ticket() == 0 && a.svc->refused_caps() == 0, "V4 non-TLS bytes never reach the service");
        // A member HELLO (magic, version, node): the held acceptor hello must come out once the first chunk is classified as a member's.
        {
            Client member(a.t.listen_port());
            Bytes hello;
            for (std::uint8_t byte : {0x31, 0x4B, 0x52, 0x51, 0x02, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0}) hello.push_back(static_cast<std::byte>(byte));
            check(member.send_all(hello), "V4 setup: a member HELLO is sent");
            Bytes got_hello;
            bool hello_eof = false;
            member.recv_for(3000, got_hello, hello_eof);
            check(got_hello.size() >= net::hello_size && got_hello[0] == std::byte{0x31}, "V4 a member still receives the acceptor HELLO (held until classified, then sent; a DialHint may follow it)");
            check(a.t.enrol_handoffs() == 0, "V4 and a member HELLO is not handed off");
        }
        // TLS-looking but only 2 bytes: not recognised (documented), so it is a bad HELLO and dies on the member path.
        Client two(a.t.listen_port());
        check(two.send_all(Bytes{std::byte{0x16}, std::byte{0x03}}), "V4 setup: two bytes");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(a.t.enrol_handoffs() == 0, "V4 a first chunk shorter than 3 bytes is not handed off");
    }

    // ---------------------------------------------------------------------------------------------- V5
    {
        EnrolServiceConfig cfg;
        cfg.max_concurrent = 2;
        cfg.max_per_ip = 100;
        Anchor a(cfg);
        net::TcpTransport member(NodeId{0x77}, pal::ipv4_loopback, 0);
        quark::test::Recorder mrec;
        member.on_receive([&](MessageFrame f) { mrec(std::move(f)); });
        check(member.start(), "V5 setup: the member transport starts");
        member.add_peer(quark::test::loopback_endpoint(NodeId{kAnchor}, a.t.listen_port()));
        a.t.add_peer(quark::test::loopback_endpoint(NodeId{0x77}, member.listen_port()));
        const auto t = new_ticket(a.w);
        std::vector<std::unique_ptr<Client>> flood;
        for (int i = 0; i < 40; ++i) {  // 2 admitted and stalling, 38 refused, all starting the TLS handshake
            EnrolJoinerSession js(a.w.code(t));
            flood.push_back(std::make_unique<Client>(a.t.listen_port()));
            Bytes out;
            js.start(out);
            (void)flood.back()->send_all(out);
        }
        member.send(NodeId{kAnchor}, quark::test::seq_frame(NodeId{0x77}, NodeId{kAnchor}, 1));
        check(wait_until([&] { return a.rec.count() >= 1; }, 5000), "V5 a member frame is delivered while the enrol flood is in progress");
        a.t.send(NodeId{0x77}, quark::test::seq_frame(NodeId{kAnchor}, NodeId{0x77}, 2));
        check(wait_until([&] { return mrec.count() >= 1; }, 5000), "V5 and the anchor can still send to the member");
        check(a.svc->refused_caps() >= 30, "V5 the surplus enrol hellos were refused by the caps");
        check(a.t.pending_inbound_count() == 0 || a.t.pending_inbound_count() < 5, "V5 the enrol connections did not pile up in the member pending pool");
    }

    // V5b: work that BLOCKS in the enrolment path (here the custodian, called from Enrollment::submit on the service's worker) does not delay the transport.
    {
        Anchor a;
        std::atomic<bool> in_custodian{false};
        a.w.custodian_override = [&](enrol::IssueRequest req, enrol::Enrollment::Completion done) {
            in_custodian = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            done(a.w.sign(req));
        };
        net::TcpTransport member(NodeId{0x78}, pal::ipv4_loopback, 0);
        member.set_allow_insecure_plaintext(true);
        check(member.start(), "V5b setup");
        member.add_peer(quark::test::loopback_endpoint(NodeId{kAnchor}, a.t.listen_port()));
        const auto t = new_ticket(a.w);
        EnrolJoinerSession j(a.w.code(t));
        Client c(a.t.listen_port());
        std::thread th([&] { (void)drive(j, c, 8000); });
        check(wait_until([&] { return in_custodian.load(); }, 5000), "V5b the enrolment reached the blocking custodian");
        const auto t0 = std::chrono::steady_clock::now();
        member.send(NodeId{kAnchor}, quark::test::seq_frame(NodeId{0x78}, NodeId{kAnchor}, 9));
        check(wait_until([&] { return a.rec.count() >= 1; }, 500), "V5b a member frame arrives within 500 ms while the custodian blocks for 1.5 s");
        check(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000), "V5b (and well before the custodian returns)");
        th.join();
        check(j.state() == JS::Joined, "V5b the slow enrolment still completes");
    }

    // ---------------------------------------------------------------------------------------------- V6
    {
        EnrolServiceConfig cfg;
        cfg.deadline_ns = 300'000'000;
        Anchor a(cfg);
        const auto t = new_ticket(a.w);
        EnrolJoinerSession j(a.w.code(t));
        Client c(a.t.listen_port());
        Bytes out;
        j.start(out);
        check(c.send_all(out), "V6 setup");
        check(wait_until([&] { return a.svc->active() == 1; }), "V6 the enrolment is running");
        check(wait_until([&] { return a.svc->timed_out() == 1 && a.svc->active() == 0; }, 5000), "V6 a stalled enrolment is closed at the deadline and frees its slot");
    }

    // ---------------------------------------------------------------------------------------------- V7
    // The PINNED member wire (no HELLO: the first bytes are a u32 LE frame length). The discrimination rests on the length bytes of an opener never being
    // `16 03 01|03 ..` (ADR-074 D4c). Until now that was argued in the ADR only.
    {
        Anchor a({}, /*pinned=*/true);
        check(a.t.pinned_wire(), "V7 setup: the listener runs the pinned wire");
        (void)new_ticket(a.w);  // a spare, so a live ticket survives the first enrolment
        const auto t = new_ticket(a.w);
        EnrolJoinerSession j(a.w.code(t));
        Client c(a.t.listen_port());
        check(drive(j, c, 5000) == JS::Joined, "V7 a joiner enrols on a PINNED-wire listener");
        check(a.t.enrol_handoffs() == 1, "V7 handed off exactly once");
        check(wait_until([&] { return a.svc->replied() == 1 && a.svc->active() == 0; }), "V7 the service answered and released its slot");
        check(wait_until([&] { return a.t.pending_inbound_count() == 0; }), "V7 the pinned pending pool holds nothing");

        // Selectivity: a length-prefixed opener-shaped chunk (length 32 = 20 00 00 00) is NOT a TLS record header, so it stays on the member path.
        Client opener(a.t.listen_port());
        Bytes fake{std::byte{0x20}, std::byte{0}, std::byte{0}, std::byte{0}};
        for (int i = 0; i < 32; ++i) fake.push_back(std::byte{0xAB});
        check(opener.send_all(fake), "V7 setup: a pinned-framed opener-shaped chunk is sent");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(a.t.enrol_handoffs() == 1, "V7 a pinned-framed chunk is not handed off");

        // The pinned wire sends nothing on accept (no held HELLO to release): an enrol client sees nothing before its own ClientHello.
        Client quiet(a.t.listen_port());
        Bytes got;
        bool eof = false;
        quiet.recv_for(200, got, eof);
        check(got.empty() && !eof, "V7 the pinned acceptor sends nothing before the peer speaks");
    }
    // V7b: with NO live ticket a pinned-wire enrol hello is closed with zero bytes, as on the HELLO wire (V2).
    {
        Anchor a({}, /*pinned=*/true);
        EnrolJoinerSession probe(a.w.code(enrol::Ticket{}));
        Client c(a.t.listen_port());
        Bytes out;
        probe.start(out);
        check(c.send_all(out), "V7b setup");
        Bytes got;
        bool eof = false;
        c.recv_for(3000, got, eof);
        check(eof && got.empty() && a.svc->refused_no_ticket() == 1, "V7b no live ticket: closed with zero bytes on the pinned wire");
    }

    std::printf(g_ok ? "mbedtls_enrol_service_check: all checks passed\n" : "mbedtls_enrol_service_check: FAILED\n");
    return g_ok ? 0 : 1;
}
