// The pinned-key engine wired into SecureTransport (secure_transport.hpp) exactly as the TLS engine is — ADR-068 claim T1:
// the seam is unchanged, the Authenticate frames carry the fixed-length Noise messages (154 / 58 bytes), and after the
// handshake the EXISTING per-lane-counter envelope runs on the libsodium ChaCha20-Poly1305 session keys in both directions
// and on both lanes (Data and Control). Controls: S2 (no session => no delivery) before the handshake; a replayed sealed frame
// is rejected; plaintext does not appear on the wire; an un-admitted client leaves no session and gets no byte back.
//
// DOCUMENTED LIMIT (checked, not hidden): this composition still sends each frame's `from`/`to` NodeIds in the clear in the
// frame header (secure_transport.hpp: "routing needs it in the clear"), so over THIS path a passive eavesdropper sees the
// derived NodeIds. The no-pre-admission-disclosure property holds for the handshake bytes themselves (pinned_privacy_check)
// and a prober that has not been admitted gets nothing; making the TCP framing itself identifier-free is the follow-up
// recorded in ADR-068.
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)".
#include <algorithm>
#include <cstdio>
#include <deque>
#include <unordered_map>

#include "pinned_test_util.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/wire_codec.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
// Deferred-delivery fabric that also LOGS every frame handed to the wire.
class Fabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> rx) { receivers_[n.value] = std::move(rx); }
    void send(NodeId to, MessageFrame f) {
        log_.push_back({to, f});
        queue_.push_back({to, std::move(f)});
    }
    bool pump_one() {
        if (queue_.empty()) return false;
        auto [to, f] = std::move(queue_.front());
        queue_.pop_front();
        if (const auto it = receivers_.find(to.value); it != receivers_.end()) it->second(std::move(f));
        return true;
    }
    void pump_all() {
        while (pump_one()) {}
    }
    [[nodiscard]] std::size_t pending() const { return queue_.size(); }
    std::vector<std::pair<NodeId, MessageFrame>> log_;

private:
    std::unordered_map<std::uint64_t, std::function<void(MessageFrame)>> receivers_;
    std::deque<std::pair<NodeId, MessageFrame>> queue_;
};
class FabricTransport final : public Transport {
public:
    FabricTransport(Fabric& f, NodeId self) : f_(&f), self_(self) {}
    void send(NodeId to, MessageFrame fr) override { f_->send(to, std::move(fr)); }
    void on_receive(std::function<void(MessageFrame)> cb) override { f_->attach(self_, std::move(cb)); }

private:
    Fabric* f_;
    NodeId self_;
};

