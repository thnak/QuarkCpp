// Pre-auth connection hardening of the default TcpTransport (review A2/C3 — REVIEW-BACKLOG.md).
// The hello NodeId is only a CLAIM at this layer (identity is proven later, by the secure handshake),
// so the transport must bound what an unauthenticated peer can pin and must not let a claim displace
// a live connection:
//   1. a socket that never sends its hello is closed after the hello deadline (slowloris);
//   2. unidentified inbound sockets are capped per source IP;
//   3. IDENTIFIED inbound connections (fake-id hellos) are capped per source IP;
//   4. a hello claiming a live peer's NodeId from a DIFFERENT source IP does not take over its slot
//      (negative control: with strict binding off, it does — proving the guard is what holds).
// Raw sockets via the PAL speak the hello directly; 127.0.0.2 gives a second loopback source IP.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "tcp_test_util.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace quark;
using namespace quark::test;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

constexpr std::uint64_t kAny = pal::ipv4(0, 0, 0, 0);
#if defined(__linux__)
// Second loopback source address — only the Linux-only raw-socket section below binds from it.
constexpr std::uint64_t kLoop2 = pal::ipv4(127, 0, 0, 2);
#endif

std::vector<std::byte> hello_bytes(NodeId node) {
    std::vector<std::byte> h;
    const auto put = [&h](std::uint64_t v, int n) {
        for (int b = 0; b < n; ++b) h.push_back(static_cast<std::byte>((v >> (8 * b)) & 0xFF));
    };
    put(net::hello_magic, 4);
    put(net::hello_version, 2);
    put(node.value, 8);
    return h;
}

// A raw client socket. send_hello() retries until the (non-blocking) connect completes.
struct RawConn {
    pal::fd_t fd = pal::invalid_fd;
    bool open_ok = false;

    RawConn(std::uint64_t addr, std::uint16_t port) {
        auto r = pal::tcp_connect(addr, port);
        if (r) {
            fd = *r;
            open_ok = true;
        }
    }
    explicit RawConn(pal::fd_t adopted) : fd(adopted), open_ok(adopted != pal::invalid_fd) {}
    RawConn(const RawConn&) = delete;
    RawConn& operator=(const RawConn&) = delete;
    ~RawConn() {
        if (open_ok) pal::close_fd(fd);
    }

    bool send_hello(NodeId node) {
        const std::vector<std::byte> h = hello_bytes(node);
        std::size_t sent = 0;
        return wait_until(
            [&] {
                auto r = pal::send_some(fd, h.data() + sent, h.size() - sent);
                if (r) sent += *r;
                return sent == h.size();
            },
            2000);
    }

    // True once the peer closed (EOF or reset). Drains whatever the server sent (its hello) first.
    bool closed() {
        std::byte buf[64];
        for (;;) {
            auto r = pal::recv_some(fd, buf, sizeof(buf));
            if (!r) return r.error() != pal::would_block();  // would_block ⇒ still open
            if (*r == 0) return true;                        // orderly EOF
        }
    }
};

