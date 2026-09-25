// Tests issue #16 — the COMPOSITION of ADR-040's strict-monotonic replay guard (secure_transport.hpp)
// with ADR-046 S4's control-ahead-of-data priority (tcp_transport.hpp). Each is covered alone by
// security_secure_transport_ordering_test.cpp / tcp_transport_control_priority_test.cpp; together, a
// single per-session seq made every data frame that a later-sealed control frame (a SWIM ping)
// overtook on the wire look like a "replay", and SecureTransport silently dropped it. The fix gives
// each lane (data, control) its own seq space, lane bound into AAD + nonce.
//
// PART 1 (deterministic): a LaneQueueTransport buffers sealed frames in two FIFOs and flushes control
// first — exactly TcpTransport's `load_next_frame` policy, without sockets. Seal D1..D5, then C1..C3,
// flush ⇒ wire order C1 C2 C3 D1..D5. Pre-fix: 5 replays_rejected, 3/8 delivered.
// ADVERSARIAL CONTROLS (must FIRE): a replayed data frame is still rejected per lane; a data frame
// whose FrameKind is flipped to Control (moved across lanes) fails the AEAD tag.
//
// PART 2 (end to end): SecureTransport over two real TcpTransports on 127.0.0.1, receiver throttled
// (as in tcp_transport_control_priority_test) so a genuine data backlog forms, control frames
// interleaved behind it — every frame must be opened, 0 replay rejections.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Holds every outbound frame in a control FIFO or a data FIFO and, on flush(), sends ALL control
// frames before ANY data frame — TcpTransport's ADR-046 S4 lane policy (FIFO within a lane, control
// preferred across lanes), minus the sockets.
class LaneQueueTransport final : public Transport {
public:
    LaneQueueTransport(LoopbackFabric& fabric, NodeId self) : inner_(fabric, self) {}
    void send(NodeId to, MessageFrame frame) override {
        (frame.kind == FrameKind::Control ? control_ : data_).emplace_back(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_.on_receive(std::move(cb)); }

    void flush() {
        for (auto* q : {&control_, &data_}) {
            while (!q->empty()) {
                auto [to, f] = std::move(q->front());
                q->pop_front();
                last_data_ = f.kind == FrameKind::Data ? f : last_data_;
                inner_.send(to, std::move(f));
            }
        }
    }
    [[nodiscard]] const MessageFrame& last_data() const noexcept { return last_data_; }
    void raw_send(NodeId to, MessageFrame f) { inner_.send(to, std::move(f)); }

private:
    LoopbackTransport inner_;
    std::deque<std::pair<NodeId, MessageFrame>> control_;
    std::deque<std::pair<NodeId, MessageFrame>> data_;
    MessageFrame last_data_{};
};

MessageFrame make_frame(NodeId from, NodeId to, std::uint64_t id, FrameKind kind, std::size_t bytes) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0x7171u}, id};
    f.msg_type = TypeKey{0x42u};
    f.trace_id = id;
    f.kind = kind;
    f.payload.assign(bytes, static_cast<std::byte>(id & 0xFF));
    return f;
}

