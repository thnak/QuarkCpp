// Shared helpers for the pinned-key checks that run over the REAL TcpTransport + SecureTransport + Noise IK engine
// (pinned_tcp_check, pinned_nodeid_role_check): NOT a *_check.cpp, only an include. A recording TCP proxy, a `Stack` (one
// node = TcpTransport under a SecureTransport driving the pinned engine), a frame builder and a send-until-delivered helper.
// Moved verbatim out of pinned_tcp_check.cpp (#63); no behaviour change.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "pinned_test_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

namespace quark::test {

inline const ClusterId kPinnedTcpCluster{777};

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F>
inline bool wait_for(F f, int ms = 5000) {
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
        sec.enable_handshake(*n.factory, kPinnedTcpCluster, identity, trust);
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

inline MessageFrame data(NodeId from, NodeId to, std::string_view text) {
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
inline bool deliver(Stack& tx, Stack& rx, NodeId to, std::uint64_t want, std::string_view text = "payload",
                    int timeout_ms = 5000) {
    const std::uint64_t base = rx.got.load();
    return wait_for([&] {
        if (rx.got.load() >= base + want) return true;
        tx.sec.send(to, data(tx.n.id, to, text));
        sleep_ms(15);
        return false;
    }, timeout_ms);
}


}  // namespace quark::test
