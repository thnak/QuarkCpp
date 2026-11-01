// Issue #45 (ADR-068 follow-up, ADR-072): the pinned-key path over the REAL `TcpTransport` + `SecureTransport` + Noise IK
// engine: no HELLO, the peer identified by the handshake, no NodeId in any frame header. The privacy checks P1-P4 of
// pinned_privacy_check, rerun against the shipped transport (that file used a test-only responder), plus the HELLO-claim
// half of #46. Each property has a control that must fail the other way.
//   T1  end to end: a (lower id, dials) and b (higher id, never dials back) exchange data both ways on a pinned wire.
//   T2  P4 capture through a recording TCP proxy: neither NodeId, no key and no cluster id appears anywhere in either
//       direction, handshake or data. CONTROL: the same session with the pinned wire OFF (HELLO + header ids) shows both.
//   T3  P1 prober: a silent connection gets ZERO bytes (no HELLO), random bytes / a wrong-key Msg1 / a replayed HELLO get
//       zero bytes back and the connection is closed. CONTROL: a transport without the pinned wire sends its HELLO first.
//   T4  P2 un-admitted initiator over real TCP: no session on either side, not one byte back to it. CONTROL: admit it, it
//       completes.
//   T5  #46 / N1: an attacker who sends a HELLO claiming a's NodeId cannot displace a's live connection on a pinned wire.
//       CONTROL: without the pinned wire the same claim supersedes it (reconnects_superseded() == 1).
//   T6  an authenticated reconnect (a's connection is reset) supersedes b's old connection and the session swaps in place:
//       delivery resumes with no failure.
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" as amended by ADR-072, 021 §"HELLO and dial dedup in pinned mode".
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "pinned_test_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster{777};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F>
bool wait_for(F f, int ms = 5000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f()) return true;
        sleep_ms(5);
    }
    return f();
}

// Recording TCP proxy: relays every accepted connection to `target_port`, keeps all bytes seen in each direction.
class Proxy {
public:
    explicit Proxy(std::uint16_t target_port) : target_(target_port) {
        lfd_ = *pal::tcp_listen(pal::ipv4_loopback, 0);
        port_ = *pal::local_port(lfd_);
        th_ = std::thread([this] { loop(); });
    }
    ~Proxy() {
        stop_.store(true);
        th_.join();
        pal::close_fd(lfd_);
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] Bytes c2s() const {
        std::lock_guard g(mu_);
        return c2s_;
    }
    [[nodiscard]] Bytes s2c() const {
        std::lock_guard g(mu_);
        return s2c_;
    }

private:
    struct Pair { pal::fd_t c, s; };
    void loop() {
        std::vector<Pair> ps;
        while (!stop_.load()) {
            bool idle = true;
            if (auto a = pal::accept_one(lfd_)) {
                if (auto s = pal::tcp_connect(pal::ipv4_loopback, target_)) ps.push_back({*a, *s});
                else pal::close_fd(*a);
                idle = false;
            }
            for (std::size_t i = 0; i < ps.size();) {
                bool drop = false;
                for (int dir = 0; dir < 2 && !drop; ++dir) {
                    const pal::fd_t from = dir == 0 ? ps[i].c : ps[i].s, to = dir == 0 ? ps[i].s : ps[i].c;
                    std::byte buf[2048];
                    auto r = pal::recv_some(from, buf, sizeof(buf));
                    if (r && *r == 0) { drop = true; break; }
                    if (r) {
                        idle = false;
                        { std::lock_guard g(mu_); (dir == 0 ? c2s_ : s2c_).insert((dir == 0 ? c2s_ : s2c_).end(), buf, buf + *r); }
                        for (std::size_t off = 0; off < *r;) {
                            auto w = pal::send_some(to, buf + off, *r - off);
                            if (w) off += *w; else sleep_ms(1);
                        }
                    } else if (r.error() != pal::would_block()) {
                        drop = true;
                    }
                }
                if (drop) {
                    pal::close_fd(ps[i].c);
                    pal::close_fd(ps[i].s);
                    ps.erase(ps.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            if (idle) sleep_ms(1);
        }
        for (auto& p : ps) { pal::close_fd(p.c); pal::close_fd(p.s); }
    }
    std::uint16_t target_;
    pal::fd_t lfd_{};
    std::uint16_t port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    Bytes c2s_, s2c_;
};

// One node: TcpTransport (pinned wire or not) under a SecureTransport driving the pinned engine.
struct Stack {
    Node& n;
    net::TcpTransport tcp;
    MockCipher unused{0};
    SecureTransport sec;
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>();
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>();
    IdentityMaterial identity{id_blob};
    TrustStore trust{trust_blob};
    std::atomic<std::uint64_t> got{0};
    std::mutex mu;
    std::vector<MessageFrame> frames;

    Stack(Node& node, bool pinned_wire)
        : n(node), tcp(node.id, pal::ipv4_loopback, 0), sec(tcp, unused, node.id) {
        tcp.set_pinned_wire(pinned_wire);
        sec.enable_handshake(*n.factory, kCluster, identity, trust);
        sec.on_receive([this](MessageFrame f) {
            std::lock_guard g(mu);
            frames.push_back(std::move(f));
            got.fetch_add(1);
        });
        tcp.set_peer_down_hook([this](NodeId p) { sec.on_peer_disconnected(p); });
        sec.set_reset_hook([this](NodeId p) { tcp.reset_peer_connection(p); });
    }
    bool start() { return tcp.start(); }
    ~Stack() { tcp.stop(); }
    void dial_via(NodeId peer, std::uint16_t port) { tcp.add_peer(loopback_endpoint(peer, port)); }
};

MessageFrame data(NodeId from, NodeId to, std::string_view text) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Data;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    for (char c : text) f.payload.push_back(static_cast<std::byte>(c));
    return f;
}

// Send until `want` frames arrived at `rx` (frames sent before the session exists are dropped by design, S2).
bool deliver(Stack& tx, Stack& rx, NodeId to, std::uint64_t want, std::string_view text = "payload") {
    const std::uint64_t base = rx.got.load();
    return wait_for([&] {
        if (rx.got.load() >= base + want) return true;
        tx.sec.send(to, data(tx.n.id, to, text));
        sleep_ms(15);
        return false;
    });
}

struct Capture {
    bool delivered = false;
    Bytes c2s, s2c;
};
Capture session(Node& a, Node& b, bool pinned_wire) {
    Capture cap;
    Stack sb(b, pinned_wire);
    if (!sb.start()) return cap;
    Proxy proxy(sb.tcp.listen_port());
    Stack sa(a, pinned_wire);
    if (!sa.start()) return cap;
    sa.dial_via(b.id, proxy.port());
    cap.delivered = deliver(sa, sb, b.id, 3, "secret payload 12345");
    // and back (b has no endpoint for a: the reply must ride the inbound connection)
    cap.delivered = cap.delivered && deliver(sb, sa, a.id, 1, "reply 67890");
    sleep_ms(100);
    cap.c2s = proxy.c2s();
    cap.s2c = proxy.s2c();
    return cap;
}
}  // namespace

int main() {
    bool ok = true;
    if (!adapters::noise::init()) return 1;
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 3; ++i) nodes.push_back(std::make_unique<Node>());
    std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
    Node& a = *nodes[0];  // admitted initiator (lowest id)
    Node& c = *nodes[1];  // knows b, never admitted by b
    Node& b = *nodes[2];  // responder (highest id)
    pair_up(a, b, kCluster);
    (void)c.book->admit(kCluster, b.pub);

