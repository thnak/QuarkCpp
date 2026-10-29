// Tests ADR-067 D3 — the CLIENT identity role (NodeId bit 63, `quark-client:` CN) is authenticated but
// is never a member. Std-only (mock handshake, loopback SWIM); the TCP relay half is in
// tcp_relay_forwarding_test.cpp and the real-certificate half in tests/adapters/mbedtls_handshake_check.cpp.
//
// Load-bearing properties, each with a positive control:
//   1. role rules: the client's dial always survives dedup, whatever the numeric ids;
//   2. handshake role: a client whose id is numerically HIGHER than the member's still gets a session
//      (it is the TLS client), and a member never initiates a handshake toward a client;
//   3. SWIM: a client's Control frames are dropped undecoded and a client id can never be admitted,
//      gossiped or joined into any membership view (so it is never a placement target);
//   4. placement: a client id is never eligible even under an admit-all authority;
//   5. revocation: a revoked CLIENT session is actively closed (reset hook), a revoked member is not.
#include <cstdint>
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
#include "quark/core/membership.hpp"
#include "quark/core/node_authority.hpp"
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

// Deterministic per-node fingerprint (low 16 bits of the id plus the client bit), as in
// security_revocation_sweep_test.cpp, so both sides agree without a real certificate.
[[nodiscard]] Fingerprint fp_of(NodeId n) noexcept {
    Fingerprint fp{};
    fp[0] = static_cast<std::byte>(n.value & 0xFF);
    fp[1] = static_cast<std::byte>((n.value >> 8) & 0xFF);
    fp[2] = static_cast<std::byte>(is_client_node(n) ? 1 : 0);
    return fp;
}

// One-round-trip mock handshake (same shape as security_revocation_sweep_test.cpp). The cipher keys
// depend only on {lo, hi} and on which side is the TLS client, so they work whichever id is lower.
class MockServerEngine final : public HandshakeEngine {
public:
    MockServerEngine(NodeId self, NodeId peer, ClusterId cluster, bool refuse)
        : self_(self), peer_(peer), cluster_(cluster), refuse_(refuse) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (refuse_) {  // stands in for a revoked / untrusted peer certificate
            failure_ = "peer certificate refused";
            return Step::Failed;
        }
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
        r.peer_fingerprint = fp_of(peer_);
        r.peer_node_id = peer_;
        r.peer_cluster_id = cluster_;
        return r;
    }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    NodeId self_, peer_;
    ClusterId cluster_;
    bool refuse_;
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
    bool refuse_as_server = false;  // every server-role handshake fails (a refused certificate)
    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer,
                                            ClusterId cluster_id, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id, refuse_as_server);
    }
};

// Deferred-delivery fabric; `in_flight()` exposes whether anything was sent at all.
class QueuedFabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> receiver) { receivers_[n] = std::move(receiver); }
    void send(NodeId to, MessageFrame frame) { queue_.push_back({to, std::move(frame)}); }
    [[nodiscard]] std::size_t in_flight() const noexcept { return queue_.size(); }
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

// One member and one peer (client or member) with handshake-gated SecureTransports over a QueuedFabric.
struct Pair {
    QueuedFabric fabric;
    NodeId member, peer;
    QueuedTransport member_inner, peer_inner;
    MockCipher unused{0};
    SecureTransport member_secure, peer_secure;
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial member_id{id_blob}, peer_id{id_blob};
    TrustStore member_trust{trust_blob}, peer_trust{trust_blob};
    MockHandshakeFactory member_factory, peer_factory;
    int to_member = 0, to_peer = 0;
    std::vector<NodeId> resets;

