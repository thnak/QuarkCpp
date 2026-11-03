// ADR-023 K6 over the pinned stack (#80): a lane that rejects 5 frames within 10 s makes the designated
// initiator renegotiate, at most once per 30 s. Control: the same replays with the policy disabled change
// nothing. Frames are real Noise-IK sealed frames captured off the wire, so the reject is the production path
// (replay guard / AEAD), not a synthetic counter poke.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pinned_test_util.hpp"
#include "quark/core/secure_transport.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
class Fabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> rx) { receivers_[n.value] = std::move(rx); }
    void send(NodeId to, MessageFrame f) { queue_.push_back({to, std::move(f)}); }
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
    void deliver_to(NodeId n, const MessageFrame& f) {  // inject as if it arrived on the wire
        if (const auto it = receivers_.find(n.value); it != receivers_.end()) it->second(f);
    }
    std::vector<std::pair<NodeId, MessageFrame>> log_;

private:
    std::unordered_map<std::uint64_t, std::function<void(MessageFrame)>> receivers_;
    std::deque<std::pair<NodeId, MessageFrame>> queue_;
};

class FabricTransport final : public Transport {
public:
    FabricTransport(Fabric& f, NodeId self) : f_(&f), self_(self) {}
    void send(NodeId to, MessageFrame fr) override {
        f_->log_.push_back({to, fr});
        f_->send(to, std::move(fr));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { f_->attach(self_, std::move(cb)); }

private:
    Fabric* f_;
    NodeId self_;
};

MessageFrame data(NodeId from, NodeId to, std::string_view text) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Data;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    for (char c : text) f.payload.push_back(static_cast<std::byte>(c));
    return f;
}

struct Result {
    std::uint64_t attempts = 0;
    std::uint64_t rejects = 0;
    bool session_up = false;
    bool captured = false;
    bool second_burst_attempted = false;
};

// One pair: the LOWER id is the initiator, so the higher id sends the frames whose replays the lower id sees.
Result run_case(bool policy_enabled) {
    Result r;
    const ClusterId cluster{9};
    const auto identity_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    const auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    std::vector<std::unique_ptr<Node>> nodes;
    for (int i = 0; i < 2; ++i) nodes.push_back(std::make_unique<Node>());
    std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
    Node& lo = *nodes[0];
    Node& hi = *nodes[1];
    pair_up(lo, hi, cluster);

    Fabric fabric;
    FabricTransport lo_in(fabric, lo.id), hi_in(fabric, hi.id);
    MockCipher unused(0);
    SecureTransport lo_sec(lo_in, unused, lo.id), hi_sec(hi_in, unused, hi.id);
    IdentityMaterial idlo(identity_blob), idhi(identity_blob);
    TrustStore tlo(trust_blob), thi(trust_blob);
    lo_sec.enable_handshake(*lo.factory, cluster, idlo, tlo);
    hi_sec.enable_handshake(*hi.factory, cluster, idhi, thi);
    lo_sec.on_receive([](MessageFrame) {});
    hi_sec.on_receive([](MessageFrame) {});
    if (!policy_enabled) lo_sec.set_reject_burst_policy({5, 10'000'000'000, 30'000'000'000, false});

    for (int i = 0; i < 500 && !(lo_sec.has_session(hi.id) && hi_sec.has_session(lo.id)); ++i) {
        lo_sec.send(hi.id, data(lo.id, hi.id, "open"));
        fabric.pump_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    r.session_up = lo_sec.has_session(hi.id) && hi_sec.has_session(lo.id);
    if (!r.session_up) return r;

    hi_sec.send(lo.id, data(hi.id, lo.id, "wire"));
    fabric.pump_all();
    MessageFrame wire{};
    for (auto it = fabric.log_.rbegin(); it != fabric.log_.rend(); ++it)
        if (it->first == lo.id) { wire = it->second; r.captured = true; break; }
    if (!r.captured) return r;

    const auto base = lo_sec.renegotiations_attempted();
    for (int i = 0; i < 5; ++i) fabric.deliver_to(lo.id, wire);
    r.attempts = lo_sec.renegotiations_attempted() - base;
    r.rejects = lo_sec.rejects(hi.id, 0);
    fabric.pump_all();  // let the renegotiation (if any) complete

    // A second burst inside 30 s must not add another attempt (only meaningful with the policy on).
    for (int i = 0; i < 5; ++i) fabric.deliver_to(lo.id, wire);
    r.second_burst_attempted = lo_sec.renegotiations_attempted() - base > 1;
    return r;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    };

    const Result on = run_case(true);
    check(on.session_up, "the pinned session is up");
    check(on.captured, "a sealed frame is captured off the wire");
    check(on.attempts == 1, "K6: five rejects in one lane renegotiate exactly once");
    check(!on.second_burst_attempted, "K6: at most one renegotiation per 30 s");

    const Result off = run_case(false);
    check(off.session_up && off.captured, "CONTROL: the pinned session is up");
    check(off.rejects >= 5, "CONTROL: the same replays are still counted");
    check(off.attempts == 0, "CONTROL: with the policy off nothing renegotiates");

    std::printf("pinned_reject_burst_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
