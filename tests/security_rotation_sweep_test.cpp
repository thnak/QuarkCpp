// Tests ADR-040 Phase 6 — `SecureTransport::sweep_rotation` / `try_renegotiate` (secure_transport.hpp)
// and `TcpTransport::reset_peer_connection` (wired only as the reset_hook_ contract here, via a mock —
// tcp_transport_reconnect_test.cpp separately proves the real reconnect machinery reset_peer_connection
// leans on). Three independent triggers, each mirroring a proven claim from the design record:
//   C5  — frames sealed under the current key past a volume threshold force a renegotiate.
//   C15 — a fully IDLE session (zero application traffic) still gets rekeyed once the poll interval
//         elapses; and C1 — it is NOT touched before that interval elapses (no premature disruption).
//   C2  — a renegotiation that FAILS drops the stale session and invokes the reset hook, so 021's
//         existing reconnect/redial machinery (not reinvented here) can bring up a fresh generation;
//         the next send() correctly falls back to S2's "no session ⇒ trigger a fresh handshake" path,
//         and a fresh handshake still succeeds once the underlying problem clears.
// Structured after security_handshake_test.cpp / security_revocation_sweep_test.cpp's QueuedFabric +
// MockHandshakeFactory doubles (no mbedTLS needed — that's Phase 4).
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
#include "quark/core/metrics.hpp"
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

[[nodiscard]] Fingerprint fp_of(NodeId n) noexcept {
    Fingerprint fp{};
    fp[0] = static_cast<std::byte>(n.value & 0xFF);
    return fp;
}

// One-round-trip mock handshake, reused unchanged for BOTH an initial handshake and a renegotiation —
// SecureTransport itself is what tells the two cases apart (S2's "a session already exists" discriminant,
// see secure_transport.hpp's install_session_from_result banner), not the engine.
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
    MockClientEngine(NodeId self, NodeId peer, ClusterId cluster, bool force_fail)
        : self_(self), peer_(peer), cluster_(cluster), force_fail_(force_fail) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (force_fail_) {
            failure_ = "forced failure (test control — simulates a renegotiation the peer rejects)";
            return Step::Failed;
        }
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
    bool force_fail_;
    bool sent_hello_ = false;
    std::string failure_;
};

class MockHandshakeFactory final : public HandshakeEngineFactory {
public:
    void force_fail_peer(NodeId peer) { force_fail_.insert(peer.value); }
    void clear_force_fail(NodeId peer) { force_fail_.erase(peer.value); }

    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer,
                                            ClusterId cluster_id, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        ++created_;
        const bool fail = force_fail_.contains(expected_peer.value);
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id, fail);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id);
    }
    [[nodiscard]] int created() const noexcept { return created_; }

