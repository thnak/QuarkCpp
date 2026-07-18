// Implements 020-Security §2 (transport security seam) — `SecureTransport`, a DECORATOR over the 010
// `Transport` that provides confidentiality + integrity + replay protection per node↔node connection.
// It composes WITH, does not replace, the 010 transport: it wraps an inner `Transport`, seals every
// outbound frame's payload with the AEAD seam (aead.hpp), and opens + replay-checks every inbound frame
// before handing it to the real receiver.
//
// REPLAY PROTECTION (020 §2): a PER-SESSION, strictly-increasing SEQUENCE NUMBER rides in the AEAD
// ASSOCIATED DATA (so it cannot be altered without breaking the tag) and is also the nonce. The receiver
// tracks the highest seq seen per sender and REJECTS any frame whose seq is not strictly greater — a
// captured-and-replayed frame is dropped. This composes with 017's delivery-layer dedup (it does not
// replace it): 017 dedups at-least-once RETRIES by message id; this rejects an ADVERSARIAL replay at
// the wire, before decode.
//
// THE HONEST EXCEPTION (020): the AEAD here is the seam. The default `MockCipher` is NOT real crypto
// (aead.hpp banner). The production handshake (mutual auth, per-session key derivation — paid ONCE per
// peer since 010 multiplexes one connection per peer) + a real AES-GCM/ChaCha20-Poly1305 cipher are a
// DEFERRED thin adapter over mbedTLS/BoringSSL. The "plaintext dev transport" is simply the inner
// `Transport` used WITHOUT this wrapper — labeled dev-only, rejected under Strict+multi-node
// (security.hpp `validate_security`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/audit.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/transport.hpp"
#include "quark/detail/hash.hpp"

namespace quark {

namespace detail {
inline void put_u64_le(std::vector<std::byte>& out, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
}
[[nodiscard]] inline std::uint64_t get_u64_le(std::span<const std::byte> in) noexcept {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[static_cast<std::size_t>(b)]))
             << (b * 8);
    return v;
}
}  // namespace detail

class SecureTransport final : public Transport {
public:
    // Decorate `inner` for THIS node `self`, sealing/opening with `cipher` (the AEAD seam). `cipher`
    // must outlive the transport. In production the per-session keys come from the handshake; the mock
    // shares one key — the framing under test is identical.
    SecureTransport(Transport& inner, const Aead& cipher, NodeId self) noexcept
        : inner_(&inner), cipher_(&cipher), self_(self) {}

    // Optional audit sink for tamper/replay drops (cold path).
    void set_audit_sink(AuditSink sink) noexcept { audit_ = sink; }

    // Seal the payload and forward. AAD = {from,to,target,msg_type,seq} so a frame replayed to another
    // target / with a mutated header / mutated seq fails the tag. `seq` is the per-peer session counter.
    void send(NodeId to, MessageFrame frame) override {
        std::uint64_t seq;
        {
            std::lock_guard<std::mutex> g(mu_);
            seq = ++send_seq_[to.value];
        }
        const std::uint64_t nonce = nonce_for(self_, to, seq);
        std::vector<std::byte> aad = build_aad(self_, to, frame, seq);

        std::vector<std::byte> envelope;
        detail::put_u64_le(envelope, seq);  // seq travels in the clear (it is the nonce) but is AAD-bound
        cipher_->seal(nonce, std::span<const std::byte>(aad),
                      std::span<const std::byte>(frame.payload), envelope);
        frame.payload = std::move(envelope);
        ++sealed_;
        inner_->send(to, std::move(frame));
    }

    // Register the real receiver; interpose the open + replay check.
    void on_receive(std::function<void(MessageFrame)> cb) override {
        receiver_ = std::move(cb);
        inner_->on_receive([this](MessageFrame frame) { this->deliver(std::move(frame)); });
    }

