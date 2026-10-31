// Implements 020-Security §2 (transport AEAD) and §"Pinned-key mode (Noise IK, #40)" — `SodiumAeadChaCha20Poly1305`,
// the libsodium ChaCha20-Poly1305 (IETF) `Aead` (aead.hpp): the SECOND real cipher behind the AEAD seam, beside
// mbedtls_aead.hpp's AES-128-GCM, so a pinned-mode build needs libsodium only (no mbedTLS). Opt-in adapter:
// compiled only with QUARK_WITH_SODIUM=ON; the core never sees libsodium.
//
// NONCE: Quark's per-lane (lane bit, strictly increasing seq) u64 (secure_transport.hpp `nonce_for`) becomes the
// 12-byte IETF nonce as 4 zero bytes || LE64 — the same layout Noise uses for its transport CipherState. Nonce
// uniqueness per key is inherited from the caller's per-lane monotonic seq, which is all ChaCha20-Poly1305 needs.
//
// THREAD SAFETY: unlike the mbedTLS GCM context, libsodium's one-shot AEAD calls keep no state beyond the const key,
// so one instance is safe to call concurrently. (PeerSession still serialises per session; nothing relies on it here.)
#pragma once

#include <sodium.h>

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "quark/adapters/sodium/noise.hpp"
#include "quark/core/aead.hpp"

namespace quark::adapters {

class SodiumAeadChaCha20Poly1305 final : public Aead {
public:
    static constexpr std::size_t kKeyBytes = noise::kKeyLen;  // 32
    static constexpr std::size_t kTagBytes = noise::kTagLen;  // 16
    static constexpr std::size_t kNonceBytes = 12;

    explicit SodiumAeadChaCha20Poly1305(std::span<const std::byte, kKeyBytes> key) {
        std::memcpy(key_.data(), key.data(), kKeyBytes);
    }
    ~SodiumAeadChaCha20Poly1305() override { noise::wipe(key_); }
    SodiumAeadChaCha20Poly1305(const SodiumAeadChaCha20Poly1305&) = delete;
    SodiumAeadChaCha20Poly1305& operator=(const SodiumAeadChaCha20Poly1305&) = delete;

    void seal(std::uint64_t nonce, std::span<const std::byte> aad, std::span<const std::byte> plaintext,
              std::vector<std::byte>& out) const override {
        const std::size_t base = out.size();
        out.resize(base + plaintext.size() + kTagBytes);
        (void)seal_into(nonce, aad, plaintext, std::span<std::byte>(out.data() + base, plaintext.size() + kTagBytes));
    }

    [[nodiscard]] bool open(std::uint64_t nonce, std::span<const std::byte> aad, std::span<const std::byte> sealed,
                            std::vector<std::byte>& out) const override {
        out.clear();
        if (sealed.size() < kTagBytes) return false;
        out.resize(sealed.size() - kTagBytes);
        std::size_t out_len = 0;
        if (!open_into(nonce, aad, sealed, std::span<std::byte>(out), out_len)) {
            out.clear();
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t tag_size() const noexcept override { return kTagBytes; }

    [[nodiscard]] std::size_t seal_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                        std::span<const std::byte> plaintext,
                                        std::span<std::byte> out) const override {
        if (!noise::init() || out.size() < plaintext.size() + kTagBytes) return 0;
        unsigned char npub[kNonceBytes];
        make_nonce(nonce, npub);
        unsigned long long clen = 0;
        ::crypto_aead_chacha20poly1305_ietf_encrypt(noise::uc(out.data()), &clen,
                                                    plaintext.empty() ? nullptr : noise::uc(plaintext.data()),
                                                    plaintext.size(), aad.empty() ? nullptr : noise::uc(aad.data()),
                                                    aad.size(), nullptr, npub, noise::uc(key_.data()));
        return static_cast<std::size_t>(clen);
    }

    [[nodiscard]] bool open_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                 std::span<const std::byte> sealed, std::span<std::byte> out,
                                 std::size_t& out_len) const override {
        if (!noise::init() || sealed.size() < kTagBytes) return false;
        const std::size_t ct_len = sealed.size() - kTagBytes;
        if (out.size() < ct_len) return false;
        unsigned char npub[kNonceBytes];
        make_nonce(nonce, npub);
        unsigned char dummy = 0;  // libsodium must not be handed a null output even for an empty plaintext
        unsigned long long mlen = 0;
        const int rc = ::crypto_aead_chacha20poly1305_ietf_decrypt(
            ct_len == 0 ? &dummy : noise::uc(out.data()), &mlen, nullptr, noise::uc(sealed.data()), sealed.size(),
            aad.empty() ? nullptr : noise::uc(aad.data()), aad.size(), npub, noise::uc(key_.data()));
        if (rc != 0) return false;  // tamper / wrong aad / wrong nonce / wrong key
        out_len = ct_len;
        return true;
    }

private:
    static void make_nonce(std::uint64_t nonce, unsigned char (&npub)[kNonceBytes]) noexcept {
        std::memset(npub, 0, 4);
        for (int b = 0; b < 8; ++b) npub[4 + b] = static_cast<unsigned char>((nonce >> (b * 8)) & 0xFF);
    }

    noise::Key32 key_{};
};

}  // namespace quark::adapters
