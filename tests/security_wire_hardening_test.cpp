// Security hardening regressions for the wire boundary:
//   1. AEAD associated data covers the fields the receiver ACTS ON (principal/mode/kind/deadline/
//      trace) — an on-path header edit must fail the tag, not silently change authorization.
//   2. SWIM control messages may only speak for the authenticated transport sender, a peer cannot
//      tombstone a third party, and frames addressed elsewhere are not applied.
//   3. Untrusted tagless payloads are bounds-checked (a truncated declared length is dropped).
//   4. Pre-auth handshake state is bounded (no unbounded `pending_` growth from spoofed senders).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"
#include "quark/core/wire.hpp"

using namespace quark;

namespace {

bool ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// ---- 1. AAD coverage ---------------------------------------------------------------------------
class TamperTransport final : public Transport {
public:
    explicit TamperTransport(Transport& inner) : inner_(&inner) {}
    bool tamper = false;
    std::uint64_t rights = 2;
    std::optional<WireMode> mode_override;
    void send(NodeId to, MessageFrame frame) override {
        if (tamper) frame.principal = Principal{7, rights};  // plaintext header edit
        if (mode_override) frame.mode = *mode_override;       // plaintext header edit
        inner_->send(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override {
        inner_->on_receive(std::move(cb));
    }

private:
    Transport* inner_;
};

void aad_coverage() {
    LoopbackFabric fab;
    MockCipher cipher{0x5EC0DE};

    LoopbackTransport rx_tx{fab, NodeId{2}};
    SecureTransport rx{rx_tx, cipher, NodeId{2}};
    std::atomic<int> delivered{0};
    rx.on_receive([&](MessageFrame) { delivered.fetch_add(1); });

    LoopbackTransport tx_tx{fab, NodeId{1}};
    TamperTransport tamper{tx_tx};
    SecureTransport tx{tamper, cipher, NodeId{1}};

    auto frame = [&] {
        MessageFrame f;
        f.from = NodeId{1};
        f.to = NodeId{2};
        f.target = ActorId{TypeKey{1}, 1};
        f.msg_type = TypeKey{2};
        f.mode = WireMode::Tagged;
        f.deadline_ns = 123;
        f.trace_id = 456;
        f.principal = Principal{5, 1};
        f.payload = {std::byte{0xAA}};
        return f;
    };

    tamper.tamper = false;
    tx.send(NodeId{2}, frame());
    check(delivered.load() == 1, "untampered frame is delivered");

    tamper.tamper = true;
    tx.send(NodeId{2}, frame());
    check(delivered.load() == 1, "principal edit does not reach the actor");
    check(rx.tamper_rejected() == 1, "principal edit fails the AEAD tag");

    // The mode is a header field the receiver acts on (it selects the decoder), so flipping it in
    // flight must also fail the tag.
    tamper.tamper = false;
    tamper.mode_override = WireMode::Tagless;
    tx.send(NodeId{2}, frame());
    check(delivered.load() == 1, "mode edit does not reach the actor");
    check(rx.tamper_rejected() == 2, "mode edit fails the AEAD tag");
}

// ---- 2. SWIM control-plane source binding ------------------------------------------------------
void swim_source_binding() {
    LoopbackFabric fab;
    SwimMembership::Config cfg;
    cfg.cluster_id = ClusterId{0xC0FFEE};
    LoopbackTransport node_tx{fab, NodeId{1}};
    SwimMembership swim{NodeId{1}, node_tx, cfg};
    LoopbackTransport attacker{fab, NodeId{2}};

    auto send_control = [&](NodeId frame_from, NodeId to, ControlMsg m) {
        MessageFrame f;
        f.from = frame_from;
        f.to = to;
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(m);
        attacker.send(to, std::move(f));
    };

    // (a) m.from must equal the frame/session sender: cannot mint members as another node.
    ControlMsg forged;
    forged.kind = ControlKind::Gossip;
    forged.cluster = cfg.cluster_id;
    forged.from = NodeId{3};
    forged.updates.push_back(MemberUpdate{31337, 1, static_cast<std::uint8_t>(MemberStatus::Alive)});
    send_control(NodeId{2}, NodeId{1}, forged);
    check(!swim.is_known(NodeId{31337}), "m.from != authenticated sender is rejected");

    // (b) a peer may not tombstone a THIRD party.
    ControlMsg evict;
    evict.kind = ControlKind::Gossip;
    evict.cluster = cfg.cluster_id;
    evict.from = NodeId{2};  // its own identity, but the update is about node 1
    evict.updates.push_back(
        MemberUpdate{1, 0xFFFFFFFFFFFFFFFFULL, static_cast<std::uint8_t>(MemberStatus::Left)});
    send_control(NodeId{2}, NodeId{1}, evict);
    check(swim.status_of(NodeId{1}) == MemberStatus::Alive,
          "third-party gossip Left is ignored");

    // (c) a frame addressed to another node is not applied.
    ControlMsg valid;
    valid.kind = ControlKind::Gossip;
    valid.cluster = cfg.cluster_id;
    valid.from = NodeId{2};
    valid.updates.push_back(MemberUpdate{4242, 1, static_cast<std::uint8_t>(MemberStatus::Alive)});
    send_control(NodeId{2}, NodeId{99}, valid);
    check(!swim.is_known(NodeId{4242}), "frame addressed to another node is dropped");

    // (d) the same frame with a correct sender and destination is accepted.
    send_control(NodeId{2}, NodeId{1}, valid);
    check(swim.is_known(NodeId{4242}), "valid gossip from the authenticated sender is accepted");

    // (e) gossip-only membership is in the convergence view but NOT in the placement view.
    bool in_view = false;
    for (NodeId n : swim.view().nodes())
        if (n.value == 4242) in_view = true;
    bool in_placement = false;
    for (NodeId n : swim.placement_view().nodes())
        if (n.value == 4242) in_placement = true;
    check(in_view, "unverified gossip member is in the convergence view");
    check(!in_placement, "unverified gossip member is excluded from placement");

    // (f) a DIRECT Ack from that node verifies it and admits it to placement.
    ControlMsg ack;
    ack.kind = ControlKind::Ack;
    ack.cluster = cfg.cluster_id;
    ack.from = NodeId{4242};
    ack.from_incarnation = 1;
    ack.subject = NodeId{1};
    ack.subject_incarnation = 0;
    ack.seq = 0;
    send_control(NodeId{4242}, NodeId{1}, ack);
    bool in_placement_after = false;
    for (NodeId n : swim.placement_view().nodes())
        if (n.value == 4242) in_placement_after = true;
    check(in_placement_after, "first-hand Ack verifies the member for placement");
}

// ---- 3. Checked tagless decode -----------------------------------------------------------------
struct Blob {
    std::string text;
    std::int32_t num = 0;
};
QUARK_SERIALIZE(Blob, (1, text), (2, num))

void checked_tagless() {
    Blob in;
    in.text = "hello";
    in.num = 42;
    std::vector<std::byte> buf(tagless_size(in));
    (void)encode_tagless(in, buf.data());

    Blob out;
    check(decode_tagless_checked(buf.data(), buf.size(), out), "well-formed tagless decodes");
    check(out.text == "hello" && out.num == 42, "tagless round-trip values");

    // u32 string length = 100 but only three bytes present -> must fail, not OOB-read.
    const std::vector<std::byte> short_buf{std::byte{100}, std::byte{0}, std::byte{0}, std::byte{0},
                                           std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    Blob bad;
    check(!decode_tagless_checked(short_buf.data(), short_buf.size(), bad),
          "truncated tagless string is rejected");
}

// ---- 4. Pending-handshake cap ------------------------------------------------------------------
class WantReadEngine final : public HandshakeEngine {
public:
    Step advance(std::span<const std::byte>, std::vector<std::byte>& out) override {
        out.clear();
        return Step::WantRead;
    }
    Result take_result() override { return Result{}; }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return "want-read"; }
};
class WantReadFactory final : public HandshakeEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<HandshakeEngine> create(
        bool, NodeId, NodeId, ClusterId, IdentityMaterial::Snapshot, TrustStore::Snapshot,
        std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        return std::make_unique<WantReadEngine>();
    }
};

void pending_handshake_cap() {
    LoopbackFabric fab;
    LoopbackTransport node_tx{fab, NodeId{1}};
    MockCipher cipher{1};
    SecureTransport secure{node_tx, cipher, NodeId{1}};

    auto identity = std::make_shared<const TlsIdentity>();
    auto roots = std::make_shared<const TrustedRoots>();
    IdentityMaterial identity_material{identity};
    TrustStore trust{roots};
    WantReadFactory factory;
    secure.enable_handshake(factory, ClusterId{0xC0FFEE}, identity_material, trust);
    secure.on_receive([](MessageFrame) {});

    LoopbackTransport attacker{fab, NodeId{2}};
    for (std::uint64_t i = 0; i < 300; ++i) {
        MessageFrame f;
        f.from = NodeId{1000 + i};
        f.to = NodeId{1};
        f.kind = FrameKind::Authenticate;
        f.payload = {std::byte{0x16}, std::byte{0x03}, std::byte{0x01}};
        attacker.send(NodeId{1}, std::move(f));
    }
    check(secure.pending_handshake_count() == SecureTransport::kMaxPendingHandshakes,
          "pending handshakes are capped");
}

}  // namespace

int main() {
    aad_coverage();
    swim_source_binding();
    checked_tagless();
    pending_handshake_cap();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
