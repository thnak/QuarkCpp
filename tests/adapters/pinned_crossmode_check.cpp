// Cross-mode: a CA-chain (mbedTLS TLS 1.3 mTLS) peer and a pinned-key (Noise IK) peer NEVER establish a session, in either
// direction; same-mode pairs of each kind do (the controls). ADR-068 claim X1. Needs BOTH adapters in one binary, so it is
// only built with QUARK_WITH_SODIUM=ON and QUARK_WITH_MBEDTLS=ON (cmake/QuarkSecurityAdapters.cmake).
//
// There is nothing to negotiate and so nothing to downgrade: the mode is chosen by which HandshakeEngineFactory a cluster
// installs, and the mode byte is in the Noise prologue (pinned_handshake_check H6 covers the prologue on its own).
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" (no downgrade) and ADR-040 (TLS mode unchanged).
#include <cstdio>

#include "pinned_test_util.hpp"
#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "test_pki.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
// Drive client against server, feeding each side's output to the other, bounded; report both terminal steps and every byte.
struct Drive {
    HandshakeEngine::Step client = HandshakeEngine::Step::WantRead;
    HandshakeEngine::Step server = HandshakeEngine::Step::WantRead;
    Bytes first_client_bytes;
    [[nodiscard]] bool both_done() const {
        return client == HandshakeEngine::Step::Done && server == HandshakeEngine::Step::Done;
    }
};
Drive drive(HandshakeEngine& client, HandshakeEngine& server) {
    Drive d;
    Bytes c2s, s2c, scratch;
    d.client = client.advance({}, c2s);
    d.first_client_bytes = c2s;
    for (int round = 0; round < 20; ++round) {
        const bool cterm = d.client == HandshakeEngine::Step::Done || d.client == HandshakeEngine::Step::Failed;
        const bool sterm = d.server == HandshakeEngine::Step::Done || d.server == HandshakeEngine::Step::Failed;
        if (cterm && sterm) break;
        bool moved = false;
        if (!c2s.empty() && !sterm) {
            scratch.clear();
            d.server = server.advance(c2s, scratch);
            c2s.clear();
            s2c.insert(s2c.end(), scratch.begin(), scratch.end());
            moved = true;
        }
        if (!s2c.empty() && !cterm) {
            scratch.clear();
            d.client = client.advance(s2c, scratch);
            s2c.clear();
            c2s.insert(c2s.end(), scratch.begin(), scratch.end());
            moved = true;
        }
        if (!moved) break;
    }
    return d;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    const ClusterId cluster{777};

    // --- the TLS side: a real CA, two leaves (NodeIds 1 and 2) --------------------------------------------------------
    test_pki::Rng rng;
    mbedtls_pk_context ca_key;
    mbedtls_pk_init(&ca_key);
    const test_pki::GeneratedCert ca = test_pki::make_ca(rng, ca_key, "quark-test-ca");
    const test_pki::GeneratedCert leaf1 = test_pki::make_leaf(rng, ca, ca_key, cluster.value, 1);
    const test_pki::GeneratedCert leaf2 = test_pki::make_leaf(rng, ca, ca_key, cluster.value, 2);
    mbedtls_pk_free(&ca_key);
    IdentityMaterial tls_id1(std::make_shared<const TlsIdentity>(TlsIdentity{leaf1.cert_der, leaf1.key_der, {}}));
    IdentityMaterial tls_id2(std::make_shared<const TlsIdentity>(TlsIdentity{leaf2.cert_der, leaf2.key_der, {}}));
    TrustStore tls_trust(std::make_shared<const TrustedRoots>(TrustedRoots{ca.cert_der}));
    MbedtlsHandshakeEngineFactory tls;
    const NodeId t1{1}, t2{2};
    auto tls_client = [&] { return tls.create(true, t1, t2, cluster, tls_id1.snapshot(0), tls_trust.snapshot(0), nullptr); };
    auto tls_server = [&] { return tls.create(false, t2, t1, cluster, tls_id2.snapshot(0), tls_trust.snapshot(0), nullptr); };

    // --- the pinned side ----------------------------------------------------------------------------------------------------
    Node a, b;
    pair_up(a, b, cluster);
    auto pin_client = [&] { return a.engine(true, b.id, cluster); };
    auto pin_server = [&] { return b.engine(false, a.id, cluster); };

    // CONTROLS: same-mode pairs complete.
    {
        auto c = tls_client();
        auto s = tls_server();
        check(drive(*c, *s).both_done(), "X1 CONTROL: TLS(CA) <-> TLS(CA) completes", ok);
        auto pc = pin_client();
        auto ps = pin_server();
        check(drive(*pc, *ps).both_done(), "X1 CONTROL: pinned <-> pinned completes", ok);
    }

    // TLS client -> pinned responder: the responder never answers and nothing completes.
    {
        auto c = tls_client();
        auto ps = pin_server();
        const Drive d = drive(*c, *ps);
        check(!d.both_done() && d.server != HandshakeEngine::Step::Done && d.client != HandshakeEngine::Step::Done,
              "X1: TLS(CA) client against a pinned responder never completes", ok);
        // And the responder emitted nothing in reply to the ClientHello (the TLS client saw no byte).
        auto c2 = tls_client();
        auto ps2 = pin_server();
        Bytes hello;
        (void)c2->advance({}, hello);
        const Feed f = feed_responder(*ps2, hello);
        check(f.step != HandshakeEngine::Step::Done && f.out.empty(),
              "X1: the pinned responder answers a TLS ClientHello with zero bytes", ok);
    }

    // pinned initiator -> TLS server: never completes.
    {
        auto pc = pin_client();
        auto s = tls_server();
        const Drive d = drive(*pc, *s);
        check(!d.both_done() && d.server != HandshakeEngine::Step::Done && d.client != HandshakeEngine::Step::Done,
              "X1: a pinned client against a TLS(CA) server never completes", ok);
    }

    // Keys from one mode never open the other's traffic (distinct cipher families and label domains).
    {
        auto pc = pin_client();
        auto ps = pin_server();
        auto tc = tls_client();
        auto ts = tls_server();
        const Drive dp = drive(*pc, *ps), dt = drive(*tc, *ts);
        if (dp.both_done() && dt.both_done()) {
            auto pr = pc->take_result();
            auto tr = ts->take_result();
            std::vector<std::byte> sealed, opened;
            pr.send_cipher->seal(1, {}, std::vector<std::byte>{std::byte{1}, std::byte{2}}, sealed);
            check(!tr.recv_cipher->open(1, {}, sealed, opened), "X1: a pinned session's frame does not open under a TLS session's key", ok);
        } else {
            check(false, "X1: setup for the key-domain check", ok);
        }
    }

    std::printf(ok ? "pinned_crossmode_check: OK\n" : "pinned_crossmode_check: FAILED\n");
    return ok ? 0 : 1;
}
