// Implements 019-Platform-Abstraction-Layer §"Android target" (Draft amendment) and 021 §"Mobile member
// profile"; ADR-071. libquark_android.so: one Quark cluster node (TcpTransport over the Linux/epoll PAL +
// SwimMembership + MobileMember) behind a C ABI (include/quark/android/quark_android.h) for the app's JNI glue.
// No JNI types, no calls up into Java. The SWIM protocol is driven by one ticker thread that posts onto the
// transport's I/O loop, so membership state is only ever touched from that loop (SwimMembership's rule).
#include "quark/android/quark_android.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "quark/core/cluster.hpp"
#include "quark/core/mobile_member.hpp"
#include "quark/net/tcp_transport.hpp"

struct QuarkAndroidNode {
    quark::NodeId self;
    quark::ClusterId cluster;
    std::uint32_t tick_ms;
    quark::net::TcpTransport tcp;
    std::unique_ptr<quark::SwimMembership> swim;
    std::unique_ptr<quark::MobileMember> mobile;
    std::atomic<std::uint64_t> data_rx{0};
    std::atomic<bool> run{false};
    std::thread ticker;

    QuarkAndroidNode(std::uint64_t id, std::uint64_t cl, std::uint32_t bind, std::uint16_t port, std::uint32_t tick)
        : self{id}, cluster{cl}, tick_ms(tick ? tick : 200), tcp(quark::NodeId{id}, bind, port) {}
};

namespace {
template <class F>
auto on_loop(QuarkAndroidNode* n, F f) {  // run f on the I/O loop and wait for its result
    using R = decltype(f());
    auto p = std::make_shared<std::promise<R>>();
    auto fut = p->get_future();
    n->tcp.post([p, f = std::move(f)]() mutable {
        if constexpr (std::is_void_v<R>) {
            f();
            p->set_value();
        } else {
            p->set_value(f());
        }
    });
    if constexpr (std::is_void_v<R>) {
        (void)fut.wait_for(std::chrono::seconds(5));
    } else {
        return fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready ? fut.get() : R{};
    }
}
}  // namespace

extern "C" {

QuarkAndroidNode* quark_android_node_create(uint64_t node_id, uint64_t cluster_id, uint32_t bind_ipv4,
                                            uint16_t listen_port, uint32_t tick_ms, int allow_insecure_dev) {
    auto* n = new QuarkAndroidNode(node_id, cluster_id, bind_ipv4, listen_port, tick_ms);
    if (allow_insecure_dev) n->tcp.set_allow_insecure_plaintext(true);
    quark::SwimMembership::Config cfg;
    cfg.cluster_id = quark::ClusterId{cluster_id};
    cfg.ack_timeout_ns = static_cast<std::int64_t>(n->tick_ms) * 1'000'000;
    cfg.suspicion_timeout_ns = 5LL * 1'000'000'000;
    cfg.seed = node_id;
    n->swim = std::make_unique<quark::SwimMembership>(n->self, n->tcp, cfg);
    n->swim->set_data_sink([n](quark::MessageFrame) { n->data_rx.fetch_add(1, std::memory_order_relaxed); });
    quark::net::TcpTransport* tcp = &n->tcp;
    n->mobile = std::make_unique<quark::MobileMember>(*n->swim, [tcp](quark::NodeId p) { tcp->reset_peer_connection(p); },
                                                      cfg.mobile);
    return n;
}

int quark_android_node_add_peer(QuarkAndroidNode* n, uint64_t peer_id, uint32_t peer_ipv4, uint16_t peer_port) {
    if (!n) return -1;
    n->tcp.add_peer(quark::Endpoint{quark::NodeId{peer_id}, peer_ipv4, peer_port});
    return 0;
}

int quark_android_node_start(QuarkAndroidNode* n) {
    if (!n || !n->tcp.start()) return -1;
    n->run.store(true);
    n->ticker = std::thread([n] {
        while (n->run.load()) {
            n->tcp.post([n] { n->swim->tick(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(n->tick_ms));
        }
    });
    return 0;
}

uint16_t quark_android_node_port(QuarkAndroidNode* n) { return n ? n->tcp.listen_port() : 0; }

int quark_android_node_join(QuarkAndroidNode* n, uint64_t seed_id) {
    if (!n) return -1;
    n->tcp.post([n, seed_id] { n->swim->request_join(quark::NodeId{seed_id}); });
    return 0;
}

int quark_android_node_send_probe(QuarkAndroidNode* n, uint64_t peer_id, uint64_t seq) {
    if (!n) return -1;
    quark::MessageFrame f;
    f.from = n->self;
    f.to = quark::NodeId{peer_id};
    f.target = quark::ActorId{quark::TypeKey{0x5151u}, seq};
    f.msg_type = quark::TypeKey{0x99u};
    f.trace_id = seq;
    f.payload = {static_cast<std::byte>(seq & 0xFF)};
    n->tcp.send(f.to, std::move(f));
    return 0;
}

void quark_android_node_suspend(QuarkAndroidNode* n, int reason, uint32_t lease_ms) {
    if (!n) return;
    on_loop(n, [n, reason, lease_ms] {
        n->mobile->suspend(static_cast<quark::SuspendReason>(reason),
                           static_cast<std::int64_t>(lease_ms) * 1'000'000);
    });
}
void quark_android_node_resume(QuarkAndroidNode* n) {
    if (n) on_loop(n, [n] { n->mobile->resume(); });
}
void quark_android_node_network_changed(QuarkAndroidNode* n) {
    if (n) on_loop(n, [n] { n->mobile->network_changed(); });
}

uint64_t quark_android_node_frames_received(QuarkAndroidNode* n) { return n ? n->data_rx.load() : 0; }
uint64_t quark_android_node_view_size(QuarkAndroidNode* n) { return n ? n->swim->view().size() : 0; }

int quark_android_node_peer_state(QuarkAndroidNode* n, uint64_t peer_id) {
    if (!n) return QUARK_PEER_UNKNOWN;
    return on_loop(n, [n, peer_id] {
        const quark::NodeId p{peer_id};
        if (!n->swim->is_known(p)) return static_cast<int>(QUARK_PEER_UNKNOWN);
        if (n->swim->is_suspended(p)) return static_cast<int>(QUARK_PEER_SUSPENDED);
        switch (n->swim->status_of(p)) {
            case quark::MemberStatus::Alive: return static_cast<int>(QUARK_PEER_ALIVE);
            case quark::MemberStatus::Suspect: return static_cast<int>(QUARK_PEER_SUSPECT);
            case quark::MemberStatus::Dead: return static_cast<int>(QUARK_PEER_DEAD);
            case quark::MemberStatus::Left: return static_cast<int>(QUARK_PEER_LEFT);
        }
        return static_cast<int>(QUARK_PEER_UNKNOWN);
    });
}

int quark_android_node_self_suspended(QuarkAndroidNode* n) {
    return n ? on_loop(n, [n] { return n->swim->self_suspended() ? 1 : 0; }) : 0;
}

void quark_android_node_destroy(QuarkAndroidNode* n) {
    if (!n) return;
    n->run.store(false);
    if (n->ticker.joinable()) n->ticker.join();
    n->tcp.stop();
    delete n;
}

}  // extern "C"