MessageFrame frame(NodeId from, NodeId to, FrameKind kind, std::string_view text) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = kind;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    for (char c : text) f.payload.push_back(static_cast<std::byte>(c));
    return f;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    const ClusterId cluster{9};

    // Three nodes sorted by derived NodeId: n0 < n1 < n2. The LOWER id is the client (initiator) by the existing
    // glare-free rule, so n0 and n1 dial n2.
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 3; ++i) nodes.push_back(std::make_unique<Node>());
    std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
    Node& a = *nodes[0];       // admitted client
    Node& c = *nodes[1];       // client B has never admitted
    Node& b = *nodes[2];       // responder
    pair_up(a, b, cluster);
    (void)c.book->admit(cluster, b.pub);  // C knows and trusts B; B does not admit C

    auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});

    Fabric fabric;
    FabricTransport a_in(fabric, a.id), b_in(fabric, b.id), c_in(fabric, c.id);
    MockCipher unused(0);  // constructor argument only; sessions come from the handshake
    SecureTransport a_sec(a_in, unused, a.id), b_sec(b_in, unused, b.id), c_sec(c_in, unused, c.id);
    IdentityMaterial ida(identity_blob), idb(identity_blob), idc(identity_blob);
    TrustStore ta(trust_blob), tb(trust_blob), tc(trust_blob);
    a_sec.enable_handshake(*a.factory, cluster, ida, ta);
    b_sec.enable_handshake(*b.factory, cluster, idb, tb);
    c_sec.enable_handshake(*c.factory, cluster, idc, tc);

    // A silently refused initiator hears nothing back, so its parked engine is only abandoned by the handshake timeout.
    // C's transport runs on a virtual clock so the control below can step past that timeout without sleeping.
    static std::int64_t c_clock_ns = 1'000'000'000;
    c_sec.set_clock([](void*) noexcept -> std::int64_t { return c_clock_ns; }, nullptr);

    std::vector<MessageFrame> at_b, at_a;
    b_sec.on_receive([&](MessageFrame f) { at_b.push_back(std::move(f)); });
    a_sec.on_receive([&](MessageFrame f) { at_a.push_back(std::move(f)); });
    c_sec.on_receive([](MessageFrame) {});

    // An injected 1-byte Authenticate frame claiming to be from A must not park a responder engine, and A's real handshake afterwards must still work. (Sent BEFORE A has a session: an
    // Authenticate frame claiming an ESTABLISHED peer makes SecureTransport treat a failed engine as a failed renegotiation and
    // drop that session. That is existing SecureTransport behaviour, identical in TLS mode, noted in ADR-068, not changed here.)
    {
        MessageFrame junk = frame(a.id, b.id, FrameKind::Authenticate, "x");
        fabric.send(b.id, junk);
        fabric.pump_all();
        check(b_sec.pending_handshake_count() == 0, "T1: a 1-byte Authenticate frame parks nothing on the responder", ok);
    }

    const std::size_t log_start = fabric.log_.size();  // after the injected junk frame
    // S2: before the handshake nothing is delivered; the first send starts it.
    a_sec.send(b.id, frame(a.id, b.id, FrameKind::Data, "too early"));
    check(a_sec.handshake_pending_dropped() == 1 && at_b.empty(), "T1: a send before a session exists is dropped (S2)", ok);
    check(fabric.pending() == 1, "T1: exactly one Authenticate frame (Msg1) is queued", ok);
    fabric.pump_all();
    check(a_sec.session_count() == 1 && b_sec.session_count() == 1, "T1: both ends installed a session after Msg1/Msg2", ok);

    // What the handshake put on the wire: only fixed-length Authenticate payloads.
    {
        std::size_t n_auth = 0;
        bool lengths = true;
        for (std::size_t li = log_start; li < fabric.log_.size(); ++li) {
            const MessageFrame& f = fabric.log_[li].second;
            if (f.kind != FrameKind::Authenticate) continue;
            ++n_auth;
            lengths = lengths && (f.payload.size() == kPinnedMsg1Len || f.payload.size() == kPinnedMsg2Len);
        }
        check(n_auth == 2 && lengths, "T1: the handshake is exactly two Authenticate frames of 154 and 58 bytes", ok);
    }

    // Both directions, both lanes, through the existing envelope.
    a_sec.send(b.id, frame(a.id, b.id, FrameKind::Data, "data a->b"));
    a_sec.send(b.id, frame(a.id, b.id, FrameKind::Control, "control a->b"));
    b_sec.send(a.id, frame(b.id, a.id, FrameKind::Data, "data b->a"));
    b_sec.send(a.id, frame(b.id, a.id, FrameKind::Control, "control b->a"));
    fabric.pump_all();
    auto text = [](const MessageFrame& f) {
        return std::string(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
    };
    check(at_b.size() == 2 && at_a.size() == 2, "T1: two frames delivered each way (one per lane)", ok);
    check(at_b.size() == 2 && text(at_b[0]) == "data a->b" && text(at_b[1]) == "control a->b" && at_b[1].kind == FrameKind::Control,
          "T1: a->b Data and Control lanes round-trip", ok);
    check(at_a.size() == 2 && text(at_a[0]) == "data b->a" && text(at_a[1]) == "control b->a" && at_a[1].kind == FrameKind::Control,
          "T1: b->a Data and Control lanes round-trip", ok);

    // The wire carries ciphertext: the plaintext is nowhere in the logged sealed payloads (control: the receiver saw it).
    {
        bool leaked = false;
        for (auto& [to, f] : fabric.log_)
            if (f.kind != FrameKind::Authenticate) {
                const std::string needle = "data a->b";
                leaked = leaked || contains(f.payload, std::as_bytes(std::span<const char>(needle.data(), needle.size())));
            }
        check(!leaked, "T1: no plaintext on the wire after the handshake", ok);
    }

    // Replay: a captured sealed frame injected again is rejected by the per-lane counter.
    {
        const auto before = b_sec.replays_rejected();
        for (auto& [to, f] : fabric.log_)
            if (f.kind == FrameKind::Data && to == b.id) {
                fabric.send(b.id, f);
                break;
            }
        fabric.pump_all();
        check(b_sec.replays_rejected() == before + 1 && at_b.size() == 2, "T1: a replayed sealed Data frame is rejected (per-lane guard)", ok);
        // CONTROL: the genuine next frame still flows.
        a_sec.send(b.id, frame(a.id, b.id, FrameKind::Data, "after replay"));
        fabric.pump_all();
        check(at_b.size() == 3, "T1 CONTROL: a fresh frame still flows after the replay was refused", ok);
    }

    // Un-admitted C: B refuses silently — no session, and not one byte sent back toward C.
    {
        const std::size_t log_before = fabric.log_.size();
        c_sec.send(b.id, frame(c.id, b.id, FrameKind::Data, "let me in"));
        fabric.pump_all();
        bool b_spoke_to_c = false;
        for (std::size_t i = log_before; i < fabric.log_.size(); ++i)
            if (fabric.log_[i].first == c.id) b_spoke_to_c = true;
        check(b_sec.session_count() == 1 && c_sec.session_count() == 0, "T1: an un-admitted client gets no session on either side", ok);
        check(!b_spoke_to_c, "T1: the responder sent NOTHING toward the un-admitted client", ok);
        // CONTROL: admit C and retry (the transport re-initiates on the next send).
        (void)b.book->admit(cluster, c.pub);
        c_clock_ns += 3'600'000'000'000;  // an hour: far past the handshake timeout, C abandons its parked engine
        c_sec.send(b.id, frame(c.id, b.id, FrameKind::Data, "retry"));
        fabric.pump_all();
        c_sec.send(b.id, frame(c.id, b.id, FrameKind::Data, "now in"));
        fabric.pump_all();
        check(c_sec.session_count() == 1 && b_sec.session_count() == 2, "T1 CONTROL: once admitted, the next handshake succeeds", ok);
    }

    // DOCUMENTED LIMIT: in this composition the frame header still carries the derived NodeIds in the clear.
    {
        const MessageFrame& f = fabric.log_.back().second;
        const auto wire = net::encode_frame(f);
        check(contains(wire, le64(f.from.value)), "T1 DOCUMENTED LIMIT: the SecureTransport/TcpTransport frame header carries from/to NodeIds in cleartext", ok);
    }

    std::printf(ok ? "pinned_secure_transport_check: OK\n" : "pinned_secure_transport_check: FAILED\n");
    return ok ? 0 : 1;
}
