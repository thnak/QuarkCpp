// Implements 020-Security §2 (transport AEAD) + §5 (at-rest AEAD) — the shared AEAD SEAM plus a MOCK
// cipher used ONLY to exercise the seam wiring in tests. Both the secure transport (secure_transport.hpp)
// and at-rest envelope encryption (at_rest.hpp) seal/open through this one interface.
//
// ============================================================================================
//  !!! THE HONEST EXCEPTION (020 §"The honest exception: crypto is not self-implemented") !!!
//  There is NO std-only default that performs REAL cryptography. Rolling in-house AEAD/TLS is the
//  classic source of catastrophic CVEs. The PRODUCTION cipher is a thin DEFERRED ADAPTER over a
//  vetted library (mbedTLS or BoringSSL — AES-GCM / ChaCha20-Poly1305), linked ONLY when a secure
//  cluster or at-rest encryption is configured. It is NOT implemented here.
//
//  `MockCipher` below is a KEYED XOR KEYSTREAM + a KEYED TAG. It is deterministic, dependency-free,
//  and EMPHATICALLY NOT SECURE — a first-year attacker breaks it. Its ONLY purpose is to let the tests
//  prove the FRAMING is correct: that seal/open round-trips, that tampering the ciphertext OR the
//  associated data (a replay sequence number, a fencing token) makes `open` FAIL. Never present it as
//  secure; never use it outside tests.
// ============================================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "quark/detail/hash.hpp"

namespace quark {

// The AEAD seam. `seal` binds `aad` (associated data — authenticated but not encrypted: the replay
// sequence number, the fencing token, frame headers) to the ciphertext under `nonce`; `open` recovers
// the plaintext ONLY if the tag over (key, nonce, aad, ciphertext) verifies — so any tampering with the
// aad or ciphertext is rejected. This is the shape a real AES-GCM adapter fills; the mock fills it too.
class Aead {
public:
    virtual ~Aead() = default;

    // Seal `plaintext` under `nonce`, authenticating `aad`. Appends {ciphertext || tag} to `out`.
    virtual void seal(std::uint64_t nonce, std::span<const std::byte> aad,
                      std::span<const std::byte> plaintext, std::vector<std::byte>& out) const = 0;

    // Open `sealed` (== {ciphertext || tag}) under `nonce`, verifying `aad`. On success writes the
    // recovered plaintext to `out` and returns true. On a tag mismatch (tamper / wrong aad / wrong
    // nonce / wrong key) returns false and leaves `out` empty — the caller drops the frame/record.
    [[nodiscard]] virtual bool open(std::uint64_t nonce, std::span<const std::byte> aad,
                                    std::span<const std::byte> sealed,
                                    std::vector<std::byte>& out) const = 0;

    [[nodiscard]] virtual std::size_t tag_size() const noexcept = 0;

    // ADDITIVE (voice_channel.hpp / ADR-030, claim F1): fixed-buffer, non-allocating seal/open. `out`
    // must be caller-owned (stack/thread_local) and at least `plaintext.size() + tag_size()` bytes;
    // writes {ciphertext||tag} into it and returns the length written. Same authentication semantics as
    // `seal`/`open` above — this ONLY changes where the bytes land (no std::vector growth on the
    // datagram hot path), never the framing/security contract.
    [[nodiscard]] virtual std::size_t seal_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                                std::span<const std::byte> plaintext,
                                                std::span<std::byte> out) const = 0;

    // Fixed-buffer `open`: on success writes the recovered plaintext into `out` (must be >=
    // sealed.size() - tag_size()) and sets `out_len`; returns true. On tamper/wrong-key returns false
    // and leaves `out_len` untouched.
    [[nodiscard]] virtual bool open_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                         std::span<const std::byte> sealed, std::span<std::byte> out,
                                         std::size_t& out_len) const = 0;
};

// ============================================================================================
// MockCipher — NOT REAL CRYPTO (see the header banner). Keyed splitmix64 keystream XOR + a keyed
// 8-byte tag folded over (key, nonce, aad, ciphertext). Enough to make the seam's framing testable:
// round-trip works; any aad/ciphertext/nonce mutation breaks the tag so `open` fails.
// ============================================================================================
class MockCipher final : public Aead {
public:
    explicit MockCipher(std::uint64_t key) noexcept : key_(key) {}

    static constexpr std::size_t kTagSize = 8;