    // ---- T1 + T2: end to end and capture ----
    {
        const Capture cap = session(a, b, /*pinned_wire=*/true);
        check(cap.delivered, "T1: data flows a->b and b->a over the pinned wire (b never dials)", ok);
        const Bytes ida = le64(a.id.value), idb = le64(b.id.value), cl = le64(kCluster.value);
        Bytes all = cap.c2s;
        all.insert(all.end(), cap.s2c.begin(), cap.s2c.end());
        check(!all.empty(), "T2: the proxy saw traffic", ok);
        check(!contains(all, ida) && !contains(all, idb), "T2: neither NodeId appears anywhere on the wire (handshake or data)", ok);
        check(!contains(all, cl), "T2: the cluster/scope id does not appear on the wire", ok);
        check(!contains(all, a.pub) && !contains(all, b.pub), "T2: no public key appears on the wire", ok);
        // CONTROL: the same session without the pinned wire discloses both ids (the search is not vacuous).
        const Capture legacy = session(a, b, /*pinned_wire=*/false);
        Bytes lall = legacy.c2s;
        lall.insert(lall.end(), legacy.s2c.begin(), legacy.s2c.end());
        check(legacy.delivered, "T2 CONTROL: the same session over the HELLO wire works", ok);
        check(contains(lall, ida) && contains(lall, idb), "T2 CONTROL: with the HELLO wire both NodeIds are visible in cleartext", ok);
    }

