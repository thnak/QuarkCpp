// Tests 020-Security §2 — the SecureTransport decorator over the 010 Transport: it seals every
// outbound frame's payload (AEAD) and opens + REPLAY-checks every inbound frame before the real receiver
// sees it. A per-session, strictly-increasing sequence number rides in the AEAD associated data (so it
// cannot be altered without breaking the tag) and gives replay protection, composing with — not
// replacing —017 dedup.
//
// CONTROL (adversarial, must FIRE): capture a legitimately-sealed frame off the wire and REPLAY it. The
// receiver's per-session high-water mark rejects the second copy (same seq) — replays_rejected fires. A
// second control tampers a sealed byte and shows the AEAD open rejects it. Uses the MOCK cipher
// (aead.hpp) — NOT real crypto; it exercises the framing/replay wiring only.
#include <cstdio>
#include <span>
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

// A tap over the loopback that RECORDS the exact (sealed) frame it forwards, so a test can replay it.
class RecorderTransport final : public Transport {
public:
    RecorderTransport(LoopbackFabric& fabric, NodeId self) : inner_(fabric, self) {}
    void send(NodeId to, MessageFrame frame) override {
        last_ = frame;  // copy the on-wire (sealed) frame before it goes out
        has_last_ = true;
        inner_.send(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_.on_receive(std::move(cb)); }

    [[nodiscard]] const MessageFrame& last() const noexcept { return last_; }
    [[nodiscard]] bool has_last() const noexcept { return has_last_; }

private:
    LoopbackTransport inner_;
    MessageFrame last_{};
    bool has_last_ = false;
};
}  // namespace

int main() {
    bool ok = true;

    LoopbackFabric fabric;
    MockCipher cipher(0xC1FE5A17ULL);  // shared key (mock); production keys come from handshake

    const NodeId n1{1};
    const NodeId n2{2};

    // Node 2: secure transport over the plain loopback; records what it receives (decrypted).
    LoopbackTransport n2_inner(fabric, n2);
    SecureTransport n2_secure(n2_inner, cipher, n2);
    std::vector<std::byte> received;
    int recv_count = 0;
    n2_secure.on_receive([&](MessageFrame f) {
        received = f.payload;  // this is the OPENED plaintext
        ++recv_count;
    });

    // Node 1: secure transport over a RECORDING loopback so we can capture the sealed frame for replay.
    RecorderTransport n1_inner(fabric, n1);
    SecureTransport n1_secure(n1_inner, cipher, n1);

    // --- Legit send: seal → wire → open → deliver. --------------------------------------------------
    MessageFrame f{};
    f.from = n1;
    f.to = n2;
    f.target = ActorId{TypeKey{0xAAAA}, 7};
    f.msg_type = TypeKey{0xBBBB};
    const std::vector<std::byte> plaintext = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    f.payload = plaintext;
    n1_secure.send(n2, f);

    check(recv_count == 1, "legit frame delivered once", ok);
    check(received == plaintext, "receiver got the OPENED plaintext (seal/open round-trip)", ok);
    check(n2_secure.opened() == 1, "one frame opened", ok);
    check(n2_secure.replays_rejected() == 0, "no replay yet", ok);
    check(n1_inner.has_last(), "recorded the sealed on-wire frame", ok);
    // The recorded on-wire payload is the sealed envelope (seq prefix + ciphertext + tag), NOT plaintext.
    check(n1_inner.last().payload != plaintext, "on-wire frame is sealed (not plaintext)", ok);

    // --- CONTROL: REPLAY the captured sealed frame directly onto node 2's endpoint. -----------------
    {
        MessageFrame replay = n1_inner.last();  // exact bytes that were legitimately delivered
        fabric.send(n2, replay);                // re-inject → node2_secure.deliver
        check(recv_count == 1, "CONTROL: replayed frame NOT delivered to the receiver", ok);
        check(n2_secure.replays_rejected() == 1, "CONTROL: replay rejected by the per-session seq guard", ok);
        check(n2_secure.opened() == 1, "CONTROL: opened count unchanged (no duplicate open)", ok);
    }

    // --- CONTROL: TAMPER a sealed byte ⇒ AEAD open rejects it. --------------------------------------
    {
        MessageFrame tampered = n1_inner.last();
        // Flip a ciphertext byte (skip the 8-byte seq prefix so it stays a valid, higher-looking seq).
        if (tampered.payload.size() > 9) tampered.payload[9] ^= std::byte{0xFF};
        fabric.send(n2, tampered);
        check(recv_count == 1, "CONTROL: tampered frame NOT delivered", ok);
        check(n2_secure.tamper_rejected() >= 1, "CONTROL: tampered frame rejected by AEAD open", ok);
    }

    // --- A SECOND legit send advances the session seq and IS delivered (guard is not a blanket block).
    {
        MessageFrame f2 = f;
        f2.payload = {std::byte{9}, std::byte{9}};
        n1_secure.send(n2, f2);
        check(recv_count == 2, "a fresh (higher-seq) frame is delivered normally", ok);
        check(n2_secure.opened() == 2, "second legit frame opened", ok);
    }

    std::printf("security_secure_transport_replay_test: %s (opened=%llu replays=%llu tamper=%llu)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(n2_secure.opened()),
                static_cast<unsigned long long>(n2_secure.replays_rejected()),
                static_cast<unsigned long long>(n2_secure.tamper_rejected()));
    return ok ? 0 : 1;
}
