// Tests the handshake-timeout fix (ADR-040 residual risk, "a lost opening handshake frame wedges that
// peer permanently"): a manual 4-container docker experiment (real net::TcpTransport + real mbedTLS
// mTLS across separate processes on a bridge network) found that a client-role handshake whose opening
// frame never reaches the peer (e.g. the peer's endpoint isn't registered yet — a normal 021
// discovery/SWIM gossip race, not a rare fault) parks in SecureTransport::pending_ FOREVER: no timeout,
// no eviction, no retry. `ensure_handshake()`/`try_renegotiate()`/`handle_authenticate_frame()` all
// treated "an engine is already parked for this peer" as permanent in-flight state.
//
// The fix: pending_ now records a last-progress timestamp per parked engine; a parked engine idle past
// `handshake_timeout_ns_` (default 5s, overridable via set_handshake_timeout()) is abandoned and a fresh
// one is created on the next trigger (ensure_handshake, try_renegotiate, or an inbound Authenticate frame
// racing the abandonment). Uses a QueuedFabric (delivery is manually pumped) + VClock (manually advanced)
// so "the opening frame is lost" and "time passes" are both deterministic, not timing-dependent.
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

// Deferred, step-wise delivery (pump_one) — needed to leave the opening ClientHello permanently
// UNdelivered (simulating a lost frame) while still exercising a real send()/handshake flow.
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
    void drop_all() { queue_.clear(); }  // simulates the frame(s) never arriving (lost, not delayed)

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

    QueuedFabric fabric;
    VClock clk;
    const NodeId n1{1}, n2{2};  // n1 < n2 ⇒ n1 is the client (glare-free by construction)
    QueuedTransport n1_inner(fabric, n1), n2_inner(fabric, n2);
    MockCipher unused_cipher(0);
    SecureTransport n1_secure(n1_inner, unused_cipher, n1);
    n1_secure.set_clock(&vclock_read, &clk);
    n1_secure.set_handshake_timeout(1'000'000'000LL);  // 1s (arbitrary; only relative advances matter)
    IdentityMaterial identity1(identity_blob), identity2(identity_blob);
    TrustStore trust1(trust_blob), trust2(trust_blob);
    MockHandshakeFactory factory1;
    n1_secure.enable_handshake(factory1, cluster, identity1, trust1);
    // n2 is never constructed as a SecureTransport here — the opening frame is deliberately never
    // delivered (drop_all()), so n2's response never matters for this test.

    // --- First attempt: send() triggers ensure_handshake(), the ClientHello is queued but LOST. -------
    n1_secure.send(n2, make_data_frame(n1, n2));
    check(factory1.created() == 1, "first send() creates exactly one client-role engine", ok);
    check(n1_secure.pending_handshake_count() == 1, "n1 has a handshake parked in flight", ok);
    check(fabric.pending() == 1, "the opening ClientHello is queued (not yet lost)", ok);
    check(n1_secure.handshakes_timed_out() == 0, "no timeout has fired yet", ok);
    fabric.drop_all();  // the frame is lost — simulates a peer whose endpoint wasn't registered yet

    // --- Before the timeout: a retry must NOT abandon the still-fresh parked engine. -------------------
    n1_secure.send(n2, make_data_frame(n1, n2));
    check(factory1.created() == 1, "a retry before the timeout reuses the parked engine (no new one)", ok);
    check(n1_secure.handshakes_timed_out() == 0, "still no timeout before the deadline elapses", ok);

    // --- Advance the virtual clock past the deadline; the NEXT trigger abandons the stale engine. ------
    clk.now += 2'000'000'000LL;  // 2s > the 1s timeout
    n1_secure.send(n2, make_data_frame(n1, n2));
    check(n1_secure.handshakes_timed_out() == 1, "the stale parked engine is abandoned exactly once", ok);
    check(factory1.created() == 2, "abandoning triggers a FRESH client-role engine (this is the fix — "
                                   "pre-fix, this peer would be wedged forever)", ok);
    check(n1_secure.pending_handshake_count() == 1, "exactly one (the new) handshake is in flight", ok);
    check(fabric.pending() == 1, "the new ClientHello is queued", ok);

    // --- The abandoned-but-still-in-flight OLD response racing an inbound Authenticate frame: an inbound
    // frame arriving for a peer with NO parked engine (fresh cold start) must still work normally — this
    // is handle_authenticate_frame's ordinary server-role path, unaffected by the fix (regression guard).
    {
        QueuedFabric fabric2;
        VClock clk2;
        QueuedTransport a_inner(fabric2, n1), b_inner(fabric2, n2);
        SecureTransport a_secure(a_inner, unused_cipher, n1), b_secure(b_inner, unused_cipher, n2);
        a_secure.set_clock(&vclock_read, &clk2);
        b_secure.set_clock(&vclock_read, &clk2);
        MockHandshakeFactory fa, fb;
        a_secure.enable_handshake(fa, cluster, identity1, trust1);
        b_secure.enable_handshake(fb, cluster, identity2, trust2);
        a_secure.on_receive([](MessageFrame) {});  // wires the inner receiver on both sides
        b_secure.on_receive([](MessageFrame) {});
        a_secure.send(n2, make_data_frame(n1, n2));
        fabric2.pump_one();  // ClientHello -> b: creates server engine, replies ServerHello, Done
        fabric2.pump_one();  // ServerHello -> a: Done
        check(a_secure.session_count() == 1 && b_secure.session_count() == 1,
              "ordinary handshake completion is unaffected by the timeout fix", ok);
    }

    std::printf("security_secure_transport_handshake_timeout_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
