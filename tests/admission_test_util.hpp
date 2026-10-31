// Shared fixtures for the ADR-069 admission tests (020 §1a): a one-round-trip mock handshake whose Done
// step can run a test hook (to publish a generation EXACTLY between a handshake's answer and its
// completion), a queued fabric so inbound frames can sit "queued" until pumped, and a two-node rig with
// one AdmissionAuthority per node (each node holds its own snapshot, as in a real deployment).
// Not a *_test.cpp: just an include.
#pragma once

#include <cstdio>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/admission.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"
#include "quark/detail/hash.hpp"

namespace quark::test {

inline void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
}
inline std::uint64_t get_u64(std::span<const std::byte> in) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[static_cast<std::size_t>(b)])) << (b * 8);
    return v;
}
inline Fingerprint fp_of(NodeId n) noexcept {
    Fingerprint fp{};
    fp[0] = static_cast<std::byte>(n.value & 0xFF);
    fp[1] = static_cast<std::byte>((n.value >> 8) & 0xFF);
    return fp;
}

// `on_server_done` runs inside the SERVER engine's final advance(): after the handshake's
// start-of-handshake admission answer was read, before the session is installed.
struct EngineHooks {
    std::function<void()> on_server_done;
};

class MockServerEngine final : public HandshakeEngine {
public:
    MockServerEngine(NodeId self, NodeId peer, ClusterId cluster, EngineHooks* h)
        : self_(self), peer_(peer), cluster_(cluster), hooks_(h) {}
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (in.size() != 24 || get_u64(in.subspan(0, 8)) != peer_.value ||
            get_u64(in.subspan(8, 8)) != self_.value || get_u64(in.subspan(16, 8)) != cluster_.value) {
            failure_ = "malformed/mismatched ClientHello";
            return Step::Failed;
        }
        put_u64(out, self_.value);
        put_u64(out, peer_.value);
        put_u64(out, cluster_.value);
        if (hooks_ && hooks_->on_server_done) hooks_->on_server_done();
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
    EngineHooks* hooks_;
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
    EngineHooks hooks;
    std::unique_ptr<HandshakeEngine> create(bool is_client, NodeId self, NodeId expected_peer,
                                            ClusterId cluster_id, IdentityMaterial::Snapshot,
                                            TrustStore::Snapshot,
                                            std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        if (is_client) return std::make_unique<MockClientEngine>(self, expected_peer, cluster_id);
        return std::make_unique<MockServerEngine>(self, expected_peer, cluster_id, &hooks);
    }
};

class QueuedFabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> receiver) { receivers_[n] = std::move(receiver); }
    void send(NodeId to, MessageFrame frame) { queue_.push_back({to, std::move(frame)}); }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
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

[[nodiscard]] inline MessageFrame make_frame(NodeId from, NodeId to, FrameKind kind = FrameKind::Data) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    f.payload = {std::byte{9}};
    f.kind = kind;
    return f;
}

[[nodiscard]] inline std::shared_ptr<const AdmissionSnapshot> snap(
    std::uint64_t gen, std::initializer_list<NodeId> admit, std::initializer_list<NodeId> log_only = {},
    std::initializer_list<NodeId> deny = {}) {
    AdmissionSnapshot::Builder b(gen);
    for (NodeId n : admit) b.admit(n);
    for (NodeId n : log_only) b.log_exchange_only(n);
    for (NodeId n : deny) b.deny(n);
    return b.build();
}

// Two nodes n1 < n2 (n1 is the client) over a queued fabric, each with its OWN authority. Both admit each
// other at generation 1 unless `publish_initial` is false.
struct TwoNodeRig {
    static constexpr std::uint64_t kCluster = 555;
    const NodeId n1{1}, n2{2};
    AdmissionAuthority a1, a2;  // declared BEFORE the transports: an authority must outlive its listeners
    QueuedFabric fabric;
    QueuedTransport i1{fabric, n1}, i2{fabric, n2};
    MockCipher unused{0};
    SecureTransport s1{i1, unused, n1}, s2{i2, unused, n2};
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial identity1{id_blob}, identity2{id_blob};
    TrustStore trust1{trust_blob}, trust2{trust_blob};
    MockHandshakeFactory f1, f2;
    std::vector<MessageFrame> got1, got2;  // frames delivered to n1 / n2
    std::vector<NodeId> closed1, closed2;  // close-hook calls

    explicit TwoNodeRig(bool publish_initial = true) {
        s1.enable_handshake(f1, ClusterId{kCluster}, identity1, trust1);
        s2.enable_handshake(f2, ClusterId{kCluster}, identity2, trust2);
        s1.on_receive([this](MessageFrame f) { got1.push_back(std::move(f)); });
        s2.on_receive([this](MessageFrame f) { got2.push_back(std::move(f)); });
        s1.set_admission(a1, [this](NodeId p) { closed1.push_back(p); });
        s2.set_admission(a2, [this](NodeId p) { closed2.push_back(p); });
        if (publish_initial) {
            (void)a1.publish(snap(1, {n2}));
            (void)a2.publish(snap(1, {n1}));
        }
    }
    // First send triggers the handshake; pump completes it both ways.
    void establish() {
        s1.send(n2, make_frame(n1, n2));
        fabric.pump_all();
    }
};

}  // namespace quark::test