void part1_deterministic(bool& ok) {
    LoopbackFabric fabric;
    MockCipher cipher(0x1A5E0016ULL);
    const NodeId n1{1}, n2{2};

    LoopbackTransport n2_inner(fabric, n2);
    SecureTransport n2_secure(n2_inner, cipher, n2);
    std::uint64_t data_rx = 0, control_rx = 0;
    n2_secure.on_receive([&](MessageFrame f) { ++(f.kind == FrameKind::Control ? control_rx : data_rx); });

    LaneQueueTransport n1_inner(fabric, n1);
    SecureTransport n1_secure(n1_inner, cipher, n1);

    constexpr std::uint64_t kData = 5, kControl = 3;
    for (std::uint64_t i = 0; i < kData; ++i) n1_secure.send(n2, make_frame(n1, n2, i, FrameKind::Data, 32));
    for (std::uint64_t i = 0; i < kControl; ++i)
        n1_secure.send(n2, make_frame(n1, n2, 100 + i, FrameKind::Control, 8));
    n1_inner.flush();  // wire order: C1 C2 C3 D1..D5 — control overtakes every earlier-sealed data frame

    check(data_rx == kData, "part1: every overtaken data frame is delivered", ok);
    check(control_rx == kControl, "part1: every control frame is delivered", ok);
    check(n2_secure.replays_rejected() == 0, "part1: 0 replay false positives across lanes", ok);
    check(n2_secure.tamper_rejected() == 0, "part1: 0 tamper rejections", ok);

    // CONTROL 1 (must FIRE): a genuine replay of an already-accepted data frame is still rejected.
    n1_inner.raw_send(n2, n1_inner.last_data());
    check(n2_secure.replays_rejected() == 1, "part1 control: replayed data frame is rejected", ok);
    check(data_rx == kData, "part1 control: the replay is not delivered", ok);

    // CONTROL 2 (must FIRE): moving a sealed data frame onto the control lane (flipping FrameKind) —
    // whose seq space is lower there, so the replay guard alone would pass it — fails the AEAD tag,
    // because the lane is bound into the AAD and the nonce.
    MessageFrame moved = n1_inner.last_data();
    moved.kind = FrameKind::Control;
    n1_inner.raw_send(n2, std::move(moved));
    check(n2_secure.tamper_rejected() == 1, "part1 control: cross-lane moved frame fails the tag", ok);
    check(control_rx == kControl, "part1 control: the moved frame is not delivered", ok);

    std::printf("  part1: data=%llu/%llu control=%llu/%llu replays=%llu tamper=%llu\n",
                static_cast<unsigned long long>(data_rx), static_cast<unsigned long long>(kData),
                static_cast<unsigned long long>(control_rx), static_cast<unsigned long long>(kControl),
                static_cast<unsigned long long>(n2_secure.replays_rejected()),
                static_cast<unsigned long long>(n2_secure.tamper_rejected()));
}

void part2_real_tcp(bool& ok) {
    const NodeId A{1}, B{2};
    constexpr std::uint64_t kData = 200;
    constexpr std::size_t kFrameBytes = 16 * 1024;  // big enough to build a genuine socket backlog
    constexpr std::uint64_t kControlEvery = 10;     // one control frame per 10 data frames sent
    constexpr std::uint64_t kControl = kData / kControlEvery;

    MockCipher cipher(0x7C9D0016ULL);
    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    SecureTransport sa(ta, cipher, A);
    SecureTransport sb(tb, cipher, B);

    std::atomic<std::uint64_t> data_rx{0}, control_rx{0};
    sb.on_receive([&](MessageFrame f) {
        if (f.kind == FrameKind::Control) {
            control_rx.fetch_add(1, std::memory_order_release);
            return;
        }
        data_rx.fetch_add(1, std::memory_order_release);
        // Throttle consumption on B's io thread so A's data genuinely queues and control overtakes it
        // (same rationale as tcp_transport_control_priority_test).
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
    });
    sa.on_receive([](MessageFrame) {});

    const bool started_a = ta.start();
    const bool started_b = tb.start();
    check(started_a, "part2: node A listener starts", ok);
    check(started_b, "part2: node B listener starts", ok);
    if (!started_a || !started_b) return;
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));

    for (std::uint64_t i = 0; i < kData; ++i) {
        sa.send(B, make_frame(A, B, i, FrameKind::Data, kFrameBytes));
        if ((i + 1) % kControlEvery == 0) sa.send(B, make_frame(A, B, 10'000 + i, FrameKind::Control, 64));
    }

    check(wait_until(
              [&] {
                  return data_rx.load(std::memory_order_acquire) + control_rx.load(std::memory_order_acquire) +
                             sb.replays_rejected() + sb.tamper_rejected() >=
                         kData + kControl;
              },
              30000),
          "part2: every frame reaches B's SecureTransport", ok);

    check(data_rx.load() == kData, "part2: every data frame is opened and delivered", ok);
    check(control_rx.load() == kControl, "part2: every control frame is opened and delivered", ok);
    check(sb.replays_rejected() == 0, "part2: 0 replay false positives over real TCP", ok);
    check(sb.tamper_rejected() == 0, "part2: 0 tamper rejections over real TCP", ok);

    std::printf("  part2: data=%llu/%llu control=%llu/%llu replays=%llu tamper=%llu\n",
                static_cast<unsigned long long>(data_rx.load()), static_cast<unsigned long long>(kData),
                static_cast<unsigned long long>(control_rx.load()),
                static_cast<unsigned long long>(kControl),
                static_cast<unsigned long long>(sb.replays_rejected()),
                static_cast<unsigned long long>(sb.tamper_rejected()));
    ta.stop();
    tb.stop();
}
}  // namespace

int main() {
    bool ok = true;
    part1_deterministic(ok);
    part2_real_tcp(ok);
    std::printf("security_secure_transport_lane_reorder_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
