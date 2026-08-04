// Tests ADR-040 Phase 3 — the HandshakeEngine seam (handshake.hpp) wired into SecureTransport
// (secure_transport.hpp): FrameKind::Authenticate demux, glare-free client/server role assignment
// (mirrors cluster.hpp's keep_local_dial rule — the lower NodeId always ends up the client, no wire
// negotiation), S2 ("no session ⇒ no delivery" — sends before a handshake completes are dropped), and
// identity-mismatch rejection (closes S6's leak-on-rejection DoS: nothing is allocated into the
// session map until identity is verified).
//
// Uses a deterministic MockHandshakeEngine/Factory test double (no mbedTLS needed — that's Phase 4) —
// a 1-round-trip protocol: client sends ClientHello{self,expected_peer,cluster}, server validates it,
// replies ServerHello{self,expected_peer,cluster} and is immediately Done; the client validates the
// reply and is Done. Both sides derive complementary directional MockCipher keys from (min,max)(self,
// peer) so client.send_cipher key == server.recv_cipher key and vice versa.
//
// QueuedTransport (NOT the synchronous LoopbackTransport) lets the test PAUSE mid-handshake and
// inspect intermediate state — a synchronous transport would cascade the whole 2-message exchange
// within one send() call, hiding the pending state this test wants to observe.
#include <cstdio>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

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

// Server engine: one-shot — a single advance(ClientHello) call validates the peer's claim and, on
// success, replies ServerHello and is immediately Done.
class MockServerEngine final : public HandshakeEngine {
public:
    MockServerEngine(NodeId self, NodeId peer, ClusterId cluster, bool force_fail, bool revoked_hit)
        : self_(self), peer_(peer), cluster_(cluster), force_fail_(force_fail), revoked_hit_(revoked_hit) {}

    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (force_fail_) { failure_ = "forced failure (test control)"; return Step::Failed; }
        if (revoked_hit_) { failure_ = "peer fingerprint is revoked"; return Step::Failed; }
        if (in.size() != 24) { failure_ = "malformed ClientHello"; return Step::Failed; }
        if (get_u64(in.subspan(0, 8)) != peer_.value || get_u64(in.subspan(8, 8)) != self_.value ||
            get_u64(in.subspan(16, 8)) != cluster_.value) {
            failure_ = "ClientHello identity mismatch";
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
        auto lo_to_hi = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));
        auto hi_to_lo = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));
        Result r;
        r.send_cipher = hi_to_lo;  // self_ == hi (server is always the higher id)
        r.recv_cipher = lo_to_hi;
        r.peer_fingerprint[0] = static_cast<std::byte>(peer_.value & 0xFF);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }

    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    NodeId self_, peer_;
    ClusterId cluster_;
    bool force_fail_, revoked_hit_;
    std::string failure_;
};

// Client engine: two advance() calls — the first (in={}) produces ClientHello and returns WantWrite;
// the second (fed the ServerHello) validates and returns Done.
class MockClientEngine final : public HandshakeEngine {
public:
    MockClientEngine(NodeId self, NodeId peer, ClusterId cluster, bool force_fail, bool revoked_hit)
        : self_(self), peer_(peer), cluster_(cluster), force_fail_(force_fail), revoked_hit_(revoked_hit) {}

    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (force_fail_) { failure_ = "forced failure (test control)"; return Step::Failed; }
        if (revoked_hit_) { failure_ = "peer fingerprint is revoked"; return Step::Failed; }
        if (!sent_hello_) {
            put_u64(out, self_.value);
            put_u64(out, peer_.value);
            put_u64(out, cluster_.value);
            sent_hello_ = true;
            return Step::WantWrite;
        }
        if (in.size() != 24) { failure_ = "malformed ServerHello"; return Step::Failed; }
        if (get_u64(in.subspan(0, 8)) != peer_.value || get_u64(in.subspan(8, 8)) != self_.value ||
            get_u64(in.subspan(16, 8)) != cluster_.value) {
            failure_ = "ServerHello identity mismatch";
            return Step::Failed;
        }
        return Step::Done;
    }

    Result take_result() override {
        const std::uint64_t lo = self_.value < peer_.value ? self_.value : peer_.value;
        const std::uint64_t hi = self_.value < peer_.value ? peer_.value : self_.value;
        auto lo_to_hi = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));
        auto hi_to_lo = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));
        Result r;
        r.send_cipher = lo_to_hi;  // self_ == lo (client is always the lower id)
        r.recv_cipher = hi_to_lo;
        r.peer_fingerprint[0] = static_cast<std::byte>(peer_.value & 0xFF);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }

    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    NodeId self_, peer_;
    ClusterId cluster_;
    bool force_fail_, revoked_hit_;
    bool sent_hello_ = false;
    std::string failure_;
};

