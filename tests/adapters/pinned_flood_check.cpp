// Issue #49 (SEC-07 re-run on the pinned wire; ADR-068/072, F13): a per-IP pre-auth flood against a pinned listener.
// 3000 anonymous connections (half silent, half carrying a wrong-key Noise Msg1) are opened against one responder.
//   F1  flood from ONE foreign source IP (127.0.0.2, Linux): the pending set stays at or under the per-IP cap, and an
//       admitted peer (127.0.0.1) completes its handshake and delivers data DURING the flood.
//   F2  CONTROL: the per-IP cap switched off (`set_max_pending_inbound_per_ip(0)`; the setter is the repo's switch for
//       it, so no extra TestFaults member): the flood fills the GLOBAL pending cap (1024) and the admitted peer is NOT
//       served inside the flood window.
//   F3  the SAME IP as the admitted peer (documents a difference from the mTLS era): the pinned wire carries no NodeId
//       before authentication, so there are no reserved slots for "known" peers. The per-IP cap still bounds pending, but
//       an admitted peer sharing the flood's IP is refused at accept until the 5 s pending reaper frees slots. Measured,
//       and asserted to recover.
// Implements 020-Security F13 as carried to the pinned wire by ADR-072; 021 pending-socket caps.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "pinned_test_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster{777};
using Clock = std::chrono::steady_clock;

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::int64_t since_ms(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

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

    explicit Stack(Node& node) : n(node), tcp(node.id, pal::ipv4_loopback, 0), sec(tcp, unused, node.id) {
        tcp.set_pinned_wire(true);
        sec.enable_handshake(*n.factory, kCluster, identity, trust);
        sec.on_receive([this](MessageFrame) { got.fetch_add(1); });
        tcp.set_peer_down_hook([this](NodeId p) { sec.on_peer_disconnected(p); });
        sec.set_reset_hook([this](NodeId p) { tcp.reset_peer_connection(p); });
    }
    ~Stack() { tcp.stop(); }
};

MessageFrame data(NodeId from, NodeId to) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Data;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    f.payload.push_back(std::byte{1});
    return f;
}

// One flood connection. From 127.0.0.1 the portable PAL connect is used; another source address needs a bound socket
// (Linux only: all of 127/8 is loopback). Returns the fd or -1.
long long flood_connect(std::uint16_t port, bool other_ip) {
    if (!other_ip) {
        auto c = pal::tcp_connect(pal::ipv4_loopback, port);
        return c ? static_cast<long long>(*c) : -1;
    }
#if defined(__linux__)
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in src{};
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(0x7F000002);
    timeval tv{0, 300'000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(0x7F000001);
    dst.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof src) != 0 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof dst) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}
void flood_send(long long fd, const Bytes& b, bool other_ip) {
    if (other_ip) {
#if defined(__linux__)
        (void)::send(static_cast<int>(fd), b.data(), b.size(), MSG_NOSIGNAL);
#endif
        return;
    }
    std::size_t off = 0;
    for (int i = 0; i < 200 && off < b.size(); ++i) {
        auto w = pal::send_some(static_cast<pal::fd_t>(fd), b.data() + off, b.size() - off);
        if (w) off += *w; else sleep_ms(1);
    }
}
void flood_close(long long fd, bool other_ip) {
    if (other_ip) {
#if defined(__linux__)
        ::close(static_cast<int>(fd));
#endif
    } else {
        pal::close_fd(static_cast<pal::fd_t>(fd));
    }
}

struct Result {
    int opened = 0;
    std::uint64_t max_pending = 0;
    std::uint64_t drops = 0;
    std::size_t sec_pending = 0;
    std::int64_t admitted_ms = -1;  // time from the admitted peer's first send to its first delivered frame; -1 = none
};

