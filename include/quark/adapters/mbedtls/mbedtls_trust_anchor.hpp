// Implements ADR-067 D1/D2 — `MbedtlsTrustAnchorVerifier`, the mbedTLS implementation of the verify-only
// `TrustAnchorVerifier` seam (signed_revocation.hpp). It holds NO private key: it verifies an
// ECDSA-with-SHA-256 signature against the PUBLIC key of a trusted root, selected by `anchor_key_id`
// (SHA-256 of the root's SubjectPublicKeyInfo DER), from the cluster `TrustStore` — current roots plus
// the still-in-window previous roots (ADR-040 overlap). A root is a revocation signer only if it is a CA
// (basicConstraints CA:TRUE, and keyCertSign when keyUsage is present) with an ECDSA P-256 key
// (ADR-067 D1); any other certificate in a bundle never verifies.
//
// The bundles are parsed with the SAME walker the TLS handshake uses (mbedtls_x509_util.hpp), so both
// trust exactly the same roots, and the parse is cached per TrustStore generation. All calls are
// serialised by one mutex: mbedTLS may lazily write precomputed tables into a shared EC group during
// verification, so a parsed key must not be used concurrently. Cold path (one call per never-seen
// revoked fingerprint, rate-limited by RevocationRegistry).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_x509_util.hpp"
#include "quark/core/signed_revocation.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark::adapters {

namespace detail {
// SHA-256 over the DER SubjectPublicKeyInfo of `pk` (mbedtls_pk_write_pubkey_der writes at the END).
[[nodiscard]] inline bool anchor_key_id_of(const mbedtls_pk_context& pk, AnchorKeyId& out) {
    std::array<unsigned char, 512> buf{};
    const int len = mbedtls_pk_write_pubkey_der(&pk, buf.data(), buf.size());
    if (len <= 0) return false;
    return mbedtls_sha256(buf.data() + buf.size() - static_cast<std::size_t>(len), static_cast<std::size_t>(len),
                          reinterpret_cast<unsigned char*>(out.data()), 0) == 0;
}
}  // namespace detail

// The anchor key id of one root certificate (DER) — what the application's custodian stamps into a
// SignedRevocation it signs with that root's key.
[[nodiscard]] inline bool anchor_key_id_of_cert(std::span<const std::byte> cert_der, AnchorKeyId& out) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    const bool ok =
        mbedtls_x509_crt_parse_der(&crt, reinterpret_cast<const unsigned char*>(cert_der.data()), cert_der.size()) == 0 &&
        detail::anchor_key_id_of(crt.pk, out);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

class MbedtlsTrustAnchorVerifier final : public TrustAnchorVerifier {
public:
    using ClockFn = std::int64_t (*)(void* ctx) noexcept;  // mirrors SecureTransport::ClockFn

    // `trust` must outlive the verifier — and so every RevocationRegistry it is installed in, which holds
    // the verifier by shared_ptr (declare the TrustStore first / own both in one object). Pass the SAME clock the TrustStore's rotations are driven by
    // (SecureTransport::set_clock's); the default is the real steady clock, SecureTransport's default.
    explicit MbedtlsTrustAnchorVerifier(const TrustStore& trust, ClockFn clock = &real_steady_ns,
                                        void* clock_ctx = nullptr) noexcept
        : trust_(trust), clock_(clock), clock_ctx_(clock_ctx) {}