    // ---- T3: prober ----
    {
        Stack sb(b, true);
        check(sb.start(), "T3: responder starts", ok);
        const std::uint16_t port = sb.tcp.listen_port();
        const ProbeResult silent = probe(port, {}, 600);
        check(silent.connected && silent.received.empty(), "T3 P1: a silent prober receives ZERO bytes (no HELLO)", ok);
        Bytes junk(154);
        for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::byte>(i * 37 + 11);
        const ProbeResult rnd = probe(port, junk, 600);
        check(rnd.received.empty() && rnd.closed_by_peer, "T3 P1: random bytes get zero bytes back and a close", ok);
        // A well-formed frame carrying a wrong-key Msg1: the engine refuses silently.
        MessageFrame f{};
        f.kind = FrameKind::Authenticate;
        f.payload.assign(kPinnedMsg1Len, std::byte{0x5A});
        const Bytes wire = net::encode_frame(f);
        const ProbeResult wk = probe(port, wire, 600);
        check(wk.received.empty() && wk.closed_by_peer, "T3 P1: a wrong-key Msg1 frame gets zero bytes back and a close", ok);
        // A legacy HELLO claiming a's NodeId is just garbage here.
        net::TcpTransport legacy_node(a.id, pal::ipv4_loopback, 0);
        legacy_node.set_authenticated_transport(true);
        (void)legacy_node.start();
        const ProbeResult hello = probe(legacy_node.listen_port(), {}, 600, net::hello_size);
        legacy_node.stop();
        check(hello.received.size() == net::hello_size, "T3 CONTROL: a transport without the pinned wire sends a HELLO to a silent prober", ok);
        const ProbeResult claim = probe(port, hello.received, 600);
        check(claim.received.empty() && claim.closed_by_peer, "T3: a legacy HELLO claiming a NodeId is refused with zero bytes", ok);
        check(sb.sec.session_count() == 0, "T3: none of it created a session", ok);
    }

    // ---- T4: un-admitted initiator ----
    {
        Stack sb(b, true);
        (void)sb.start();
        Proxy proxy(sb.tcp.listen_port());
        Stack sc(c, true);
        (void)sc.start();
        sc.dial_via(b.id, proxy.port());
        for (int i = 0; i < 30; ++i) {
            sc.sec.send(b.id, data(c.id, b.id, "let me in"));
            sleep_ms(20);
        }
        check(sb.sec.session_count() == 0 && sc.sec.session_count() == 0, "T4: an un-admitted initiator gets no session on either side", ok);
        check(proxy.s2c().empty(), "T4 P2: not one byte went back toward the un-admitted initiator", ok);
        check(sb.got.load() == 0, "T4: nothing was delivered", ok);
        // CONTROL: admit it and it completes (needs the handshake timeout to pass for c's parked engine: use a fresh stack).
        (void)b.book->admit(kCluster, c.pub);
        Stack sc2(c, true);
        (void)sc2.start();
        sc2.dial_via(b.id, proxy.port());
        check(deliver(sc2, sb, b.id, 1, "now admitted"), "T4 CONTROL: once admitted the same initiator is served", ok);
        (void)b.book->revoke(kCluster, c.pub);
    }

    // ---- T5 / T6: displacement and authenticated reconnect ----
    for (const bool pinned : {true, false}) {
        Stack sb(b, pinned);
        (void)sb.start();
        Stack sa(a, pinned);
        (void)sa.start();
        sa.dial_via(b.id, sb.tcp.listen_port());
        check(deliver(sa, sb, b.id, 2), "T5: a's session is live", ok);
        const std::uint64_t down_before = sb.sec.sessions_dropped_on_disconnect();
        const std::uint64_t superseded_before = sb.tcp.reconnects_superseded();
        // The attacker claims a's NodeId in a legacy HELLO (the only identity claim there ever was) and then sits there.
        net::TcpTransport legacy_node(a.id, pal::ipv4_loopback, 0);
        legacy_node.set_authenticated_transport(true);
        (void)legacy_node.start();
        const ProbeResult hello = probe(legacy_node.listen_port(), {}, 600, net::hello_size);
        legacy_node.stop();
        sleep_ms(1200);  // past the reconnect-supersede window (an aged connection is what a claim could replace)
        const auto fd = pal::tcp_connect(pal::ipv4_loopback, sb.tcp.listen_port());
        if (fd && hello.received.size() == net::hello_size) {
            std::size_t off = 0;
            while (off < hello.received.size()) {
                auto w = pal::send_some(*fd, hello.received.data() + off, hello.received.size() - off);
                if (w) off += *w; else sleep_ms(1);
            }
            sleep_ms(300);
        }
        const bool displaced = sb.tcp.reconnects_superseded() > superseded_before;
        if (fd) pal::close_fd(*fd);
        if (pinned) {
            check(!displaced, "T5: a HELLO claim does not displace a's live connection on the pinned wire", ok);
            check(sb.sec.sessions_dropped_on_disconnect() == down_before, "T5: a's session was not dropped", ok);
            check(deliver(sa, sb, b.id, 2, "after the claim"), "T5: a's delivery continues after the claim", ok);
            // T6: authenticated reconnect.
            const std::uint64_t before_sessions = sb.sec.session_count();
            sa.tcp.reset_peer_connection(b.id);
            check(deliver(sa, sb, b.id, 3, "after reconnect"), "T6: after a's connection is reset the session is re-established and delivery resumes", ok);
            check(sb.sec.session_count() == before_sessions, "T6: b still holds exactly one session for a", ok);
        } else {
            check(displaced, "T5 CONTROL: without the pinned wire the same HELLO claim supersedes the live connection", ok);
        }
    }

    std::printf(ok ? "pinned_tcp_check: OK\n" : "pinned_tcp_check: FAILED\n");
    return ok ? 0 : 1;
}
