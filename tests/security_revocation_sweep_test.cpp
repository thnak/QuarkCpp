// Tests ADR-040 Phase 5 — `RevocationRegistry` (revocation_registry.hpp) and its two consumers:
//   (1) `SecureTransport::sweep_revocations` (secure_transport.hpp): S5, enforcement against a
//       session that is ALREADY OPEN. A live session's send()/deliver() must start dropping
//       unconditionally within the SAME sweep call that revokes its peer fingerprint — no grace
//       period, no additional round trip.
//   (2) `SwimMembership::set_revocation_gossip` (cluster.hpp): a fingerprint revoked LOCALLY on one
//       node propagates to another purely via the EXISTING SWIM gossip channel (no new wire protocol),
//       converging the same way roster membership does (021 §3).
// Structured after security_handshake_test.cpp (QueuedFabric/MockHandshakeFactory, so a session can be
// established deterministically without mbedTLS) and cluster_swim_failure_detection_test.cpp
// (LoopbackFabric + virtual clock for the gossip-convergence half).
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
#include "quark/core/cluster.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metrics.hpp"
#include "quark/core/revocation_registry.hpp"
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

// A fingerprint derived deterministically from a NodeId, so both sides agree on "the peer's
// fingerprint" without a real cert (mirrors security_handshake_test.cpp's MockServerEngine/
// MockClientEngine convention: r.peer_fingerprint[0] = low byte of the peer's NodeId).
[[nodiscard]] Fingerprint fp_of(NodeId n) noexcept {
    Fingerprint fp{};
    fp[0] = static_cast<std::byte>(n.value & 0xFF);
    fp[1] = static_cast<std::byte>((n.value >> 8) & 0xFF);
    return fp;
}

// One-round-trip mock handshake (identical shape to security_handshake_test.cpp), so this file does
// not need mbedTLS (Phase 4) to establish a live PeerSession.
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
        r.send_cipher = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));  // server == hi
        r.recv_cipher = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));
        r.peer_fingerprint = fp_of(peer_);
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
        r.send_cipher = std::make_shared<MockCipher>(detail::hash_combine(lo, hi));  // client == lo
        r.recv_cipher = std::make_shared<MockCipher>(detail::hash_combine(hi, lo));
        r.peer_fingerprint = fp_of(peer_);
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
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id);
    }
};

