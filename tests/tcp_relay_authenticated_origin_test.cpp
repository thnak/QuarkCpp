// Tests issue #14 — relay IDENTITY beyond the HELLO claim. The relay node R runs a SecureTransport with
// a real (mock-engine) mTLS handshake on each of its links and gates forwarding on
// `has_session(origin) && has_session(dest)`: only nodes that authenticated to R are relayed for, so a
// node that merely CLAIMS an id in its HELLO is refused. A and B also handshake with EACH OTHER through
// the relay (their Authenticate frames are themselves forwarded), then exchange end-to-end sealed data.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/detail/hash.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
}
[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> in) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[static_cast<std::size_t>(b)])) << (b * 8);
    return v;
}

class MockServerEngine final : public HandshakeEngine {
public:
    MockServerEngine(NodeId self, NodeId peer, ClusterId cluster) : self_(self), peer_(peer), cluster_(cluster) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (in.size() != 24 || get_u64(in.subspan(0, 8)) != peer_.value ||
            get_u64(in.subspan(8, 8)) != self_.value || get_u64(in.subspan(16, 8)) != cluster_.value) {
            failure_ = "malformed/mismatched ClientHello";
            return Step::Failed;
        }
        put_u64(out, self_.value);
        put_u64(out, peer_.value);
        put_u64(out, cluster_.value);
        return Step::Done;
    }
    Result take_result() override {
        const std::uint64_t lo = self_.value < peer_.value ? self_.value : peer_.value;
        const std::uint64_t hi = self_.value < peer_.value ? peer_.value : self_.value;
        Result r;
        r.send_cipher = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));
        r.recv_cipher = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));
        r.peer_fingerprint[0] = static_cast<std::byte>(peer_.value & 0xFF);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    NodeId self_, peer_;
    ClusterId cluster_;
    std::string failure_;
};

class MockClientEngine final : public HandshakeEngine {
public:
    MockClientEngine(NodeId self, NodeId peer, ClusterId cluster) : self_(self), peer_(peer), cluster_(cluster) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (!sent_hello_) {
            put_u64(out, self_.value);
            put_u64(out, peer_.value);
            put_u64(out, cluster_.value);
            sent_hello_ = true;
            return Step::WantWrite;
        }
        if (in.size() != 24 || get_u64(in.subspan(0, 8)) != peer_.value ||
            get_u64(in.subspan(8, 8)) != self_.value || get_u64(in.subspan(16, 8)) != cluster_.value) {
            failure_ = "malformed/mismatched ServerHello";
            return Step::Failed;
        }
        return Step::Done;
    }
    Result take_result() override {
        const std::uint64_t lo = self_.value < peer_.value ? self_.value : peer_.value;
        const std::uint64_t hi = self_.value < peer_.value ? peer_.value : self_.value;
        Result r;
        r.send_cipher = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));
        r.recv_cipher = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));
        r.peer_fingerprint[0] = static_cast<std::byte>(peer_.value & 0xFF);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    NodeId self_, peer_;
    ClusterId cluster_;
    bool sent_hello_ = false;
    std::string failure_;
};

class MockHandshakeFactory final : public HandshakeEngineFactory {
public:
    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer,
                                            ClusterId cluster_id, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        ++created_;
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id);
    }
    [[nodiscard]] int created() const noexcept { return created_; }