    [[nodiscard]] bool verify(const AnchorKeyId& anchor_key_id, std::span<const std::byte> message,
                              std::span<const std::byte> signature) const override {
        if (signature.empty() || signature.size() > kMaxRevocationSignatureBytes) return false;
        std::array<unsigned char, 32> hash{};
        if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(message.data()), message.size(), hash.data(), 0) != 0)
            return false;
        std::lock_guard<std::mutex> g(mu_);
        Signer* s = find_locked(anchor_key_id, /*current_only=*/false);
        if (s == nullptr) return false;
        return mbedtls_pk_verify(&s->cert->pk, MBEDTLS_MD_SHA256, hash.data(), hash.size(),
                                 reinterpret_cast<const unsigned char*>(signature.data()), signature.size()) == 0;
    }

    [[nodiscard]] bool is_current_root(const AnchorKeyId& anchor_key_id) const override {
        std::lock_guard<std::mutex> g(mu_);
        return find_locked(anchor_key_id, /*current_only=*/true) != nullptr;
    }

private:
    struct Signer {
        AnchorKeyId id{};
        mbedtls_x509_crt* cert = nullptr;  // owned by Roots::chain
        bool current = false;
    };
    // The parsed roots of one TrustStore generation. Owns the mbedTLS chain.
    struct Roots {
        mbedtls_x509_crt chain;
        std::vector<Signer> signers;
        std::uint64_t generation = 0, previous_generation = 0;
        bool has_previous = false;
        Roots() { mbedtls_x509_crt_init(&chain); }
        ~Roots() { mbedtls_x509_crt_free(&chain); }
        Roots(const Roots&) = delete;
        Roots& operator=(const Roots&) = delete;
    };

    static std::int64_t real_steady_ns(void*) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // A certificate may sign revocations only if it is a CA with an ECDSA P-256 key (ADR-067 D1).
    [[nodiscard]] static bool eligible_signer(const mbedtls_x509_crt& c) {
        return mbedtls_x509_crt_get_ca_istrue(&c) == 1 &&
               mbedtls_x509_crt_check_key_usage(&c, MBEDTLS_X509_KU_KEY_CERT_SIGN) == 0 &&
               mbedtls_pk_get_type(&c.pk) == MBEDTLS_PK_ECKEY &&
               mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(c.pk)) == MBEDTLS_ECP_DP_SECP256R1;
    }

    // Append the eligible signers of one bundle. A bundle that does not parse contributes nothing.
    static void add_bundle(Roots& r, const TrustedRoots* bundle, bool current) {
        if (bundle == nullptr || bundle->ca_chain_der.empty()) return;
        mbedtls_x509_crt* before_tail = &r.chain;
        const bool chain_empty = r.chain.raw.len == 0;
        if (!chain_empty)
            while (before_tail->next != nullptr) before_tail = before_tail->next;
        if (!detail::parse_der_concat(r.chain, bundle->ca_chain_der)) return;
        for (mbedtls_x509_crt* c = chain_empty ? &r.chain : before_tail->next; c != nullptr && c->raw.len != 0;
             c = c->next) {
            Signer s;
            if (!eligible_signer(*c) || !detail::anchor_key_id_of(c->pk, s.id)) continue;
            s.cert = c;
            s.current = current;
            r.signers.push_back(s);
        }
    }

    Signer* find_locked(const AnchorKeyId& id, bool current_only) const {
        const TrustStore::Snapshot snap = trust_.snapshot(clock_(clock_ctx_));
        const bool has_previous = snap.previous != nullptr;
        if (!roots_ || roots_->generation != snap.generation || roots_->has_previous != has_previous ||
            roots_->previous_generation != snap.previous_generation) {
            auto fresh = std::make_unique<Roots>();
            fresh->generation = snap.generation;
            fresh->previous_generation = snap.previous_generation;
            fresh->has_previous = has_previous;
            add_bundle(*fresh, snap.current.get(), true);
            add_bundle(*fresh, snap.previous.get(), false);
            roots_ = std::move(fresh);
        }
        for (Signer& s : roots_->signers)
            if (s.id == id && (s.current || !current_only)) return &s;
        return nullptr;
    }

    const TrustStore& trust_;
    ClockFn clock_;
    void* clock_ctx_;
    mutable std::mutex mu_;
    mutable std::unique_ptr<Roots> roots_;
};

}  // namespace quark::adapters