// Deferred-delivery fabric (identical to security_handshake_test.cpp's QueuedFabric) so the handshake's
// 2-message exchange can be pumped step by step.
class QueuedFabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> receiver) { receivers_[n] = std::move(receiver); }
    void send(NodeId to, MessageFrame frame) { queue_.push_back({to, std::move(frame)}); }
    void pump_all() {
        while (!queue_.empty()) {
            auto [to, frame] = std::move(queue_.front());
            queue_.pop_front();
            const auto it = receivers_.find(to);
            if (it != receivers_.end()) it->second(std::move(frame));
        }
    }

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

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{555};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});

    // ============================================================================================
    // Part 1 — RevocationRegistry: union-only ratchet, bounded sample(), snapshot immutability.
    // ============================================================================================
    {
        RevocationRegistry reg;
        check(reg.size() == 0, "fresh registry is empty", ok);
        auto snap0 = reg.snapshot();
        check(snap0 != nullptr && snap0->empty(), "initial snapshot is a valid empty set", ok);

        const Fingerprint fp_a = fp_of(NodeId{1});
        const Fingerprint fp_b = fp_of(NodeId{2});
        reg.revoke_locally(fp_a);
        check(reg.is_revoked(fp_a), "fp_a revoked after revoke_locally", ok);
        check(!reg.is_revoked(fp_b), "fp_b not revoked yet", ok);
        check(snap0->empty(), "a PRIOR snapshot is unaffected by a later mutation (immutable publish)", ok);

        const bool changed_dup = reg.merge({fp_a});
        check(!changed_dup, "merging an already-revoked fingerprint reports no change (idempotent)", ok);
        const bool changed_new = reg.merge({fp_a, fp_b});
        check(changed_new, "merging a set containing a NEW fingerprint reports a change", ok);
        check(reg.is_revoked(fp_b), "fp_b revoked after merge", ok);
        check(reg.size() == 2, "registry holds exactly the union (fp_a, fp_b)", ok);

        auto snap1 = reg.snapshot();
        check(snap1->size() == 2 && snap1->contains(fp_a) && snap1->contains(fp_b),
              "snapshot reflects the merged union", ok);

        const auto sampled = reg.sample(1);
        check(sampled.size() == 1, "sample(1) is bounded to the requested max", ok);
        const auto sampled_all = reg.sample(10);
        check(sampled_all.size() == 2, "sample(10) returns everything when max exceeds the set size", ok);
    }

    // ============================================================================================
    // Part 2 — SecureTransport::sweep_revocations: S5 enforcement against an ALREADY-OPEN session.
    // ============================================================================================
    {
        QueuedFabric fabric;
        const NodeId n1{1}, n2{2};  // n1 < n2 ⇒ n1 is the client (glare-free by construction)
        QueuedTransport n1_inner(fabric, n1), n2_inner(fabric, n2);
        MockCipher unused_cipher(0);
        SecureTransport n1_secure(n1_inner, unused_cipher, n1);
        SecureTransport n2_secure(n2_inner, unused_cipher, n2);
        IdentityMaterial identity1(identity_blob), identity2(identity_blob);
        TrustStore trust1(trust_blob), trust2(trust_blob);
        MockHandshakeFactory factory1, factory2;
        n1_secure.enable_handshake(factory1, cluster, identity1, trust1);
        n2_secure.enable_handshake(factory2, cluster, identity2, trust2);

        // ADR-040 Phase 8: wire n1's security_* counters into a real ShardCounters block.
        MetricsRegistry registry;
        ShardCounters& shard = registry.add_shard();
        n1_secure.set_metrics(shard);

        int delivered1 = 0, delivered2 = 0;
        n1_secure.on_receive([&](MessageFrame) { ++delivered1; });
        n2_secure.on_receive([&](MessageFrame) { ++delivered2; });

        // Establish a live session both ways (each side's first send triggers/completes the handshake).
        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_all();  // ClientHello -> n2 (installs), ServerHello -> n1 (installs)
        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_all();
        check(delivered2 == 1, "pre-revocation: n1 -> n2 is delivered", ok);

        n2_secure.send(n1, make_data_frame(n2, n1));
        fabric.pump_all();
        check(delivered1 == 1, "pre-revocation: n2 -> n1 is delivered", ok);

        // --- CONTROL: sweeping a set that does NOT contain the peer's fingerprint changes nothing. ---
        RevocationRegistry unrelated_reg;
        unrelated_reg.revoke_locally(fp_of(NodeId{999}));
        n1_secure.sweep_revocations(unrelated_reg.snapshot());
        check(n1_secure.revocations_enforced() == 0, "CONTROL: sweeping an unrelated fingerprint enforces nothing", ok);
        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_all();
        check(delivered2 == 2, "CONTROL: still delivered after an unrelated sweep", ok);

        // --- n1 revokes n2's fingerprint and sweeps: n1 -> n2 must stop within THIS sweep, no grace. ---
        // A PeerSession is per-PEER, not per-direction (one object covers both send_cipher_ and
        // recv_cipher_ for n2) — so revoking it from n1's side kills BOTH directions of local traffic
        // with n2: n1's own send() drops (S5), and n1's deliver() ALSO drops a frame n2 still happily
        // sends (n2 itself is unaware anything changed; only n1 has swept).
        RevocationRegistry reg1;
        reg1.revoke_locally(fp_of(n2));
        n1_secure.sweep_revocations(reg1.snapshot());
        check(n1_secure.revocations_enforced() == 1, "sweep_revocations enforced exactly the matching session", ok);
        check(registry.snapshot().security_revocations_enforced == 1, "metrics: security_revocations_enforced wired through", ok);

        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_all();
        check(delivered2 == 2, "S5: post-revocation n1 -> n2 send is never delivered", ok);
        check(n1_secure.revoked_dropped() == 1, "the revoked session's send() incremented revoked_dropped", ok);

        // n2 has NOT swept its own side — it seals and forwards normally — but n1's LOCAL session
        // record for n2 is already revoked, so n1's deliver() drops it too (no round trip needed).
        n2_secure.send(n1, make_data_frame(n2, n1));
        fabric.pump_all();
        check(delivered1 == 1, "S5: n1's deliver() also drops n2's frame — the local sweep covers both directions", ok);
        check(n1_secure.revoked_dropped() == 2, "revoked_dropped counts BOTH the send-path and deliver-path drop", ok);
        check(n2_secure.revoked_dropped() == 0, "n2 itself never swept — it has no revoked session of its own", ok);
    }

    // ============================================================================================
    // Part 2b — the same story from the OTHER side: the higher-id (server) node sweeps instead, on a
    // fresh node pair, confirming the effect is symmetric regardless of client/server role.
    // ============================================================================================
    {
        QueuedFabric fabric;
        const NodeId n3{3}, n4{4};  // n3 < n4 ⇒ n3 is the client, n4 the server
        QueuedTransport n3_inner(fabric, n3), n4_inner(fabric, n4);
        MockCipher unused_cipher(0);
        SecureTransport n3_secure(n3_inner, unused_cipher, n3);
        SecureTransport n4_secure(n4_inner, unused_cipher, n4);
        IdentityMaterial identity3(identity_blob), identity4(identity_blob);
        TrustStore trust3(trust_blob), trust4(trust_blob);
        MockHandshakeFactory factory3, factory4;
        n3_secure.enable_handshake(factory3, cluster, identity3, trust3);
        n4_secure.enable_handshake(factory4, cluster, identity4, trust4);

        int delivered3 = 0, delivered4 = 0;
        n3_secure.on_receive([&](MessageFrame) { ++delivered3; });
        n4_secure.on_receive([&](MessageFrame) { ++delivered4; });

        n3_secure.send(n4, make_data_frame(n3, n4));
        fabric.pump_all();
        n3_secure.send(n4, make_data_frame(n3, n4));
        fabric.pump_all();
        check(delivered4 == 1, "pre-revocation: n3 -> n4 is delivered", ok);

        RevocationRegistry reg4;
        reg4.revoke_locally(fp_of(n3));  // the SERVER sweeps this time
        n4_secure.sweep_revocations(reg4.snapshot());

        n3_secure.send(n4, make_data_frame(n3, n4));
        fabric.pump_all();
        check(delivered4 == 1, "S5 from the server side: n3 -> n4 no longer delivered after n4's sweep", ok);
        check(n4_secure.revoked_dropped() == 1, "n4's deliver() dropped the frame from its now-revoked session", ok);
    }

    // ============================================================================================
    // Part 3 — SwimMembership::set_revocation_gossip: a LOCAL revocation converges to a peer purely
    // via the existing SWIM gossip channel (021 §3), same convergence style as roster membership.
    // ============================================================================================
    {
        LoopbackFabric fabric;
        VClock clk;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.ack_timeout_ns = 100'000'000;
        cfg.suspicion_timeout_ns = 500'000'000;
        cfg.gossip_fanout = 3;
        cfg.seed = 0x51D3;

        LoopbackTransport t1(fabric, NodeId{1}), t2(fabric, NodeId{2});
        SwimMembership swim1(NodeId{1}, t1, cfg), swim2(NodeId{2}, t2, cfg);
        swim1.set_clock(&vclock_read, &clk);
        swim2.set_clock(&vclock_read, &clk);
        (void)swim1.admit(NodeId{2}, cluster);
        (void)swim2.admit(NodeId{1}, cluster);

        RevocationRegistry reg1, reg2;
        swim1.set_revocation_gossip([&reg1](std::size_t max) { return reg1.sample(max); },
                                    [&reg1](const std::vector<Fingerprint>& in) { reg1.merge(in); });
        swim2.set_revocation_gossip([&reg2](std::size_t max) { return reg2.sample(max); },
                                    [&reg2](const std::vector<Fingerprint>& in) { reg2.merge(in); });

        const Fingerprint compromised = fp_of(NodeId{13});
        check(!reg2.is_revoked(compromised), "node 2 has not heard of the revocation yet", ok);
        reg1.revoke_locally(compromised);  // the LOCAL origin of the revocation, node 1 only

        bool converged = false;
        for (int round = 0; round < 50 && !converged; ++round) {
            clk.now += 120'000'000;
            swim1.tick();
            swim2.tick();
            converged = reg2.is_revoked(compromised);
        }
        check(converged, "a locally-revoked fingerprint reaches the peer via the existing gossip channel", ok);
        check(reg1.is_revoked(compromised), "the originating node still has it revoked (ratchet, not consumed)", ok);
    }

    std::printf("security_revocation_sweep_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