private:
    std::unordered_set<std::uint64_t> force_fail_;
    int created_ = 0;
};

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

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{333};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});

    // ============================================================================================
    // Part A — C5: frames sealed under the current key past the threshold force a renegotiate, purely
    // driven by application traffic (no idle-timer involvement — set that trigger's interval huge).
    // ============================================================================================
    {
        QueuedFabric fabric;
        VClock clk;
        const NodeId n1{1}, n2{2};  // n1 < n2 ⇒ n1 is the client (glare-free by construction)
        QueuedTransport n1_inner(fabric, n1), n2_inner(fabric, n2);
        MockCipher unused_cipher(0);
        SecureTransport n1_secure(n1_inner, unused_cipher, n1);
        SecureTransport n2_secure(n2_inner, unused_cipher, n2);
        n1_secure.set_clock(&vclock_read, &clk);
        n2_secure.set_clock(&vclock_read, &clk);
        n1_secure.set_rotation_policy(/*max_frames_per_key=*/3, /*renegotiate_interval_ns=*/1'000'000'000'000LL);
        IdentityMaterial identity1(identity_blob), identity2(identity_blob);
        TrustStore trust1(trust_blob), trust2(trust_blob);
        MockHandshakeFactory factory1, factory2;
        n1_secure.enable_handshake(factory1, cluster, identity1, trust1);
        n2_secure.enable_handshake(factory2, cluster, identity2, trust2);

        // ADR-040 Phase 8: wire n1's own security_* counters into a real ShardCounters block, so this
        // scenario also proves the metrics.hpp wiring (not just SecureTransport's own diagnostic atomics).
        MetricsRegistry registry;
        ShardCounters& shard = registry.add_shard();
        n1_secure.set_metrics(shard);

        int delivered2 = 0;
        n2_secure.on_receive([&](MessageFrame) { ++delivered2; });
        n1_secure.on_receive([](MessageFrame) {});

        n1_secure.send(n2, make_data_frame(n1, n2));  // triggers + completes the initial handshake
        fabric.pump_all();
        check(registry.snapshot().security_handshakes_attempted == 1, "metrics: 1 handshake attempted (initial)", ok);
        check(registry.snapshot().security_handshakes_succeeded == 1, "metrics: 1 handshake succeeded (initial, not a rotation)", ok);
        for (int i = 0; i < 3; ++i) {
            n1_secure.send(n2, make_data_frame(n1, n2));
            fabric.pump_all();
        }
        check(delivered2 == 3, "3 frames sealed under generation 0's key", ok);
        check(n1_secure.renegotiations_completed() == 0, "no renegotiation yet — sweep hasn't run", ok);

        n1_secure.sweep_rotation(clk.now);  // frames_sealed_on_key() == 3 >= threshold ⇒ renegotiate
        fabric.pump_all();                  // completes the 2-message renegotiation exchange
        check(n1_secure.renegotiations_attempted() == 1, "C5: sweep_rotation triggered exactly one renegotiate", ok);
        check(n1_secure.renegotiations_completed() == 1, "the renegotiation completed successfully", ok);
        check(n1_secure.session_count() == 1, "in-place swap: still exactly one session for n2 (not a new entry)", ok);
        check(n2_secure.renegotiations_completed() == 1, "n2's server-role side also swapped in place", ok);
        {
            const MetricsSnapshot snap = registry.snapshot();
            check(snap.security_handshakes_attempted == 2, "metrics: 2nd attempt recorded (the renegotiate)", ok);
            check(snap.security_rotations_completed == 1, "metrics: the renegotiate is counted as a ROTATION, not a handshake", ok);
            check(snap.security_handshakes_succeeded == 1, "metrics: handshakes_succeeded stays at 1 (unchanged by the rotation)", ok);
        }

        // Post-renegotiate traffic still flows (fresh nonce space, seq reset symmetrically on both sides).
        n1_secure.send(n2, make_data_frame(n1, n2));
        fabric.pump_all();
        check(delivered2 == 4, "traffic flows normally immediately after the in-place renegotiate", ok);

        // CONTROL: frames_sealed_on_key() just reset to 1 (well under the threshold of 3) — a sweep
        // right now must NOT thrash into another renegotiate.
        n1_secure.sweep_rotation(clk.now);
        check(n1_secure.renegotiations_attempted() == 1, "CONTROL: no thrash — still under threshold, no 2nd renegotiate", ok);
    }

    // ============================================================================================
    // Part B — C15/C1: a fully IDLE session (zero application traffic after the handshake) is rekeyed
    // once the poll interval elapses, and is NEVER touched before that interval elapses.
    // ============================================================================================
    {
        QueuedFabric fabric;
        VClock clk;
        const NodeId n3{3}, n4{4};
        QueuedTransport n3_inner(fabric, n3), n4_inner(fabric, n4);
        MockCipher unused_cipher(0);
        SecureTransport n3_secure(n3_inner, unused_cipher, n3);
        SecureTransport n4_secure(n4_inner, unused_cipher, n4);
        n3_secure.set_clock(&vclock_read, &clk);
        n4_secure.set_clock(&vclock_read, &clk);
        constexpr std::int64_t kIdleIntervalNs = 500'000'000;  // 500 ms
        n3_secure.set_rotation_policy(/*max_frames_per_key=*/1'000'000, kIdleIntervalNs);
        IdentityMaterial identity3(identity_blob), identity4(identity_blob);
        TrustStore trust3(trust_blob), trust4(trust_blob);
        MockHandshakeFactory factory3, factory4;
        n3_secure.enable_handshake(factory3, cluster, identity3, trust3);
        n4_secure.enable_handshake(factory4, cluster, identity4, trust4);
        n3_secure.on_receive([](MessageFrame) {});
        n4_secure.on_receive([](MessageFrame) {});

        n3_secure.send(n4, make_data_frame(n3, n4));  // establishes the session at clk.now == 0
        fabric.pump_all();
        check(n3_secure.session_count() == 1, "session established at t=0", ok);

        // --- C1: well before the interval elapses, sweep_rotation must leave the idle session alone. ---
        clk.now += kIdleIntervalNs / 2;
        n3_secure.sweep_rotation(clk.now);
        check(n3_secure.renegotiations_attempted() == 0, "C1: no renegotiate before the idle interval elapses", ok);
        check(fabric.pending() == 0, "C1: nothing queued — the idle session was left untouched", ok);

        // --- C15: past the interval, with STILL zero application traffic, the sweep rekeys it anyway. ---
        clk.now += kIdleIntervalNs;  // now well past kIdleIntervalNs since the t=0 install
        n3_secure.sweep_rotation(clk.now);
        fabric.pump_all();
        check(n3_secure.renegotiations_completed() == 1, "C15: a fully idle session is still rekeyed on schedule", ok);
        check(n3_secure.session_count() == 1, "still exactly one session (in-place swap)", ok);
    }

    // ============================================================================================
    // Part C — C2: a renegotiation that FAILS drops the stale session and invokes the reset hook;
    // the next send() falls back to S2 and a fresh handshake succeeds once the problem clears.
    // ============================================================================================
    {
        QueuedFabric fabric;
        VClock clk;
        const NodeId n5{5}, n6{6};
        QueuedTransport n5_inner(fabric, n5), n6_inner(fabric, n6);
        MockCipher unused_cipher(0);
        SecureTransport n5_secure(n5_inner, unused_cipher, n5);
        SecureTransport n6_secure(n6_inner, unused_cipher, n6);
        n5_secure.set_clock(&vclock_read, &clk);
        n6_secure.set_clock(&vclock_read, &clk);
        n5_secure.set_rotation_policy(/*max_frames_per_key=*/1, /*renegotiate_interval_ns=*/1'000'000'000'000LL);
        IdentityMaterial identity5(identity_blob), identity6(identity_blob);
        TrustStore trust5(trust_blob), trust6(trust_blob);
        MockHandshakeFactory factory5, factory6;
        n5_secure.enable_handshake(factory5, cluster, identity5, trust5);
        n6_secure.enable_handshake(factory6, cluster, identity6, trust6);
        MetricsRegistry registry;
        ShardCounters& shard = registry.add_shard();
        n5_secure.set_metrics(shard);
        int delivered6 = 0;
        n6_secure.on_receive([&](MessageFrame) { ++delivered6; });
        n5_secure.on_receive([](MessageFrame) {});

        int reset_calls = 0;
        NodeId reset_peer{};
        n5_secure.set_reset_hook([&](NodeId p) {
            ++reset_calls;
            reset_peer = p;
        });

        n5_secure.send(n6, make_data_frame(n5, n6));  // initial handshake — not force-failed
        fabric.pump_all();
        check(n5_secure.session_count() == 1, "initial session established", ok);
        check(reset_calls == 0, "no reset yet — nothing has failed", ok);

        // Arm a forced failure on n5's CLIENT-role engine toward n6 — simulates a renegotiation the
        // peer's trust store now rejects (e.g. a CA rotated out from under it).
        factory5.force_fail_peer(n6);
        n5_secure.send(n6, make_data_frame(n5, n6));  // 1 frame sealed ⇒ meets max_frames_per_key==1
        fabric.pump_all();
        n5_secure.sweep_rotation(clk.now);  // triggers try_renegotiate(n6); its client engine fails immediately
        check(n5_secure.renegotiations_failed() == 1, "C2: the renegotiation attempt failed", ok);
        check(n5_secure.renegotiations_completed() == 0, "no successful renegotiation happened", ok);
        check(n5_secure.session_count() == 0, "C2: the stale session was dropped on renegotiate failure", ok);
        check(reset_calls == 1 && reset_peer == n6, "C2: the reset hook fired for exactly the failed peer", ok);
        check(registry.snapshot().security_handshakes_failed == 1, "metrics: the failed renegotiation is counted as a failed handshake", ok);

        // The very next send() correctly falls back to S2 (no session ⇒ drop this frame + attempt a
        // fresh handshake) rather than crashing or silently reusing anything from the dropped session.
        // The mock engine still fails SYNCHRONOUSLY here (force_fail_ is checked before any WantWrite
        // state would be produced), so nothing is left pending and nothing is queued on the fabric —
        // S2 dropped exactly one more frame and the attempt failed immediately, same as before.
        n5_secure.send(n6, make_data_frame(n5, n6));
        check(n5_secure.pending_handshake_count() == 0, "the failed attempt left nothing pending (synchronous failure)", ok);
        check(fabric.pending() == 0, "nothing was ever queued — the mock engine fails before producing bytes", ok);
        check(n5_secure.session_count() == 0, "still no session — the underlying problem has not cleared yet", ok);

        // Once the underlying problem clears (e.g. the operator rotates n6 back to a trusted identity),
        // 021's redial keeps trying and the FIRST post-recovery handshake succeeds (C2's "first frame
        // opens" — the fresh generation, not the same broken one, is what unblocks traffic).
        factory5.clear_force_fail(n6);
        n5_secure.send(n6, make_data_frame(n5, n6));
        fabric.pump_all();
        check(n5_secure.session_count() == 1, "a fresh handshake succeeds once the problem clears", ok);
        n5_secure.send(n6, make_data_frame(n5, n6));
        fabric.pump_all();
        check(delivered6 >= 1, "C2: the first post-recovery frame is delivered (fresh generation opens)", ok);
    }

    std::printf("security_rotation_sweep_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
