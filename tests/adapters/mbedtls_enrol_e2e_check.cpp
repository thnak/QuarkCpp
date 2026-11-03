// End-to-end check for ADR-074 D4 / issue #36: a node with ONLY a join code becomes a cluster member, over the real stack.
//
//   anchor : net::TcpTransport  <- SecureTransport (mbedTLS mTLS engine) ; the same listener also hands enrol hellos to an EnrolService
//   joiner : EnrolJoinerSession over a raw socket -> JoinedIdentity -> its own TcpTransport + SecureTransport with that identity
//
//   E1  before enrolment, a node whose certificate comes from a DIFFERENT root exchanges no frame with the anchor (CONTROL for E2)
//   E2  after enrolment the joiner's issued identity completes the mTLS handshake with the anchor and frames flow BOTH ways
//   E3  the issued identity passes the application's startup rule (`validate_identity_not_root`) and names the cluster and node the custodian assigned
//   E4  enrolment and member traffic share the one listener: a member frame is delivered while an enrolment is in flight
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "adapters/enrol_test_client.hpp"
#include "adapters/enrol_test_pki.hpp"
#include "quark/adapters/mbedtls/mbedtls_enrol_service.hpp"
#include "quark/adapters/mbedtls/mbedtls_enrollment.hpp"
#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/secure_transport.hpp"
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

// One node: TcpTransport under a SecureTransport driving the mbedTLS engine with `id` / `roots`.
struct Stack {
    NodeId self;
    net::TcpTransport tcp;
    MockCipher unused{0};
    SecureTransport sec;
    std::shared_ptr<const TlsIdentity> id_blob;
    std::shared_ptr<const TrustedRoots> trust_blob;
    IdentityMaterial identity;
    TrustStore trust;
    MbedtlsHandshakeEngineFactory factory;
    std::atomic<std::uint64_t> got{0};

    Stack(NodeId node, TlsIdentity id, Bytes root_der)
        : self(node),
          tcp(node, pal::ipv4_loopback, 0),
          sec(tcp, unused, node),
          id_blob(std::make_shared<const TlsIdentity>(std::move(id))),
          trust_blob(std::make_shared<const TrustedRoots>(TrustedRoots{std::move(root_der)})),
          identity(id_blob),
          trust(trust_blob) {
        sec.enable_handshake(factory, ClusterId{kCluster}, identity, trust);
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
    f.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    return f;
}

// Send from `tx` until `rx` has seen `want` more frames (frames sent before the session exists are dropped by design).
bool deliver(Stack& tx, Stack& rx, NodeId to, std::uint64_t want, int timeout_ms = 8000) {
    const std::uint64_t base = rx.got.load();
    return wait_until(
        [&] {
            if (rx.got.load() >= base + want) return true;
            tx.sec.send(to, data(tx.self, to));
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            return false;
        },
        timeout_ms);
}
}  // namespace

int main() {
    World w;
    const NodeId anchor_node{kAnchor};
    Stack anchor(anchor_node, w.anchor_id, w.root.cert);
    EnrolService svc(w.anchor_id, *w.enr, EnrolServiceConfig{}, [&] { return w.clock.load(); });
    anchor.tcp.set_enrol_handoff([&](net::TcpTransport::EnrolHandoff& h) { return svc.handoff(h.fd, h.peer_addr, std::move(h.initial)); });
    if (!anchor.tcp.start()) {
        std::fprintf(stderr, "anchor start failed\n");
        return 2;
    }

    // ---------------------------------------------------------------------------------------------- E1 (control)
    {
        const NodeId rn{0x66};
        Leaf l = make_leaf(w.rogue, good_spec(cn_for(kCluster, rn.value)));
        Stack rogue(rn, TlsIdentity{concat(l.cert, w.rogue.cert), l.key_der, {}}, w.rogue.cert);
        rogue.tcp.add_peer(quark::test::loopback_endpoint(anchor_node, anchor.tcp.listen_port()));
        check(rogue.tcp.start(), "E1 setup: the rogue node starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (int i = 0; i < 10; ++i) {
            rogue.sec.send(anchor_node, data(rn, anchor_node));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        check(anchor.got.load() == 0, "E1 CONTROL: a certificate from another root exchanges no frame with the anchor");
    }

    // ---------------------------------------------------------------------------------------------- enrol
    auto ticket = w.enr->issue_ticket(enrol::Role::Member, 60'000'000'000, w.clock.load());
    if (!ticket) {
        std::fprintf(stderr, "ticket failed\n");
        return 2;
    }
    EnrolJoinerSession j(w.code(*ticket));
    {
        Client c(anchor.tcp.listen_port());
        check(drive(j, c, 8000) == EnrolJoinerSession::State::Joined, "E2 setup: the joiner enrols over the anchor's member listener");
    }
    auto id = j.take_identity();
    if (!id) {
        std::fprintf(stderr, "no identity\n");
        return 1;
    }

    // ---------------------------------------------------------------------------------------------- E3
    check(id->cluster.value == kCluster && id->node.value == kNode, "E3 the identity names the cluster and the node the custodian assigned");
    TlsIdentity issued{id->cert_chain_der, id->key_der, {}};
    {
        TrustStore ts(std::make_shared<const TrustedRoots>(TrustedRoots{id->ca_der}));
        check(static_cast<bool>(validate_identity_not_root(issued, ts.snapshot(0))), "E3 the issued identity passes the startup rule (a node never runs with a root's key)");
    }

    // ---------------------------------------------------------------------------------------------- E2
    Stack joiner(id->node, issued, id->ca_der);
    joiner.tcp.add_peer(quark::test::loopback_endpoint(anchor_node, anchor.tcp.listen_port()));
    check(joiner.tcp.start(), "E2 setup: the joiner's member stack starts");
    check(deliver(joiner, anchor, anchor_node, 1), "E2 a frame from the enrolled joiner reaches the anchor");
    check(deliver(anchor, joiner, id->node, 1), "E2 and a frame from the anchor reaches the joiner");

    // ---------------------------------------------------------------------------------------------- E4
    {
        auto t2 = w.enr->issue_ticket(enrol::Role::Member, 60'000'000'000, w.clock.load());
        (void)w.enr->issue_ticket(enrol::Role::Member, 60'000'000'000, w.clock.load());
        EnrolJoinerSession j2(w.code(*t2));
        Client c2(anchor.tcp.listen_port());
        Bytes out;
        j2.start(out);
        check(c2.send_all(out), "E4 setup: a second enrolment is in flight (ClientHello sent)");
        check(deliver(joiner, anchor, anchor_node, 1), "E4 member traffic is delivered while an enrolment is in flight on the same listener");
    }

    std::printf(g_ok ? "mbedtls_enrol_e2e_check: all checks passed\n" : "mbedtls_enrol_e2e_check: FAILED\n");
    return g_ok ? 0 : 1;
}
