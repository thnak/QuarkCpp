// Security hardening regressions for the wire boundary:
//   1. AEAD associated data covers the fields the receiver ACTS ON (principal/mode/kind/deadline/
//      trace) — an on-path header edit must fail the tag, not silently change authorization.
//   2. SWIM control messages may only speak for the authenticated transport sender, a peer cannot
//      tombstone a third party, and frames addressed elsewhere are not applied.
//   3. Untrusted tagless payloads are bounds-checked (a truncated declared length is dropped).
//   4. Pre-auth handshake state is bounded (no unbounded `pending_` growth from spoofed senders).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/net/wire_codec.hpp"
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
    cfg.control_rate_per_sec = 1;       // deliberate low rate for the flood regression
    cfg.control_burst = 8;
    cfg.unverified_ttl_ns = 1'000'000;  // 1 ms, so the ghost reaper is observable
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
    const auto convergence_view = swim.view();  // by value: keep it alive for the span below
    for (NodeId n : convergence_view.nodes())
        if (n.value == 4242) in_view = true;
    bool in_placement = false;
    const auto placement_before = swim.placement_view();
    for (NodeId n : placement_before.nodes())
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
    const auto placement_after = swim.placement_view();
    for (NodeId n : placement_after.nodes())
        if (n.value == 4242) in_placement_after = true;
    check(in_placement_after, "first-hand Ack verifies the member for placement");

    // (g) P2: a gossip-only member that never answers is reaped after the TTL. Send it before the
    // flood so the deliberately tiny burst does not shed it.
    ControlMsg ghost;
    ghost.kind = ControlKind::Gossip;
    ghost.cluster = cfg.cluster_id;
    ghost.from = NodeId{2};
    ghost.updates.push_back(MemberUpdate{5555, 1,
                                         static_cast<std::uint8_t>(MemberStatus::Alive)});
    send_control(NodeId{2}, NodeId{1}, ghost);
    check(swim.is_known(NodeId{5555}), "unverified ghost is known before the TTL");

    // (h) P2: a control-frame flood is shed by the fixed per-sender budget.
    for (int i = 0; i < 100; ++i) {
        ControlMsg g;
        g.kind = ControlKind::Gossip;
        g.cluster = cfg.cluster_id;
        g.from = NodeId{2};
        g.updates.push_back(MemberUpdate{
            static_cast<std::uint64_t>(10000 + i), 1,
            static_cast<std::uint8_t>(MemberStatus::Alive)});
        send_control(NodeId{2}, NodeId{1}, g);
    }
    check(swim.control_sheds() > 0, "control-frame flood is rate-limited");

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    swim.tick();
    check(!swim.is_known(NodeId{5555}), "unverified ghost is reaped after the TTL");
}

// ---- 2b. Rate limiter must LIMIT, not starve ---------------------------------------------------
// A sender faster than 1/rate (here 10k frames/s vs a 4096/s budget) must still get ~rate frames/s
// admitted. The old refill moved the clock to `now` even when integer division yielded 0 tokens, so
// after the burst such a sender was admitted ~never (starving SWIM acks → false suspicion).
std::int64_t g_virtual_ns = 1'000'000'000;
std::int64_t virtual_clock(void*) noexcept { return g_virtual_ns; }