private:
    int created_ = 0;
};


}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{14};
    const NodeId A{1}, R{2}, B{3}, C{4};  // lower id = client role in each pair
    auto id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial ida(id_blob), idr(id_blob), idb(id_blob);
    TrustStore tsa(trust_blob), tsr(trust_blob), tsb(trust_blob);
    MockHandshakeFactory fa, fr, fb;
    MockCipher unused(0);

    net::TcpTransport ta(A, pal::ipv4_loopback, 0), tr(R, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
    SecureTransport sa(ta, unused, A), sr(tr, unused, R), sb(tb, unused, B);
    sa.enable_handshake(fa, cluster, ida, tsa);
    sr.enable_handshake(fr, cluster, idr, tsr);
    sb.enable_handshake(fb, cluster, idb, tsb);
    Recorder rb;
    Recorder ra;
    sa.on_receive([&](MessageFrame f) { ra(std::move(f)); });
    sr.on_receive([](MessageFrame) {});
    sb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    tr.set_relay_forwarding([&](NodeId origin, NodeId dest) { return sr.has_session(origin) && sr.has_session(dest); });
    check(ta.start() && tr.start() && tb.start(), "listeners start", ok);
    if (!ok) return 1;
    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(A, ta.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));
    ta.set_relay_via(B, {R});
    tb.set_relay_via(A, {R});

    // An unauthenticated claimant C: raw TcpTransport (a bare HELLO claim, no session with R).
    net::TcpTransport tc(C, pal::ipv4_loopback, 0);
    tc.set_allow_insecure_plaintext(true);
    tc.on_receive([](MessageFrame) {});
    check(tc.start(), "claimant starts", ok);
    tc.add_peer(loopback_endpoint(R, tr.listen_port()));
    tc.set_relay_via(B, {R});

    // 1. Authenticate to the relay (both spokes), and the relay to B / A.
    check(wait_until([&] { sa.send(R, seq_frame(A, R, 0)); return sr.has_session(A) && sa.has_session(R); }, 8000),
          "A <-> R session established", ok);
    check(wait_until([&] { sr.send(B, seq_frame(R, B, 0)); return sr.has_session(B) && sb.has_session(R); }, 8000),
          "R <-> B session established", ok);

    // 2. Unauthenticated claimant is refused even though its HELLO names a valid-looking id.
    const auto refused0 = tr.relay_refused();
    tc.send(B, seq_frame(C, B, 77));
    check(wait_until([&] { return tr.relay_refused() > refused0; }, 5000),
          "relay refuses a node that has no session with it", ok);

    // 3. A and B handshake end to end THROUGH the relay, then sealed data flows.
    check(wait_until([&] { sa.send(B, seq_frame(A, B, 1)); return sa.has_session(B) && sb.has_session(A); }, 8000),
          "A <-> B end-to-end session established via the relay", ok);
    const auto before = rb.count();
    for (std::uint64_t i = 0; i < 10; ++i) sa.send(B, seq_frame(A, B, 100 + i));
    check(wait_until([&] { return rb.count() >= before + 10; }, 5000), "10 sealed frames delivered via the relay", ok);
    check(sb.replays_rejected() == 0 && sb.tamper_rejected() == 0, "no replay/tamper rejections", ok);
    check(rb.count() == before + 10 || rb.count() > before, "no refused frame leaked to B", ok);
    {
        std::lock_guard<std::mutex> g(rb.m);
        for (auto s : rb.seqs) check(s != 77, "the claimant's frame never reached B", ok);
    }

    // 3b. The reverse direction also crosses the relay, including Control-lane frames: this is the path
    // ADR-046's `Congested` signal takes from a congested destination back to the origin, so the
    // end-to-end congestion signal survives the hop (B -> R -> A) instead of stopping at the relay.
    MessageFrame cf = seq_frame(B, A, 4242);
    cf.kind = FrameKind::Control;
    sb.send(A, std::move(cf));
    check(wait_until([&] { return ra.count() >= 1; }, 5000), "control frame B -> R -> A (congestion-signal path)", ok);

    // 4. Revoking A's session at the relay cuts the path.
    sr.on_peer_disconnected(A);
    const auto refused1 = tr.relay_refused();
    sa.send(B, seq_frame(A, B, 999));
    check(wait_until([&] { return tr.relay_refused() > refused1; }, 5000), "dropped session at R cuts A's relay path", ok);

    ta.stop(); tb.stop(); tc.stop(); tr.stop();
    std::printf("tcp_relay_authenticated_origin_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