class MockHandshakeFactory final : public HandshakeEngineFactory {
public:
    void force_fail_peer(NodeId peer) { force_fail_.insert(peer.value); }

    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer,
                                            ClusterId cluster_id, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>> revoked)
        override {
        ++created_;
        bool revoked_hit = false;
        if (revoked) {
            Fingerprint fp{};
            fp[0] = static_cast<std::byte>(expected_peer.value & 0xFF);
            revoked_hit = revoked->contains(fp);
        }
        const bool fail = force_fail_.contains(expected_peer.value);
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id, fail, revoked_hit);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id, fail, revoked_hit);
    }

    [[nodiscard]] int created() const noexcept { return created_; }

private:
    std::unordered_set<std::uint64_t> force_fail_;
    int created_ = 0;
};

// A DEFERRED-delivery fabric: send() queues, pump_one() delivers the oldest queued frame. Unlike
// LoopbackFabric (synchronous, cascades a whole multi-hop exchange within one call), this lets the
// test observe intermediate handshake state between hops.
class QueuedFabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> receiver) { receivers_[n] = std::move(receiver); }
    void send(NodeId to, MessageFrame frame) { queue_.push_back({to, std::move(frame)}); }
    bool pump_one() {
        if (queue_.empty()) return false;
        auto [to, frame] = std::move(queue_.front());
        queue_.pop_front();
        const auto it = receivers_.find(to);
        if (it != receivers_.end()) it->second(std::move(frame));
        return true;
    }
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

private:
    std::unordered_map<NodeId, std::function<void(MessageFrame)>> receivers_;
    std::deque<std::pair<NodeId, MessageFrame>> queue_;
};

class QueuedTransport final : public Transport {
public:
    QueuedTransport(QueuedFabric& fabric, NodeId self) noexcept : fabric_(&fabric), self_(self) {}
    void send(NodeId to, MessageFrame frame) override { fabric_->send(to, std::move(frame)); }
    void on_receive(std::function<void(MessageFrame)> cb) override { fabric_->attach(self_, std::move(cb)); }

private:
    QueuedFabric* fabric_;
    NodeId self_;
};

