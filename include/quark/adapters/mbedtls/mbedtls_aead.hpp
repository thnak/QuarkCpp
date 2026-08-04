// Implements ADR-040 Phase 4 — `MbedtlsAeadGcm`, the real AES-128-GCM `Aead` (aead.hpp) the design
// record adopted over `MockCipher`. Opt-in adapter: only compiled/linked when QUARK_WITH_MBEDTLS=ON
// (cmake/QuarkSecurityAdapters.cmake) — the core never sees mbedTLS.
//
// THREAD SAFETY (load-bearing, see secure_transport.hpp's required-fix banner): mbedTLS's one-shot
// GCM calls (`mbedtls_gcm_crypt_and_tag`/`mbedtls_gcm_auth_decrypt`) internally drive the context's
// streaming state (starts/update/finish) — concurrent calls on the SAME `mbedtls_gcm_context` race.
// This is safe here ONLY because of how ADR-040 uses it: each `MbedtlsAeadGcm` instance becomes
// EXACTLY ONE `PeerSession`'s send_cipher_ OR recv_cipher_ (never shared across sessions, never used
// for both directions), and `PeerSession::session_mu_` already serializes every seal()/open() call on
// a given instance (secure_transport.hpp's send()/deliver()). Do not share one instance across
// sessions or call it without that lock held.
#pragma once

#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <mbedtls/gcm.h>

#include "quark/core/aead.hpp"

namespace quark::adapters {

class MbedtlsAeadGcm final : public Aead {
public:
    static constexpr std::size_t kKeyBytes = 16;  // AES-128 (ADR-040's adopted key size)
    static constexpr std::size_t kTagBytes = 16;  // standard GCM tag
    static constexpr std::size_t kIvBytes = 12;   // standard GCM nonce length

    explicit MbedtlsAeadGcm(std::span<const std::byte, kKeyBytes> key) {
        mbedtls_gcm_init(&ctx_);
        const int rc = mbedtls_gcm_setkey(&ctx_, MBEDTLS_CIPHER_ID_AES,
                                          reinterpret_cast<const unsigned char*>(key.data()),
                                          static_cast<unsigned int>(kKeyBytes * 8));
        if (rc != 0) {
            mbedtls_gcm_free(&ctx_);
            throw std::runtime_error("MbedtlsAeadGcm: mbedtls_gcm_setkey failed");
        }
    }
    ~MbedtlsAeadGcm() override { mbedtls_gcm_free(&ctx_); }
    MbedtlsAeadGcm(const MbedtlsAeadGcm&) = delete;
    MbedtlsAeadGcm& operator=(const MbedtlsAeadGcm&) = delete;

    void seal(std::uint64_t nonce, std::span<const std::byte> aad, std::span<const std::byte> plaintext,
              std::vector<std::byte>& out) const override {
        const std::size_t base = out.size();
        out.resize(base + plaintext.size() + kTagBytes);
        (void)seal_into(nonce, aad, plaintext,
                       std::span<std::byte>(out.data() + base, plaintext.size() + kTagBytes));
    }

    [[nodiscard]] bool open(std::uint64_t nonce, std::span<const std::byte> aad,
                            std::span<const std::byte> sealed, std::vector<std::byte>& out) const override {
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
        unsigned char iv[kIvBytes];
        nonce_to_iv(nonce, iv);
        const int rc = mbedtls_gcm_crypt_and_tag(
            &ctx_, MBEDTLS_GCM_ENCRYPT, plaintext.size(), iv, kIvBytes,
            reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            reinterpret_cast<unsigned char*>(out.data()), kTagBytes,
            reinterpret_cast<unsigned char*>(out.data()) + plaintext.size());
        if (rc != 0) {
            // Unreachable in correct usage (fixed key/iv/tag lengths); fail closed rather than emit
            // an unsealed/garbage envelope.
            std::memset(out.data(), 0, out.size());
            return 0;
        }
        return plaintext.size() + kTagBytes;
    }

    [[nodiscard]] bool open_into(std::uint64_t nonce, std::span<const std::byte> aad,
                                 std::span<const std::byte> sealed, std::span<std::byte> out,
                                 std::size_t& out_len) const override {
        if (sealed.size() < kTagBytes) return false;
        const std::size_t ct_len = sealed.size() - kTagBytes;
        if (out.size() < ct_len) return false;
        unsigned char iv[kIvBytes];
        nonce_to_iv(nonce, iv);
        const int rc = mbedtls_gcm_auth_decrypt(
            &ctx_, ct_len, iv, kIvBytes, reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
            reinterpret_cast<const unsigned char*>(sealed.data()) + ct_len, kTagBytes,
            reinterpret_cast<const unsigned char*>(sealed.data()), reinterpret_cast<unsigned char*>(out.data()));
        if (rc != 0) return false;  // MBEDTLS_ERR_GCM_AUTH_FAILED: tamper / wrong aad / wrong key
        out_len = ct_len;
        return true;
    }

private:
    // Our nonce is an 8-byte, session-scoped, strictly-monotonic counter (secure_transport.hpp's
    // nonce_for) — zero-extend to the standard 12-byte GCM IV. Uniqueness-per-key is inherited from
    // the caller's strict seq monotonicity, which is what GCM actually requires (the IV need not be
    // random, only ever-repeating under the same key).
    static void nonce_to_iv(std::uint64_t nonce, unsigned char (&iv)[kIvBytes]) noexcept {
        std::memset(iv, 0, kIvBytes);
        for (int b = 0; b < 8; ++b)
            iv[kIvBytes - 8 + b] = static_cast<unsigned char>((nonce >> (b * 8)) & 0xFF);
    }

    mutable mbedtls_gcm_context ctx_;
};

}  // namespace quark::adapters
