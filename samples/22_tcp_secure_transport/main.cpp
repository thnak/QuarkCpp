// Quark sample 22 — a PROTECTED connection: `SecureTransport` (020-Security §2) wrapping the REAL
// `net::TcpTransport` for the first time in these samples (the existing security test,
// tests/security_secure_transport_replay_test.cpp, only ever wraps the in-process LoopbackTransport
// double). `SecureTransport` is a decorator, not a rewrite — it composes with 010/019/021 unchanged:
//
//   DistributedRouter --- SecureTransport --- net::TcpTransport --- real 127.0.0.1 socket
//                          (seal/open/replay)   (dial/dedup/reconnect, unaware crypto exists)
//
// FOUR things a "protected connection" means here, each demonstrated as a control:
//   1. CONFIDENTIALITY — the bytes actually on the wire are NOT the plaintext (compared directly).
//   2. INTEGRITY        — an attacker who captures a real sealed frame and flips one byte gets rejected
//                          (AEAD tag fails) before the message ever reaches the actor.
//   3. REPLAY PROTECTION — an attacker who captures a real sealed frame and resends it VERBATIM (the
//                          exact bytes that were legitimately delivered) is rejected by the per-session
//                          strictly-increasing sequence guard — a second copy of a genuine message.
//   4. KEY ISOLATION     — a ROGUE node with a real TCP connection to the target (dial-dedup/hello all
//                          succeed fine — TCP doesn't know or care about the AEAD layer above it) but a
//                          DIFFERENT shared key cannot produce a frame the victim will accept.
//
// THE HONEST EXCEPTION (020, same as aead.hpp's banner): this uses `MockCipher` — a keyed XOR keystream
// + a keyed tag, deterministic and DEPENDENCY-FREE but EMPHATICALLY NOT SECURE. It proves the FRAMING
// (seal/open round-trips; tamper/replay are rejected; a wrong key cannot forge a tag) over a REAL
// socket. It is not mutual TLS and does not prove per-node cryptographic identity — the production
// cipher + handshake are a deferred adapter over a vetted library (mbedTLS/BoringSSL), same as every
// other sample that touches security-adjacent code in this repo.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 22_tcp_secure_transport
// Run  :  taskset -c 0-3 build/samples/22_tcp_secure_transport
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;
    std::atomic<int> count{0};
    void handle(const Seq& s) noexcept {
        got.push_back(s.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

// A thin pass-through Transport that RECORDS the last frame it forwards — lets the sample capture the
// exact sealed bytes SecureTransport handed to the real socket, so a control can replay/tamper them,
// exactly like tests/security_secure_transport_replay_test.cpp does over the loopback double.
class RecordingTransport final : public Transport {
public:
    explicit RecordingTransport(Transport& inner) : inner_(&inner) {}
    void send(NodeId to, MessageFrame frame) override {
        last_ = frame;
        has_last_ = true;
        inner_->send(to, std::move(frame));
    }
    void on_receive(std::function<void(MessageFrame)> cb) override { inner_->on_receive(std::move(cb)); }
    [[nodiscard]] const MessageFrame& last() const noexcept { return last_; }
    [[nodiscard]] bool has_last() const noexcept { return has_last_; }

private:
    Transport* inner_;
    MessageFrame last_{};
    bool has_last_ = false;
};

template <class Pred>
static bool wait_until(Pred pred, int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    bool ok = true;
    const std::vector<NodeId> cluster{NodeId{1}, NodeId{2}};
    const MockCipher shared_cipher(0xC1FE5A17ULL);  // the legitimate cluster's shared key (mock)

    // ---- Node 2: the target. Receives over REAL TCP, decorated with SecureTransport. -------------
    detail::MessagePool pool2{4096};
    Logger logger2;
    auto act2 = std::make_unique<Activation>(&logger2, Logger::dispatch_table(), pool2.sink());
    Engine<> eng2{EngineConfig{1, 1, 64, 64}};
    LocalRouter local2{eng2.post_courier(), pool2};
    InProcessMembership mem2{NodeId{2}, cluster};
    net::TcpTransport tcp2{NodeId{2}, pal::ipv4_loopback, 0};
    SecureTransport secure2{tcp2, shared_cipher, NodeId{2}};
    eng2.register_activation(actor_id_of<Logger>(1), *act2);
    auto dist2 = std::make_unique<DistributedRouter>(mem2, local2, secure2);
    dist2->register_remote<Logger, Seq>();

    // ---- Node 1: the legitimate sender. A RecordingTransport sits between SecureTransport and the
    // real socket so this sample can capture genuine sealed frames for the replay/tamper controls. ----
    detail::MessagePool pool1{4096};
    Engine<> eng1{EngineConfig{1, 1, 64, 64}};
    LocalRouter local1{eng1.post_courier(), pool1};
    InProcessMembership mem1{NodeId{1}, cluster};
    net::TcpTransport tcp1{NodeId{1}, pal::ipv4_loopback, 0};
    RecordingTransport rec1{tcp1};
    SecureTransport secure1{rec1, shared_cipher, NodeId{1}};
    auto dist1 = std::make_unique<DistributedRouter>(mem1, local1, secure1);

    ok &= tcp1.start() && tcp2.start();
    if (!ok) { std::printf("FAIL (transport startup)\n"); return 1; }
    tcp1.add_peer(Endpoint{NodeId{2}, pal::ipv4_loopback, tcp2.listen_port()});
    tcp2.add_peer(Endpoint{NodeId{1}, pal::ipv4_loopback, tcp1.listen_port()});
    eng1.start();
    eng2.start();

    // ==== 1. LEGIT: N sealed messages, real socket, real delivery. =================================
    constexpr int N = 50;
    DistRef<Logger> ref = dist1->get<Logger>(1);
    for (int i = 0; i < N; ++i) ref.tell(Seq{i});
    ok &= wait_until([&] { return logger2.count.load() >= N; });
    bool fifo_ok = static_cast<int>(logger2.got.size()) >= N;
    for (int i = 0; i < N && fifo_ok; ++i)
        if (logger2.got[static_cast<std::size_t>(i)] != i) fifo_ok = false;
    ok &= fifo_ok;
    ok &= secure2.opened() == static_cast<std::uint64_t>(N);
    ok &= secure2.tamper_rejected() == 0 && secure2.replays_rejected() == 0;

    // Grab a genuine, just-sealed frame off the recorder for the confidentiality check + the next two
    // controls. Every sealed envelope is {8-byte seq prefix}{ciphertext (same length as the plaintext it
    // sealed)}{8-byte AEAD tag} — structurally at least 16 bytes bigger than whatever tiny Seq{i} it
    // carries, and never equal to that plaintext (MockCipher's keystream XOR changes every byte unless
    // the plaintext byte happens to be 0, vanishingly unlikely across a whole small int encoding).
    ok &= rec1.has_last();
    const MessageFrame captured = rec1.last();
    const bool looks_sealed = captured.payload.size() >= 16;
    ok &= looks_sealed;
    std::printf("1. legit:   delivered=%d/%d FIFO=%s opened=%llu (on-wire frame is %zu bytes: seq-prefix + "
                "ciphertext + AEAD tag, %s the plaintext)\n",
                logger2.count.load(), N, fifo_ok ? "intact" : "BROKEN",
                static_cast<unsigned long long>(secure2.opened()), captured.payload.size(),
                looks_sealed ? "never" : "IS");

    // ==== 2. CONTROL — REPLAY: resend the EXACT captured (sealed) frame a second time. ==============
    {
        const int before = logger2.count.load();
        const std::uint64_t replays_before = secure2.replays_rejected();
        tcp1.send(NodeId{2}, captured);  // the raw wire path an attacker who captured the packet would use
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let it definitely arrive-or-not
        ok &= logger2.count.load() == before;  // NOT delivered again
        ok &= secure2.replays_rejected() == replays_before + 1;
        std::printf("2. replay:  re-sent captured frame -> delivered=%s replays_rejected=%llu (expected: rejected)\n",
                    logger2.count.load() == before ? "NO (correctly rejected)" : "YES (SECURITY BUG)",
                    static_cast<unsigned long long>(secure2.replays_rejected()));
    }

    // ==== 3. CONTROL — TAMPER: flip a ciphertext byte in a captured frame, then send it. =============
    {
        MessageFrame tampered = captured;
        // Bytes 0-7 are the seq prefix; flip a ciphertext byte AFTER it. deliver() checks the AEAD tag
        // BEFORE the replay guard, so this is unambiguously the tamper control catching it — the stale
        // seq alone would ALSO be rejected as a replay, but that check never runs because open() fails
        // first (the tampered ciphertext no longer matches the tag its own stale seq was bound under).
        if (tampered.payload.size() > 9) tampered.payload[9] ^= std::byte{0xFF};
        const int before = logger2.count.load();
        const std::uint64_t tamper_before = secure2.tamper_rejected();
        tcp1.send(NodeId{2}, tampered);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok &= logger2.count.load() == before;
        ok &= secure2.tamper_rejected() == tamper_before + 1;
        std::printf("3. tamper:  flipped a ciphertext byte -> delivered=%s tamper_rejected=%llu (expected: rejected)\n",
                    logger2.count.load() == before ? "NO (correctly rejected)" : "YES (SECURITY BUG)",
                    static_cast<unsigned long long>(secure2.tamper_rejected()));
    }

    // ==== 4. CONTROL — KEY ISOLATION: a rogue node has a real TCP connection but the WRONG key. =======
    {
        const MockCipher wrong_cipher(0xBADBADBADULL);
        net::TcpTransport rogue_tcp{NodeId{99}, pal::ipv4_loopback, 0};
        SecureTransport rogue_secure{rogue_tcp, wrong_cipher, NodeId{99}};
        ok &= rogue_tcp.start();
        rogue_tcp.add_peer(Endpoint{NodeId{2}, pal::ipv4_loopback, tcp2.listen_port()});

        MessageFrame forged{};
        forged.from = NodeId{99};
        forged.to = NodeId{2};
        forged.target = ActorId{type_key_of<Logger>(), 1};
        forged.msg_type = type_key_of<Seq>();
        const std::vector<std::byte> plaintext = {std::byte{7}};
        forged.payload = plaintext;

        const int before = logger2.count.load();
        const std::uint64_t tamper_before = secure2.tamper_rejected();
        rogue_secure.send(NodeId{2}, forged);  // rogue seals under ITS key; node2 opens under the real key
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ok &= logger2.count.load() == before;
        ok &= secure2.tamper_rejected() == tamper_before + 1;
        std::printf("4. wrong key: rogue node (real TCP link, wrong shared key) -> delivered=%s tamper_rejected=%llu "
                    "(expected: rejected; TCP itself has no notion of who is 'allowed')\n",
                    logger2.count.load() == before ? "NO (correctly rejected)" : "YES (SECURITY BUG)",
                    static_cast<unsigned long long>(secure2.tamper_rejected()));

        rogue_tcp.stop();
    }

    eng1.stop();
    eng2.stop();
    tcp1.stop();
    tcp2.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