// Flood `total` connections, then (flood still held) let the admitted peer dial and measure. `limit_ms`: how long the
// admitted peer may take. Flood connections are held for the whole measurement.
Result run(Node& a, Node& b, bool other_ip, std::uint32_t per_ip_cap, int total, int limit_ms) {
    Result r;
    Stack sb(b);
    sb.tcp.set_max_pending_inbound_per_ip(per_ip_cap);
    (void)sb.tcp.start();
    const std::uint16_t port = sb.tcp.listen_port();

    MessageFrame f{};
    f.kind = FrameKind::Authenticate;
    f.payload.assign(kPinnedMsg1Len, std::byte{0x5A});
    const Bytes msg1 = net::encode_frame(f);

    std::vector<long long> held;
    for (int i = 0; i < total; ++i) {
        const long long fd = flood_connect(port, other_ip);
        if (fd < 0) continue;
        ++r.opened;
        if (i % 2 == 1) flood_send(fd, msg1, other_ip);  // wrong-key opener: refused and closed by the node
        held.push_back(fd);
        if (i % 100 == 0) r.max_pending = std::max(r.max_pending, sb.tcp.pending_inbound_count());
    }
    sleep_ms(150);
    r.max_pending = std::max(r.max_pending, sb.tcp.pending_inbound_count());

    Stack sa(a);
    sa.tcp.add_peer(loopback_endpoint(b.id, port));
    const auto t0 = Clock::now();
    while (since_ms(t0) < limit_ms) {
        sa.sec.send(b.id, data(a.id, b.id));
        r.max_pending = std::max(r.max_pending, sb.tcp.pending_inbound_count());
        if (sb.got.load() > 0) {
            r.admitted_ms = since_ms(t0);
            break;
        }
        sleep_ms(15);
    }
    r.drops = sb.tcp.drops();
    r.sec_pending = sb.sec.pending_handshake_count();
    for (const long long fd : held) flood_close(fd, other_ip);
    return r;
}

void report(const char* tag, const Result& r) {
    std::printf("%s: opened=%d max_pending=%llu drops=%llu admitted_ms=%lld\n", tag, r.opened,
                static_cast<unsigned long long>(r.max_pending), static_cast<unsigned long long>(r.drops),
                r.sec_pending, static_cast<long long>(r.admitted_ms));
}
}  // namespace

int main() {
    bool ok = true;
    if (!adapters::noise::init()) return 1;
    // The responder must be the higher id (only it answers an opener, #48); the admitted dialler the lower.
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 2; ++i) nodes.push_back(std::make_unique<Node>());
    std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
    Node& lo = *nodes[0];
    Node& hi = *nodes[1];
    pair_up(lo, hi, kCluster);

    constexpr int kFlood = 3000;
    constexpr std::uint32_t kPerIp = 64;  // TcpTransport's default
    constexpr std::uint32_t kGlobal = 1024;

#if defined(__linux__)
    {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            (void)::setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    {
        const Result r = run(lo, hi, /*other_ip=*/true, kPerIp, 0, 3000);
        report("F0 BASELINE (no flood)", r);
        check(r.admitted_ms >= 0, "F0 BASELINE: with no flood the admitted peer is served (the harness works)", ok);
    }
    {
        const Result r = run(lo, hi, /*other_ip=*/true, kPerIp, kFlood, 3000);
        report("F1 flood from 127.0.0.2", r);
        check(r.opened >= kFlood * 9 / 10, "F1: the flood actually opened ~3000 connections", ok);
        check(r.max_pending <= kPerIp, "F1: pending never exceeds the per-IP cap under the flood", ok);
        check(r.admitted_ms >= 0 && r.admitted_ms < 3000, "F1: an admitted peer (other IP) completes its handshake DURING the flood", ok);
    }
    {
        const Result r = run(lo, hi, /*other_ip=*/true, /*per_ip_cap=*/0, kFlood, 2000);
        report("F2 CONTROL (per-IP cap off)", r);
        check(r.max_pending > kPerIp, "F2 CONTROL: without the cap the pending set grows past it", ok);
        check(r.max_pending <= kGlobal, "F2 CONTROL: the global cap still bounds it", ok);
        check(r.admitted_ms < 0, "F2 CONTROL: with the cap off the admitted peer is NOT served inside the flood window", ok);
    }
#else
    std::printf("pinned_flood_check: F1/F2 need a bound source IP (Linux); skipped on this platform\n");
#endif
    {
        const Result r = run(lo, hi, /*other_ip=*/false, kPerIp, kFlood, 12000);
        report("F3 same-IP flood", r);
        check(r.max_pending <= kPerIp, "F3: pending never exceeds the per-IP cap with the flood on the admitted peer's own IP", ok);
        check(r.admitted_ms >= 0, "F3: the admitted peer is served once the 5 s pending reaper frees the slots", ok);
    }

    std::printf(ok ? "pinned_flood_check: OK\n" : "pinned_flood_check: FAILED\n");
    return ok ? 0 : 1;
}
