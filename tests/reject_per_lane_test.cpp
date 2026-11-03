// #80 (ADR-023 rule 4), slice 1: reject counters are per SESSION and per LANE. One sent frame is replayed
// into the receiver twice: the first opens, the second is a replay reject on the Data lane only; the same on
// the Control lane leaves both counters at 1. renegotiate() is public and a no-op without a handshake.
#include <cstdio>
#include <functional>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/peer_session.hpp"
#include "quark/core/secure_transport.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
class Rec final : public Transport {
public:
    void send(NodeId, MessageFrame f) override { sent_.push_back(std::move(f)); }
    void on_receive(std::function<void(MessageFrame)> cb) override { cb_ = std::move(cb); }
    [[nodiscard]] MessageFrame sent(std::size_t i) const { return sent_[i]; }
    [[nodiscard]] std::function<void(MessageFrame)> sink() const { return cb_; }

private:
    std::vector<MessageFrame> sent_;
    std::function<void(MessageFrame)> cb_;
};
MessageFrame frame(FrameKind kind, std::byte tag) {
    MessageFrame f{};
    f.from = NodeId{1};
    f.to = NodeId{2};
    f.kind = kind;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    f.payload = {tag};
    return f;
}
}  // namespace

int main() {
    MockCipher cipher(9);  // handshake disabled: every session shares this cipher
    Rec ra, rb;
    SecureTransport a(ra, cipher, NodeId{1}), b(rb, cipher, NodeId{2});
    std::vector<MessageFrame> got;
    b.on_receive([&](MessageFrame f) { got.push_back(std::move(f)); });
    const auto inject = rb.sink();

    // Data lane: one delivery, then one replay.
    a.send(NodeId{2}, frame(FrameKind::Data, std::byte{7}));
    const auto wire = ra.sent(0);
    inject(wire);
    inject(wire);
    check(got.size() == 1, "the first Data frame is delivered");
    check(b.rejects(NodeId{1}, 0) == 1, "the replay is counted on the Data lane");
    check(b.rejects(NodeId{1}, 1) == 0, "the Control lane is untouched");

    // Control lane: same shape, independent counter.
    a.send(NodeId{2}, frame(FrameKind::Control, std::byte{8}));
    const auto cwire = ra.sent(1);
    inject(cwire);
    inject(cwire);
    check(got.size() == 2, "the first Control frame is delivered");
    check(b.rejects(NodeId{1}, 1) == 1, "the Control replay is counted on the Control lane");
    check(b.rejects(NodeId{1}, 0) == 1, "the Data lane is untouched by it");

    b.renegotiate(NodeId{1});  // #80: public, and a no-op without a handshake factory
    check(b.rejects(NodeId{1}, 0) == 1 && b.rejects(NodeId{1}, 1) == 1,
          "renegotiate() without a factory changes nothing");

    std::printf("reject_per_lane_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