    Pair(NodeId m, NodeId p, ClusterId cluster)
        : member(m), peer(p), member_inner(fabric, m), peer_inner(fabric, p),
          member_secure(member_inner, unused, m), peer_secure(peer_inner, unused, p) {
        member_secure.enable_handshake(member_factory, cluster, member_id, member_trust);
        peer_secure.enable_handshake(peer_factory, cluster, peer_id, peer_trust);
        member_secure.on_receive([this](MessageFrame) { ++to_member; });
        peer_secure.on_receive([this](MessageFrame) { ++to_peer; });
        member_secure.set_reset_hook([this](NodeId n) { resets.push_back(n); });
    }
};
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{777};
    const NodeId m1{1}, m2{2};
    const NodeId c5{kClientNodeBit | 5}, c6{kClientNodeBit | 6};

    // ============================================================================================
    // 1. Role rules (cluster.hpp): the client's dial survives against a member; same-role pairs keep
    //    lower-id-wins. Both ends must always give complementary answers (else dedup kills both sockets).
    // ============================================================================================
    check(keep_local_dial(m1, m2) && !keep_local_dial(m2, m1), "control: members keep lower-id-wins", ok);
    check(!keep_local_dial(m1, c5), "a member never keeps its own dial to a client", ok);
    check(keep_local_dial(c5, m1), "a client always keeps its own dial to a member", ok);
    check(keep_local_dial(c5, c6) != keep_local_dial(c6, c5), "two clients give complementary answers", ok);
    check(dial_winner(m1, c5) == c5 && dial_winner(c5, m1) == c5, "dial_winner names the client either way", ok);
    check(dial_winner(c5, c6) == dial_winner(c6, c5), "dial_winner is symmetric between two clients", ok);
    check(dial_winner(m2, m1) == m1, "control: dial_winner between members is the lower id", ok);

    // ============================================================================================
    // 2. Handshake role: a numerically HIGHER client still gets a session; a member never initiates.
    // ============================================================================================
    {
        Pair p(m2, c5, cluster);
        p.member_secure.send(c5, make_data_frame(m2, c5));  // member first, no session yet
        check(p.fabric.in_flight() == 0, "a member never initiates a handshake toward a client", ok);
        check(p.to_peer == 0, "and its frame is not delivered (S2: no session, no delivery)", ok);

        p.peer_secure.send(m2, make_data_frame(c5, m2));  // client opens the handshake
        check(p.fabric.in_flight() == 1, "the client opens the handshake although its id is higher", ok);
        p.fabric.pump_all();
        p.peer_secure.send(m2, make_data_frame(c5, m2));
        p.fabric.pump_all();
        check(p.to_member == 1, "client -> member delivered over the client-opened session", ok);
        p.member_secure.send(c5, make_data_frame(m2, c5));
        p.fabric.pump_all();
        check(p.to_peer == 1, "member -> client reply delivered over the same session", ok);
    }
    {  // control: between two members the lower id still opens (unchanged behaviour)
        Pair p(m1, m2, cluster);
        p.peer_secure.send(m1, make_data_frame(m2, m1));
        check(p.fabric.in_flight() == 0, "control: the higher member id does not open toward a lower one", ok);
        p.member_secure.send(m2, make_data_frame(m1, m2));
        check(p.fabric.in_flight() == 1, "control: the lower member id opens", ok);
    }
    {  // two clients: exactly one side opens (the lower id), and a session forms
        Pair p(c5, c6, cluster);
        p.peer_secure.send(c5, make_data_frame(c6, c5));
        check(p.fabric.in_flight() == 0, "two clients: the higher id does not open", ok);
        p.member_secure.send(c6, make_data_frame(c5, c6));
        check(p.fabric.in_flight() == 1, "two clients: the lower id opens", ok);
        p.fabric.pump_all();
        p.member_secure.send(c6, make_data_frame(c5, c6));
        p.fabric.pump_all();
        check(p.to_peer == 1, "two clients: a session forms and delivers", ok);
    }

    // ============================================================================================
    // 2b. A client whose INITIAL handshake fails (e.g. a revoked certificate) is disconnected; a
    //     member peer's failed handshake is not (unchanged: no session, nothing to reset).
    // ============================================================================================
    {
        Pair p(m2, c5, cluster);
        p.member_factory.refuse_as_server = true;
        p.peer_secure.send(m2, make_data_frame(c5, m2));
        p.fabric.pump_all();
        check(!p.member_secure.has_session(c5), "the refused client has no session", ok);
        check(p.resets.size() == 1 && p.resets[0] == c5, "a client's failed initial handshake closes its socket", ok);
    }
    {
        Pair p(m2, m1, cluster);  // m1 (lower) opens toward m2, whose server-side handshake fails
        p.member_factory.refuse_as_server = true;
        p.peer_secure.send(m2, make_data_frame(m1, m2));
        p.fabric.pump_all();
        check(!p.member_secure.has_session(m1), "control: the refused member has no session", ok);
        check(p.resets.empty(), "control: a member's failed initial handshake does not reset", ok);
    }

    // ============================================================================================
    // 5. Revocation: a revoked CLIENT session is actively closed; a revoked MEMBER session is not
    //    (its SWIM suspicion cycle closes it, unchanged).
    // ============================================================================================
    {
        Pair p(m2, c5, cluster);
        p.peer_secure.send(m2, make_data_frame(c5, m2));
        p.fabric.pump_all();
        RevocationRegistry reg;
        reg.revoke_locally(fp_of(c5));
        p.member_secure.sweep_revocations(reg.snapshot());
        check(p.member_secure.revocations_enforced() == 1, "the client's session is revoked", ok);
        check(p.resets.size() == 1 && p.resets[0] == c5, "a revoked client session is actively closed", ok);
        check(!p.member_secure.has_session(c5), "the revoked client has no usable session", ok);
        const int before = p.to_member;
        p.peer_secure.send(m2, make_data_frame(c5, m2));  // the client still believes its session is live
        p.fabric.pump_all();
        check(p.to_member == before, "nothing from the revoked client is delivered afterwards", ok);
        p.member_secure.sweep_revocations(reg.snapshot());
        check(p.resets.size() == 1, "an already-revoked session is not closed twice", ok);

        // The revoked session must be ERASED, not merely marked: when the client comes back with a
        // re-issued certificate (the mock keeps the fingerprint; only the handshake matters here), the
        // member must build a FRESH session. Left in place, the new keys would be swapped into the
        // revoked session and the client blackholed for good (a relayed client has no socket whose
        // close would otherwise retire it).
        p.peer_secure.on_peer_disconnected(m2);  // the client side reconnects
        p.peer_secure.send(m2, make_data_frame(c5, m2));
        p.fabric.pump_all();
        p.peer_secure.send(m2, make_data_frame(c5, m2));
        p.fabric.pump_all();
        check(p.to_member == before + 1, "a re-enrolled client gets a fresh, working session", ok);
    }
    {
        Pair p(m1, m2, cluster);
        p.member_secure.send(m2, make_data_frame(m1, m2));
        p.fabric.pump_all();
        RevocationRegistry reg;
        reg.revoke_locally(fp_of(m2));
        p.member_secure.sweep_revocations(reg.snapshot());
        check(p.member_secure.revocations_enforced() == 1, "control: the member's session is revoked", ok);
        check(p.resets.empty(), "control: a revoked member session is left to SWIM (no active close)", ok);
    }

    // ============================================================================================
    // 3. SWIM: a client never enters membership — not by its own Join, not by local admit, not by
    //    gossip — and its Control frames are dropped undecoded.
    // ============================================================================================
    {
        LoopbackFabric fabric;
        VClock clk;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.ack_timeout_ns = 100'000'000;
        cfg.suspicion_timeout_ns = 500'000'000;
        cfg.gossip_fanout = 3;
        cfg.seed = 0xC11E;

        LoopbackTransport t1(fabric, m1), t2(fabric, m2), tc(fabric, c5), t3(fabric, NodeId{3});
        SwimMembership s1(m1, t1, cfg), s2(m2, t2, cfg), sc(c5, tc, cfg), s3(NodeId{3}, t3, cfg);
        for (SwimMembership* s : {&s1, &s2, &sc, &s3}) s->set_clock(&vclock_read, &clk);

        check(!s1.admit(c5, cluster), "admit() refuses a client id", ok);
        check(!s1.view().contains(c5), "a locally admitted client id is absent from the view", ok);
        check(s1.admit(m2, cluster) && s2.admit(m1, cluster), "control: members admit each other", ok);

        sc.request_join(m1);  // a client speaking SWIM: Join toward a member
        s3.request_join(m1);  // control: a member doing the same
        check(!s2.admit(c5, cluster), "admit() refuses a client id on every member", ok);
        for (int round = 0; round < 40; ++round) {
            clk.now += 120'000'000;
            for (SwimMembership* s : {&s1, &s2, &sc, &s3}) s->tick();
        }
        check(s1.view().contains(NodeId{3}), "control: a member's Join is admitted", ok);
        check(s1.client_control_drops() > 0, "the client's Control frames were dropped at the door", ok);

        // A MEMBER (authenticated, so its frame passes the door) gossips an update naming the client id —
        // the apply_update choke point must refuse it. Control: the same update naming a member id
        // creates (an unverified) entry, so the frame itself is accepted and decoded.
        ControlMsg gossip;
        gossip.kind = ControlKind::Gossip;
        gossip.cluster = cluster;
        gossip.from = m2;
        gossip.from_incarnation = s2.self_incarnation();
        gossip.wire_version = cfg.wire_version;
        gossip.updates = {MemberUpdate{c5.value, 1, static_cast<std::uint8_t>(MemberStatus::Alive)},
                          MemberUpdate{9, 1, static_cast<std::uint8_t>(MemberStatus::Alive)}};
        MessageFrame f{};
        f.from = m2;
        f.to = m1;
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(gossip);
        t2.send(m1, std::move(f));
        check(s1.is_known(NodeId{9}), "control: a member's gossip naming a member id is absorbed", ok);
        check(!s1.is_known(c5), "a member's gossip naming a client id is refused at apply_update", ok);
        for (SwimMembership* s : {&s1, &s2, &s3}) {
            check(!s->view().contains(c5), "the client is absent from every member's view", ok);
            check(!s->placement_view().contains(c5), "the client is absent from every placement view", ok);
            check(!s->is_known(c5), "no member holds any record of the client id", ok);
        }

        // 4. Placement: even an admit-all authority never makes a client eligible.
        auto nodes = std::make_shared<const std::vector<NodeId>>(std::vector<NodeId>{m1, m2, c5});
        const MembershipView forged(nodes, 1);
        AdmitAllNodeAuthority all;
        std::vector<NodeId> eligible;
        admitted_nodes(forged, all, eligible);
        check(eligible.size() == 2 && eligible[0] == m1 && eligible[1] == m2,
              "a client id in a (forged) view is never placement-eligible", ok);
        bool never_client = true;
        for (std::uint64_t a = 1; a <= 2000; ++a) {
            const auto winner = place_admitted(ActorId{TypeKey{0x5EED}, a}, forged, all);
            if (!winner || is_client_node(*winner)) never_client = false;
        }
        check(never_client, "2000 placements over that view never land on the client", ok);
    }

    std::printf("security_client_role_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