#if defined(__linux__)
// Connect to dst:port with the SOURCE address pinned to `src` (the kernel would otherwise pick
// 127.0.0.1 for any 127/8 destination). Blocking connect on loopback, then non-blocking.
pal::fd_t connect_from(std::uint64_t src_host_order, std::uint64_t dst_host_order, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return pal::invalid_fd;
    ::sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(static_cast<std::uint32_t>(src_host_order));
    if (::bind(fd, reinterpret_cast<::sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd);
        return pal::invalid_fd;
    }
    ::sockaddr_in da{};
    da.sin_family = AF_INET;
    da.sin_port = htons(port);
    da.sin_addr.s_addr = htonl(static_cast<std::uint32_t>(dst_host_order));
    if (::connect(fd, reinterpret_cast<::sockaddr*>(&da), sizeof(da)) < 0) {
        ::close(fd);
        return pal::invalid_fd;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
#endif

std::size_t count_closed(std::vector<std::unique_ptr<RawConn>>& cs) {
    std::size_t n = 0;
    for (auto& c : cs)
        if (c->closed()) ++n;
    return n;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- 1. hello deadline ----------------------------------------------------------------------
    {
        net::TcpTransport t(NodeId{100}, kAny, 0);
        t.set_pending_hello_timeout_ns(100'000'000);  // 100 ms
        check(t.start(), "listener starts", ok);
        RawConn silent(pal::ipv4_loopback, t.listen_port());
        check(silent.open_ok, "raw client connects", ok);
        check(wait_until([&] { return silent.closed(); }, 3000),
              "a socket that never sends its hello is closed after the deadline", ok);
        t.stop();
    }

    // ---- 2. per-IP cap on UNIDENTIFIED sockets --------------------------------------------------
    {
        net::TcpTransport t(NodeId{100}, kAny, 0);
        t.set_pending_hello_timeout_ns(0);  // keep them pending: isolate the per-IP cap
        t.set_max_pending_inbound_per_ip(3);
        check(t.start(), "listener starts", ok);
        std::vector<std::unique_ptr<RawConn>> cs;
        for (int i = 0; i < 6; ++i) cs.push_back(std::make_unique<RawConn>(pal::ipv4_loopback, t.listen_port()));
        check(wait_until([&] { return count_closed(cs) >= 3; }, 3000), "excess pending sockets are closed", ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(count_closed(cs) == 3, "exactly 6 - cap(3) pending sockets were refused", ok);
        t.stop();
    }

    // ---- 3. per-IP cap on IDENTIFIED inbound connections (the fake-id hello bypass) --------------
    {
        net::TcpTransport t(NodeId{100}, kAny, 0);
        t.set_max_inbound_per_ip(2);
        check(t.start(), "listener starts", ok);
        std::vector<std::unique_ptr<RawConn>> cs;
        for (std::uint64_t i = 0; i < 5; ++i) {
            cs.push_back(std::make_unique<RawConn>(pal::ipv4_loopback, t.listen_port()));
            check(cs.back()->send_hello(NodeId{11 + i}), "hello sent", ok);
            // Let each one settle so acceptance order (hence which 2 win) is deterministic.
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        check(wait_until([&] { return count_closed(cs) >= 3; }, 3000),
              "identified inbound connections beyond the per-IP cap are closed", ok);
        check(wait_until([&] { return t.connections_open() == 2; }, 3000),
              "exactly the per-IP cap of fake-id connections stays open", ok);
        t.stop();
    }

#if defined(__linux__)
    // ---- 4. a claim from another IP cannot take over a live peer's slot -------------------------
    for (const bool strict : {true, false}) {
        // self=100 > peer=5, so on a dedup tie the NEW inbound wins (keep_local_dial(100,5) == false):
        // with strict binding off the hijack succeeds — the control that proves the guard matters.
        net::TcpTransport t(NodeId{100}, kAny, 0);
        t.set_strict_peer_addr_binding(strict);
        check(t.start(), "listener starts", ok);
        RawConn legit(connect_from(pal::ipv4_loopback, pal::ipv4_loopback, t.listen_port()));
        check(legit.send_hello(NodeId{5}), "legit hello sent", ok);
        check(wait_until([&] { return t.connections_open() == 1; }, 3000), "legit peer registered", ok);

        RawConn attacker(connect_from(kLoop2, pal::ipv4_loopback, t.listen_port()));
        check(attacker.open_ok, "attacker binds 127.0.0.2 and connects", ok);
        check(attacker.send_hello(NodeId{5}), "attacker hello sent", ok);  // claims the live peer's id
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (strict) {
            check(attacker.closed(), "strict: the spoofed claim from another IP is rejected", ok);
            check(!legit.closed(), "strict: the live connection is untouched", ok);
            check(t.connections_open() == 1, "strict: still exactly one connection", ok);
        } else {
            check(legit.closed() && !attacker.closed(),
                  "control: without binding, the spoofed claim displaces the live peer", ok);
        }
        t.stop();
    }

#else
    std::printf("  (case 4 needs a source-address-bound client socket: Linux only, skipped)\n");
#endif

    std::printf("tcp_transport_pre_auth_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
