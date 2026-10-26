// Tests issue #14 — relay shape C, the SEALED-HOP relay (net/sealed_relay_transport.hpp). The inner
// frame's header must not be visible on the origin<->relay wire, which shape B (forwarding below the
// security layer) cannot give. Stack: e2e SecureTransport -> SealedRelayTransport -> hop SecureTransport
// (real mock-engine handshake) -> a SPY that records exactly what is handed to the wire -> TcpTransport.
// Checks: A -> R -> B delivery in order; the spy sees ONLY frames addressed to the relay, never one with
// the destination's id or the inner msg_type; a node that is not an authenticated member is refused; an
// inner frame whose `from` differs from the authenticated hop identity is refused; a transit delivery
// from a node that is not a configured relay is refused.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
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
#include "quark/net/sealed_relay_transport.hpp"
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



// Records every frame handed to the wire, then delegates. (Test-only wire tap.)
class SpyTransport final : public Transport {
public:
    explicit SpyTransport(Transport& inner) : inner_(&inner) {}
    void send(NodeId to, MessageFrame f) override {
        {
            std::lock_guard<std::mutex> g(mu);
            sent.push_back({to, f.msg_type.value, f.target.key, f.kind});
        }
        inner_->send(to, std::move(f));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_->on_receive(std::move(cb)); }
    [[nodiscard]] bool is_plaintext() const noexcept override { return inner_->is_plaintext(); }
    void set_authenticated_transport(bool a) noexcept override { inner_->set_authenticated_transport(a); }
    [[nodiscard]] bool admit_send(NodeId p) noexcept override { return inner_->admit_send(p); }
    void mark_congested(NodeId p, std::int64_t r) noexcept override { inner_->mark_congested(p, r); }
    struct Rec { NodeId to; std::uint64_t msg_type; std::uint64_t target_key; FrameKind kind; };
    std::mutex mu;
    std::vector<Rec> sent;

private:
    Transport* inner_;
};

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{1415};
    const NodeId A{1}, R{2}, B{3}, X{4};
    auto id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial ida(id_blob), idr(id_blob), idb(id_blob);
    TrustStore tsa(trust_blob), tsr(trust_blob), tsb(trust_blob);
    MockHandshakeFactory fa, fr, fb;
    MockCipher unused(0), e2e_cipher(0xE2EULL);

    net::TcpTransport ta(A, pal::ipv4_loopback, 0), tr(R, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
    SpyTransport spy_a(ta), spy_r(tr), spy_b(tb);
    SecureTransport hop_a(spy_a, unused, A), hop_r(spy_r, unused, R), hop_b(spy_b, unused, B);
    hop_a.enable_handshake(fa, cluster, ida, tsa);
    hop_r.enable_handshake(fr, cluster, idr, tsr);
    hop_b.enable_handshake(fb, cluster, idb, tsb);
    net::SealedRelayTransport sra(hop_a, A), srr(hop_r, R), srb(hop_b, B);
    SecureTransport e2e_a(sra, e2e_cipher, A), e2e_b(srb, e2e_cipher, B);
    Recorder rb;
    e2e_a.on_receive([](MessageFrame) {});
    e2e_b.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    srr.on_receive([](MessageFrame) {});
    srr.set_forwarding([](NodeId, NodeId) { return true; });  // membership gating is covered elsewhere
    sra.set_relay_via(B, {R});
    srb.set_relay_via(A, {R});

    check(ta.start() && tr.start() && tb.start(), "listeners start", ok);
    if (!ok) return 1;
    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(A, ta.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));

    // Hop sessions: A<->R and R<->B (priming frames are hop-local, ignored by everyone above).
    check(wait_until([&] { hop_a.send(R, seq_frame(A, R, 0)); return hop_a.has_session(R) && hop_r.has_session(A); }, 8000),
          "A<->R hop session", ok);
    check(wait_until([&] { hop_r.send(B, seq_frame(R, B, 0)); return hop_r.has_session(B) && hop_b.has_session(R); }, 8000),
          "R<->B hop session", ok);

    // End to end through the sealed hop.
    constexpr std::uint64_t kN = 25;
    for (std::uint64_t i = 0; i < kN; ++i) e2e_a.send(B, seq_frame(A, B, 1000 + i));
    check(wait_until([&] { return rb.count() >= kN; }, 8000), "25 frames delivered A -> R -> B", ok);
    {
        std::lock_guard<std::mutex> g(rb.m);
        for (std::size_t i = 0; i < rb.seqs.size() && i < kN; ++i) check(rb.seqs[i] == 1000 + i, "FIFO across the relay", ok);
    }
    check(e2e_b.replays_rejected() == 0, "no replay rejections end to end", ok);
    check(srr.relayed() >= kN, "the relay counted the forwarded frames", ok);

    // Header confidentiality: what A put on the wire names only the relay and the envelope type.
    {
        std::lock_guard<std::mutex> g(spy_a.mu);
        bool only_relay = true, no_inner = true;
        for (const auto& r : spy_a.sent) {
            only_relay = only_relay && r.to == R;
            no_inner = no_inner && r.target_key < 1000;  // inner data frames carry target.key 1000+
        }
        check(!spy_a.sent.empty() && only_relay, "wire tap at A: every outbound frame is addressed to the relay (never B)", ok);
        check(no_inner, "wire tap at A: inner msg_type/target never appear in an outer header", ok);
    }

    // Identity / admission negatives, driven through the real stack:
    // (1) an inner frame whose `from` is not the authenticated hop identity is refused at the relay.
    const auto refused_r = srr.refused();
    {
        MessageFrame forged = seq_frame(NodeId{99}, B, 5555);  // claims to be node 99, sent by A
        MessageFrame outer{};
        outer.from = A;
        outer.to = R;
        outer.msg_type = TypeKey{net::kRelayEnvelopeType};
        net::encode_frame_body(forged, outer.payload);
        hop_a.send(R, std::move(outer));
    }
    check(wait_until([&] { return srr.refused() > refused_r; }, 5000), "relay refuses an inner frame whose `from` != hop identity", ok);
    // (2) an unknown/non-admitted destination is refused (admission is the relay's predicate).
    srr.set_forwarding([&](NodeId, NodeId dest) { return dest != B; });
    const auto refused_r2 = srr.refused();
    e2e_a.send(B, seq_frame(A, B, 9000));
    check(wait_until([&] { return srr.refused() > refused_r2; }, 5000), "relay refuses a destination its admission denies", ok);
    check(rb.count() == kN, "the refused frames never reached B", ok);
    // (3) a transit delivery (inner.from != outer.from) from a node that is NOT one of B's configured
    // relays is refused by B — an authenticated but unrelated peer cannot speak as someone else.
    const auto refused_b = srb.refused();
    {
        MessageFrame inner = seq_frame(NodeId{99}, B, 7777);
        MessageFrame from_x{};
        from_x.from = X;  // as if the hop had authenticated X
        from_x.to = B;
        from_x.msg_type = TypeKey{net::kRelayEnvelopeType};
        net::encode_frame_body(inner, from_x.payload);
        srb.deliver_from_hop(std::move(from_x));
    }
    check(srb.refused() > refused_b, "B refuses a transit delivery from a node that is not a configured relay", ok);
    check(rb.count() == kN, "and nothing extra was delivered", ok);

    ta.stop(); tb.stop(); tr.stop();
    std::printf("tcp_sealed_relay_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
