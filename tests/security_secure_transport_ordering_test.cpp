// Tests ADR-040's REQUIRED pre-merge hardening item — "same-session send-path ordering": N
// concurrent senders on the SAME PeerSession must never have wire-arrival order diverge from their
// seq-assignment order. The pre-ADR-040 shape only held the lock across the seq DRAW, then sealed and
// handed off to the wire UNLOCKED — so a thread that drew a LATER seq could physically reach the wire
// before a thread that drew an EARLIER one. That is exactly the failure this test would have exposed:
// a later-seq, earlier-arriving frame advances the receiver's strict-monotonic replay high-water mark,
// so the genuinely earlier-seq frame that then arrives is rejected as a spurious "replay" (ADR-040's
// judge notes, imported from Design 1's proven C14 methodology: seq-draw + seal + wire-handoff must be
// one atomic critical section per session).
//
// ADVERSARIAL: a SlowCipher wraps MockCipher and inserts a bounded busy-yield loop INSIDE seal() —
// this widens the window between "seq drawn" and "frame reaches the wire" as much as possible without
// an unbounded stall, maximizing the chance a race would manifest if the fix's critical section were
// too narrow. 4 sending threads (machine-safety cap) x many sends each, all to the SAME peer/session.
//
// ASSERTIONS: (1) every send reaches the wire — 0 drops; (2) the RECORDED wire-arrival order of seq
// numbers is EXACTLY the strictly-increasing sequence 1..total, matching Design 1's proven "strict
// per-sender FIFO, 0 drops" shape; (3) the receiver opens every frame with 0 tamper/replay rejections
// — a replay-guard false-positive (the exact failure mode this fix closes) would show up here as
// replays_rejected() > 0.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Wraps MockCipher, widening the seq-draw-to-wire window inside seal() with a bounded busy-yield —
// adversarial pressure toward exposing any gap in the required fix's critical section, without an
// unbounded stall (bounded iteration count keeps the test's wall-clock predictable).
class SlowCipher final : public Aead {
public:
    explicit SlowCipher(std::uint64_t key) : inner_(key) {}

    void seal(std::uint64_t nonce, std::span<const std::byte> aad, std::span<const std::byte> plaintext,
              std::vector<std::byte>& out) const override {
        for (int i = 0; i < 64; ++i) std::this_thread::yield();
        inner_.seal(nonce, aad, plaintext, out);
    }
    [[nodiscard]] bool open(std::uint64_t nonce, std::span<const std::byte> aad,
                            std::span<const std::byte> sealed, std::vector<std::byte>& out) const override {
        return inner_.open(nonce, aad, sealed, out);
    }
    [[nodiscard]] std::size_t tag_size() const noexcept override { return inner_.tag_size(); }
    [[nodiscard]] std::size_t seal_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                        std::span<const std::byte> plaintext,
                                        std::span<std::byte> out) const override {
        return inner_.seal_into(nonce, aad, plaintext, out);
    }
    [[nodiscard]] bool open_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                 std::span<const std::byte> sealed, std::span<std::byte> out,
                                 std::size_t& out_len) const override {
        return inner_.open_into(nonce, aad, sealed, out, out_len);
    }

private:
    MockCipher inner_;
};

// Records the exact order frames reach the wire (the order inner_->send() is called), under its own
// lock — this is a TEST recorder, not the invariant under test (that invariant is SecureTransport's
// per-session lock; this class just observes the outcome).
class RecorderTransport final : public Transport {
public:
    RecorderTransport(LoopbackFabric& fabric, NodeId self) : inner_(fabric, self) {}
    void send(NodeId to, MessageFrame frame) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            order_.push_back(frame.payload);  // capture the sealed envelope (seq prefix + ciphertext)
        }
        inner_.send(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_.on_receive(std::move(cb)); }

    [[nodiscard]] std::vector<std::vector<std::byte>> order_copy() const {
        std::lock_guard<std::mutex> g(mu_);
        return order_;
    }

private:
    LoopbackTransport inner_;
    mutable std::mutex mu_;
    std::vector<std::vector<std::byte>> order_;
};

[[nodiscard]] std::uint64_t seq_of(const std::vector<std::byte>& envelope) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(envelope[static_cast<std::size_t>(b)]))
             << (b * 8);
    return v;
}
}  // namespace

int main() {
    bool ok = true;

    LoopbackFabric fabric;
    SlowCipher cipher(0xF00DBEEFULL);

    const NodeId n1{1};
    const NodeId n2{2};

    LoopbackTransport n2_inner(fabric, n2);
    SecureTransport n2_secure(n2_inner, cipher, n2);
    std::atomic<std::uint64_t> delivered{0};
    n2_secure.on_receive([&](MessageFrame) { delivered.fetch_add(1, std::memory_order_relaxed); });

    RecorderTransport n1_inner(fabric, n1);
    SecureTransport n1_secure(n1_inner, cipher, n1);

    constexpr int kThreads = 4;       // machine-safety cap
    constexpr int kPerThread = 2000;  // 8000 total sends on ONE shared session
    constexpr std::uint64_t kTotal = static_cast<std::uint64_t>(kThreads) * kPerThread;

    std::vector<std::thread> senders;
    for (int t = 0; t < kThreads; ++t) {
        senders.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                MessageFrame f{};
                f.from = n1;
                f.to = n2;
                f.target = ActorId{TypeKey{0x1111}, 1};
                f.msg_type = TypeKey{0x2222};
                f.payload = {std::byte{1}};
                n1_secure.send(n2, f);
            }
        });
    }
    for (auto& th : senders) th.join();

    const auto order = n1_inner.order_copy();
    check(order.size() == kTotal, "0 drops: every send reached the wire", ok);

    bool strictly_increasing = true;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (seq_of(order[i]) != i + 1) {
            strictly_increasing = false;
            break;
        }
    }
    check(strictly_increasing,
          "wire-arrival order EXACTLY matches seq order 1..N (required fix: no divergence)", ok);

    check(delivered.load() == kTotal, "receiver opened every frame", ok);
    check(n2_secure.replays_rejected() == 0,
          "0 replay-guard false positives (the exact failure mode the fix closes)", ok);
    check(n2_secure.tamper_rejected() == 0, "0 tamper rejections", ok);

    std::printf(
        "security_secure_transport_ordering_test: %s (sent=%zu delivered=%llu replays_rejected=%llu)\n",
        ok ? "OK" : "FAIL", order.size(), static_cast<unsigned long long>(delivered.load()),
        static_cast<unsigned long long>(n2_secure.replays_rejected()));
    return ok ? 0 : 1;
}
