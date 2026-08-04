// Tests the peer-disconnect fix (ADR-040 residual risk, "an mTLS session outlives the TCP connection it
// was negotiated on"): a manual 4-container docker experiment found that SecureTransport never dropped a
// session when the underlying connection died. After a peer restarts: the survivor keeps sealing frames
// under dead keys (never delivered, since the restarted peer has no session — S2 drops them), and since
// the restarted peer is server-role (cannot self-initiate) while the survivor still "has" a session (so
// never re-handshakes), the pair is a permanent bidirectional blackhole.
//
// The fix: `TcpTransport::set_peer_down_hook()` fires when an established, identified connection dies
// (mirrors the existing `set_reset_hook`/`reset_peer_connection` shape — a narrow, sanctioned seam, not a
// generic transport-death callback). Wiring it to `SecureTransport::on_peer_disconnected()` drops that
// peer's session (and any parked handshake), so the NEXT send() re-triggers a clean handshake instead of
// sealing into a void. This test proves `on_peer_disconnected()` itself against a mock transport (no real
// sockets needed — tcp_transport_reconnect_test.cpp separately proves the real connection-death detection
// this hook rides on, the same relationship reset_hook_/reset_peer_connection already has).
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
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{556};
    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});

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

    int delivered2 = 0;
    n2_secure.on_receive([&](MessageFrame) { ++delivered2; });
    n1_secure.on_receive([](MessageFrame) {});  // wires n1's inner receiver too (needed for ServerHello)

    // --- Establish a real session both ways (mirrors security_handshake_test.cpp's first scenario). ----
    n1_secure.send(n2, make_data_frame(n1, n2));  // triggers the handshake; this first frame is dropped (S2)
    fabric.pump_all();
    check(n1_secure.session_count() == 1 && n2_secure.session_count() == 1,
          "setup: a real session exists on both sides before simulating a disconnect", ok);

    n1_secure.send(n2, make_data_frame(n1, n2));
    fabric.pump_all();
    check(delivered2 == 1, "setup: post-handshake data flows normally", ok);

    // --- Simulate the underlying TCP connection dying (peer restart / RST / network drop). The FIX: ----
    // on_peer_disconnected() is what TcpTransport::set_peer_down_hook() now calls into (proven wired for
    // the real transport by inspection; tcp_transport_reconnect_test.cpp proves the death-detection this
    // hook rides on, matching the existing reset_hook_/reset_peer_connection relationship).
    n1_secure.on_peer_disconnected(n2);
    check(n1_secure.session_count() == 0, "on_peer_disconnected drops the (now-stale) session", ok);
    check(n1_secure.sessions_dropped_on_disconnect() == 1, "the drop is counted exactly once", ok);
    check(n1_secure.pending_handshake_count() == 0,
          "no dangling parked handshake either (there wasn't one here, but the call must not crash/leak)", ok);

    // --- Pre-fix, this send would have kept sealing under the dropped side's old session forever (the
    // OTHER side, n2, still thinks it has one) with nothing ever re-establishing. Post-fix, n1 has NO
    // session, so send() correctly falls back to S2 and re-triggers a fresh handshake — proving the peer
    // is NOT permanently blackholed (n2 still holding its stale session doesn't matter: it's SERVER-role
    // for this pair, so it passively accepts n1's fresh ClientHello exactly like a cold start). -----------
    n1_secure.send(n2, make_data_frame(n1, n2));
    check(factory1.created() == 2, "the next send() creates a FRESH client-role engine (re-handshake), "
                                   "not a reuse of anything stale", ok);
    check(n1_secure.pending_handshake_count() == 1, "a fresh handshake is now in flight", ok);
    fabric.pump_all();
    check(n1_secure.session_count() == 1, "the fresh handshake completes and re-establishes a session", ok);

    // One more data frame now flows on the NEW session, proving the pair is not blackholed.
    n1_secure.send(n2, make_data_frame(n1, n2));
    fabric.pump_all();
    check(delivered2 == 2, "data delivery resumes on the re-established session "
                          "(pre-fix: this pair would be a permanent bidirectional blackhole)", ok);

    std::printf("security_secure_transport_peer_disconnect_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