    void seal(std::uint64_t nonce, std::span<const std::byte> aad,
              std::span<const std::byte> plaintext, std::vector<std::byte>& out) const override {
        const std::size_t base = out.size();
        out.resize(base + plaintext.size() + kTagSize);
        // XOR keystream (NOT SECURE): a splitmix64 stream keyed on (key, nonce, block index).
        for (std::size_t i = 0; i < plaintext.size(); ++i)
            out[base + i] = plaintext[i] ^ keystream_byte(nonce, i);
        // Keyed tag over aad + ciphertext (NOT a real MAC): folds every aad + ciphertext byte in.
        const std::uint64_t tag = compute_tag(nonce, aad,
                                              std::span<const std::byte>(out.data() + base, plaintext.size()));
        for (std::size_t b = 0; b < kTagSize; ++b)
            out[base + plaintext.size() + b] = static_cast<std::byte>((tag >> (b * 8)) & 0xFF);
    }

    [[nodiscard]] bool open(std::uint64_t nonce, std::span<const std::byte> aad,
                            std::span<const std::byte> sealed,
                            std::vector<std::byte>& out) const override {
        out.clear();
        if (sealed.size() < kTagSize) return false;  // malformed: no room for a tag
        const std::size_t ct_len = sealed.size() - kTagSize;
        const std::span<const std::byte> ct(sealed.data(), ct_len);
        const std::uint64_t want = compute_tag(nonce, aad, ct);
        std::uint64_t got = 0;
        for (std::size_t b = 0; b < kTagSize; ++b)
            got |= static_cast<std::uint64_t>(static_cast<unsigned char>(sealed[ct_len + b])) << (b * 8);
        if (got != want) return false;  // tamper / wrong aad (replay seq, fencing token) / wrong key
        out.resize(ct_len);
        for (std::size_t i = 0; i < ct_len; ++i) out[i] = ct[i] ^ keystream_byte(nonce, i);
        return true;
    }

    [[nodiscard]] std::size_t tag_size() const noexcept override { return kTagSize; }

    // ADDITIVE fixed-buffer forms (F1): identical algorithm to seal()/open() above, writing directly
    // into caller-supplied storage instead of growing a std::vector. Zero heap allocation.
    [[nodiscard]] std::size_t seal_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                        std::span<const std::byte> plaintext,
                                        std::span<std::byte> out) const override {
        for (std::size_t i = 0; i < plaintext.size(); ++i)
            out[i] = plaintext[i] ^ keystream_byte(nonce, i);
        const std::uint64_t tag =
            compute_tag(nonce, aad, std::span<const std::byte>(out.data(), plaintext.size()));
        for (std::size_t b = 0; b < kTagSize; ++b)
            out[plaintext.size() + b] = static_cast<std::byte>((tag >> (b * 8)) & 0xFF);
        return plaintext.size() + kTagSize;
    }

    [[nodiscard]] bool open_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                 std::span<const std::byte> sealed, std::span<std::byte> out,
                                 std::size_t& out_len) const override {
        if (sealed.size() < kTagSize) return false;
        const std::size_t ct_len = sealed.size() - kTagSize;
        if (out.size() < ct_len) return false;
        const std::span<const std::byte> ct(sealed.data(), ct_len);
        const std::uint64_t want = compute_tag(nonce, aad, ct);
        std::uint64_t got = 0;
        for (std::size_t b = 0; b < kTagSize; ++b)
            got |= static_cast<std::uint64_t>(static_cast<unsigned char>(sealed[ct_len + b])) << (b * 8);
        if (got != want) return false;
        for (std::size_t i = 0; i < ct_len; ++i) out[i] = ct[i] ^ keystream_byte(nonce, i);
        out_len = ct_len;
        return true;
    }

private:
    [[nodiscard]] std::byte keystream_byte(std::uint64_t nonce, std::size_t i) const noexcept {
        const std::uint64_t word = detail::splitmix64(key_ ^ detail::splitmix64(nonce + (i / 8)));
        return static_cast<std::byte>((word >> ((i % 8) * 8)) & 0xFF);
    }
    [[nodiscard]] std::uint64_t compute_tag(std::uint64_t nonce, std::span<const std::byte> aad,
                                            std::span<const std::byte> ct) const noexcept {
        std::uint64_t h = detail::hash_combine(key_, nonce);
        h = detail::hash_combine(h, aad.size());
        for (std::byte b : aad)
            h = detail::hash_combine(h, static_cast<std::uint64_t>(static_cast<unsigned char>(b)));
        h = detail::hash_combine(h, ct.size());
        for (std::byte b : ct)
            h = detail::hash_combine(h, static_cast<std::uint64_t>(static_cast<unsigned char>(b)));
        return h;
    }

    std::uint64_t key_;
};

}  // namespace quark