[[nodiscard]] MessageFrame make_data_frame(NodeId from, NodeId to) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    f.payload = {std::byte{9}};
    return f;
}
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{777};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});

    // --- Handshake completes over a QueuedFabric; S2 (no session ⇒ no delivery) observed mid-flight. -
    {
        QueuedFabric fabric;
        const NodeId n1{1}, n2{2};  // n1 < n2 ⇒ n1 is the client (glare-free by construction)
        QueuedTransport n1_inner(fabric, n1), n2_inner(fabric, n2);
        MockCipher unused_cipher(0);  // constructor arg; irrelevant once a session comes from the handshake
        SecureTransport n1_secure(n1_inner, unused_cipher, n1);
        SecureTransport n2_secure(n2_inner, unused_cipher, n2);
        IdentityMaterial identity1(identity_blob), identity2(identity_blob);
        TrustStore trust1(trust_blob), trust2(trust_blob);
        MockHandshakeFactory factory1, factory2;
        n1_secure.enable_handshake(factory1, cluster, identity1, trust1);
        n2_secure.enable_handshake(factory2, cluster, identity2, trust2);

        int delivered2 = 0;
        n2_secure.on_receive([&](MessageFrame) { ++delivered2; });
        n1_secure.on_receive([](MessageFrame) {});

        n1_secure.send(n2, make_data_frame(n1, n2));  // triggers ensure_handshake; frame itself is dropped
        check(n1_secure.handshake_pending_dropped() == 1, "S2: first send before a session exists is dropped", ok);
        check(n1_secure.pending_handshake_count() == 1, "n1 has a handshake in flight (as client)", ok);
        check(factory2.created() == 0, "n2 has not yet created a server engine (nothing arrived)", ok);
        check(fabric.pending() == 1, "ClientHello queued, not yet delivered", ok);
        check(n1_secure.session_count() == 0 && n2_secure.session_count() == 0, "no session on either side yet", ok);

        fabric.pump_one();  // ClientHello -> n2: creates server engine, replies ServerHello, Done
        check(factory2.created() == 1, "n2 created exactly one server engine", ok);
        check(n2_secure.session_count() == 1, "n2 installs its session as soon as it validates the client", ok);
        check(n1_secure.session_count() == 0, "n1 still waiting on the ServerHello reply", ok);
        check(fabric.pending() == 1, "ServerHello queued back to n1", ok);

        fabric.pump_one();  // ServerHello -> n1: validates, Done, installs session
        check(n1_secure.session_count() == 1, "n1 installs its session after validating the ServerHello", ok);
        check(n1_secure.pending_handshake_count() == 0, "n1's in-flight handshake is cleared", ok);
        check(fabric.pending() == 0, "nothing left queued — exactly 2 messages for this protocol", ok);

        // Now real data flows both ways without being dropped.
        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_one();
        check(delivered2 == 1, "post-handshake data is delivered", ok);
        check(n1_secure.handshake_pending_dropped() == 1, "no NEW drop — the session already exists", ok);
    }

    // --- Glare-free role assignment: whoever calls send() first, only the LOWER NodeId becomes client. -
    {
        QueuedFabric fabric;
        const NodeId lo{10}, hi{20};
        QueuedTransport hi_inner(fabric, hi), lo_inner(fabric, lo);
        MockCipher unused_cipher(0);
        SecureTransport hi_secure(hi_inner, unused_cipher, hi);
        SecureTransport lo_secure(lo_inner, unused_cipher, lo);
        IdentityMaterial identity_hi(identity_blob), identity_lo(identity_blob);
        TrustStore trust_hi(trust_blob), trust_lo(trust_blob);
        MockHandshakeFactory factory_hi, factory_lo;
        hi_secure.enable_handshake(factory_hi, cluster, identity_hi, trust_hi);
        lo_secure.enable_handshake(factory_lo, cluster, identity_lo, trust_lo);

        // The HIGHER id attempts to send FIRST — it must NOT self-initiate as client.
        hi_secure.send(lo, make_data_frame(hi, lo));
        check(hi_secure.pending_handshake_count() == 0,
              "higher-id side never self-initiates a handshake toward a lower id (waits passively)", ok);
        check(fabric.pending() == 0, "nothing queued from the higher-id attempt", ok);

        lo_secure.send(hi, make_data_frame(lo, hi));
        check(lo_secure.pending_handshake_count() == 1, "lower-id side initiates as client", ok);
        check(fabric.pending() == 1, "lower-id side's ClientHello is queued", ok);
    }

    // --- Identity mismatch: a peer claiming the wrong NodeId is rejected; no session leaks in. -------
    {
        QueuedFabric fabric;
        const NodeId n1{1}, n2{2};
        QueuedTransport n1_inner(fabric, n1), n2_inner(fabric, n2);
        MockCipher unused_cipher(0);
        SecureTransport n1_secure(n1_inner, unused_cipher, n1);
        SecureTransport n2_secure(n2_inner, unused_cipher, n2);
        IdentityMaterial identity1(identity_blob), identity2(identity_blob);
        TrustStore trust1(trust_blob), trust2(trust_blob);
        MockHandshakeFactory factory1, factory2;
        factory2.force_fail_peer(n1);  // n2's server-side engine toward n1 always fails (S3-shaped control)
        n1_secure.enable_handshake(factory1, cluster, identity1, trust1);
        n2_secure.enable_handshake(factory2, cluster, identity2, trust2);

        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_one();  // ClientHello -> n2's engine, which is forced to fail
        check(n2_secure.session_count() == 0, "S6: a failed handshake leaks NO session", ok);
        check(fabric.pending() == 0, "a Failed engine sends nothing back", ok);
    }

    std::printf("security_handshake_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