void rate_limiter_sustained() {
    LoopbackFabric fab;
    SwimMembership::Config cfg;
    cfg.cluster_id = ClusterId{0xC0FFEE};
    cfg.control_rate_per_sec = 4096;
    cfg.control_burst = 1;
    LoopbackTransport node_tx{fab, NodeId{1}};
    SwimMembership swim{NodeId{1}, node_tx, cfg};
    swim.set_clock(&virtual_clock, nullptr);
    LoopbackTransport peer{fab, NodeId{2}};

    constexpr int kFrames = 10'000;  // one virtual second at 100 us spacing
    for (int i = 0; i < kFrames; ++i) {
        g_virtual_ns += 100'000;
        ControlMsg g;
        g.kind = ControlKind::Gossip;
        g.cluster = cfg.cluster_id;
        g.from = NodeId{2};
        MessageFrame f;
        f.from = NodeId{2};
        f.to = NodeId{1};
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(g);
        peer.send(NodeId{1}, std::move(f));
    }
    const auto admitted = static_cast<std::int64_t>(kFrames) -
                          static_cast<std::int64_t>(swim.control_sheds());
    std::printf("  rate limiter: admitted %lld of %d (budget 4096/s)\n",
                static_cast<long long>(admitted), kFrames);
    check(admitted >= 3800 && admitted <= 4400,
          "a sender above the budget is admitted at ~rate, not starved to zero");
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

void per_ip_pending_cap() {
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
    secure.set_max_pending_handshakes(8);
    secure.set_max_pending_per_ip(3);
    secure.set_reserved_handshake_slots(2);
    secure.set_known_peer([](NodeId n) { return n.value == 777 || n.value == 778; });

    LoopbackTransport attacker{fab, NodeId{2}};
    auto send = [&](std::uint64_t id, std::uint64_t ip) {
        MessageFrame f;
        f.from = NodeId{id};
        f.to = NodeId{1};
        f.kind = FrameKind::Authenticate;
        f.payload = {std::byte{0x16}, std::byte{0x03}, std::byte{0x01}};
        f.source_address = PeerAddress{ip, 4444, true};
        attacker.send(NodeId{1}, std::move(f));
    };

    // Same-IP flood: only 3 pending engines.
    for (std::uint64_t i = 0; i < 3; ++i) send(2000 + i, 0x0A000001);
    check(secure.pending_handshake_count() == 3, "same-IP pending entries are admitted up to the cap");
    send(2003, 0x0A000001);
    check(secure.pending_handshake_count() == 3, "same-IP flood is capped");

    // A different IP is unaffected.
    send(2004, 0x0A000002);
    check(secure.pending_handshake_count() == 4, "a different source IP is not charged to the flood");

    // Fill the general pool (8) from distinct IPs.
    for (std::uint64_t i = 0; i < 4; ++i) send(2100 + i, 0x0A000010 + i);
    check(secure.pending_handshake_count() == 8, "general pending pool is filled");
    send(2200, 0x0A000020);
    check(secure.pending_handshake_count() == 8, "unknown peer is denied when the general pool is full");

    // Reserved slots let known peers through; only the configured reservation.
    send(777, 0x0A000030);
    check(secure.pending_handshake_count() == 9, "known peer uses a reserved slot");
    send(778, 0x0A000031);
    check(secure.pending_handshake_count() == 10, "second known peer uses the last reserved slot");
    send(779, 0x0A000032);
    check(secure.pending_handshake_count() == 10, "unreserved peer cannot exceed the reservation");
}

// A7: `frame.from` is unauthenticated before the handshake completes, so (a) a frame for a parked
// engine from a DIFFERENT source IP than the one that opened it must be ignored without touching or
// evicting the legitimate engine, and (b) a claimed-known NodeId must not bypass the per-IP cap.
void handshake_spoof_resistance() {
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
    secure.set_max_pending_per_ip(2);
    secure.set_known_peer([](NodeId n) { return n.value >= 777 && n.value <= 779; });

    LoopbackTransport attacker{fab, NodeId{2}};
    auto send = [&](std::uint64_t id, std::uint64_t ip) {
        MessageFrame f;
        f.from = NodeId{id};
        f.to = NodeId{1};
        f.kind = FrameKind::Authenticate;
        f.payload = {std::byte{0x16}, std::byte{0x03}, std::byte{0x01}};
        f.source_address = PeerAddress{ip, 4444, true};
        attacker.send(NodeId{1}, std::move(f));
    };
    constexpr std::uint64_t kLegitIp = 0x0A000001;
    constexpr std::uint64_t kAttackIp = 0x0A0000FF;

    // (a) hijack attempt on a parked engine.
    send(3000, kLegitIp);
    check(secure.pending_handshake_count() == 1, "legit handshake is parked");
    const std::uint64_t dropped0 = secure.handshake_pending_dropped();
    send(3000, kAttackIp);  // same claimed id, different source
    check(secure.pending_handshake_count() == 1,
          "a spoofed frame from another IP does not evict the legit parked engine");
    check(secure.handshake_pending_dropped() == dropped0 + 1,
          "the spoofed frame is counted as dropped");
    send(3000, kLegitIp);  // the real peer keeps going
    check(secure.pending_handshake_count() == 1 && secure.handshake_pending_dropped() == dropped0 + 1,
          "the legit peer's next frame is still accepted by its parked engine");

    // (b) a known-peer claim must not bypass the per-IP cap.
    const std::uint64_t base = secure.pending_handshake_count();
    send(777, kAttackIp);
    send(778, kAttackIp);
    send(779, kAttackIp);  // third from one IP: over the cap of 2, `known` or not
    check(secure.pending_handshake_count() == base + 2,
          "a claimed-known NodeId does not bypass the per-IP pending cap");
}

void custom_frame_cap() {
    net::FrameStream fs{64};
    std::array<std::byte, 4 + 65> bytes{};
    const std::uint32_t len = 65;
    for (int i = 0; i < 4; ++i)
        bytes[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((len >> (8 * i)) & 0xFFu);
    bool called = false;
    const bool accepted =
        fs.feed(bytes.data(), bytes.size(), [&](MessageFrame&&) { called = true; });
    check(!accepted, "custom max_frame_body rejects an oversized prefix");
    check(!called, "oversized frame is never emitted");
    check(fs.max_body() == 64, "max_body reports the configured cap");
}

int main() {
    aad_coverage();
    swim_source_binding();
    rate_limiter_sustained();
    checked_tagless();
    pending_handshake_cap();
    per_ip_pending_cap();
    handshake_spoof_resistance();
    custom_frame_cap();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