    // --- Test/diagnostic counters -------------------------------------------------------------
    [[nodiscard]] std::uint64_t sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::uint64_t opened() const noexcept { return opened_; }
    [[nodiscard]] std::uint64_t replays_rejected() const noexcept { return replays_rejected_; }
    [[nodiscard]] std::uint64_t tamper_rejected() const noexcept { return tamper_rejected_; }

private:
    void deliver(MessageFrame frame) {
        if (frame.payload.size() < 8) {  // malformed: no seq prefix
            ++tamper_rejected_;
            return;
        }
        const std::uint64_t seq = detail::get_u64_le(std::span<const std::byte>(frame.payload.data(), 8));
        const std::span<const std::byte> sealed(frame.payload.data() + 8, frame.payload.size() - 8);
        const std::uint64_t nonce = nonce_for(frame.from, self_, seq);

        // Reconstruct AAD from the RECEIVED header (the exact bytes the sender bound). A mutated header
        // or seq yields a different AAD ⇒ the tag fails below.
        std::vector<std::byte> aad = build_aad(frame.from, self_, frame, seq);

        std::vector<std::byte> plaintext;
        if (!cipher_->open(nonce, std::span<const std::byte>(aad), sealed, plaintext)) {
            ++tamper_rejected_;
            audit_(AuditRecord{AuditKind::AuthnFailure, errc::serialization, frame.from.value,
                               "secure transport: AEAD open failed (tamper/wrong key)", 0});
            return;
        }
        // Per-session replay guard: strictly-increasing seq per sender. A replayed frame has seq <= the
        // high-water mark ⇒ reject (composes with 017 dedup; this stops an adversarial wire replay).
        {
            std::lock_guard<std::mutex> g(mu_);
            std::uint64_t& high = recv_high_[frame.from.value];
            if (seq <= high) {
                ++replays_rejected_;
                audit_(AuditRecord{AuditKind::AuthnFailure, errc::unavailable, frame.from.value,
                                   "secure transport: replayed sequence number rejected", 0});
                return;
            }
            high = seq;
        }
        ++opened_;
        frame.payload = std::move(plaintext);
        if (receiver_) receiver_(std::move(frame));
    }

    // A session-scoped nonce: fold the ordered (src,dst) pair with the seq. Both peers derive the same
    // value from the frame's from/to, so open matches seal. Unique per (session, seq) — seq is strictly
    // increasing, so a nonce never repeats within a session.
    [[nodiscard]] static std::uint64_t nonce_for(NodeId src, NodeId dst, std::uint64_t seq) noexcept {
        return detail::splitmix64(detail::hash_combine(src.value, dst.value)) ^ seq;
    }

    // AAD = the routing header the receiver must be able to trust: from, to, target actor, msg type, and
    // the replay seq. Authenticated, not encrypted (routing needs it in the clear).
    [[nodiscard]] static std::vector<std::byte> build_aad(NodeId from, NodeId to,
                                                          const MessageFrame& f, std::uint64_t seq) {
        std::vector<std::byte> aad;
        detail::put_u64_le(aad, from.value);
        detail::put_u64_le(aad, to.value);
        detail::put_u64_le(aad, f.target.type.value);
        detail::put_u64_le(aad, f.target.key);
        detail::put_u64_le(aad, f.msg_type.value);
        detail::put_u64_le(aad, seq);
        return aad;
    }

    Transport* inner_;
    const Aead* cipher_;
    NodeId self_;
    std::function<void(MessageFrame)> receiver_;
    AuditSink audit_{};

    std::mutex mu_;
    std::unordered_map<std::uint64_t, std::uint64_t> send_seq_;   // per-peer outbound counter
    std::unordered_map<std::uint64_t, std::uint64_t> recv_high_;  // per-peer inbound high-water mark

    std::uint64_t sealed_ = 0;
    std::uint64_t opened_ = 0;
    std::uint64_t replays_rejected_ = 0;
    std::uint64_t tamper_rejected_ = 0;
};

}  // namespace quark
