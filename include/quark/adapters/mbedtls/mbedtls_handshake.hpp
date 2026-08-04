// Implements ADR-040 Phase 4 — `MbedtlsHandshakeEngine`/`Factory`, the real TLS 1.3 mutual handshake
// behind the `HandshakeEngine` seam (handshake.hpp). Bridges mbedTLS's blocking, socket-shaped SSL API
// to the seam's async step-machine via an internal byte-buffer BIO: advance() feeds `in` into the read
// buffer, drives `mbedtls_ssl_handshake()` until it blocks (WANT_READ/WANT_WRITE) or finishes, and
// returns whatever bytes mbedTLS queued to send. mbedTLS's own TLS record framing is self-describing,
// so concatenating multiple records into one Authenticate frame (or splitting across several) is
// transparent to it either way.
//
// IDENTITY BINDING: NodeId/ClusterId are bound from the peer's verified leaf certificate's Subject
// Common Name, formatted `"quark:<cluster_id>:<node_id>"` (decimal). A SAN URI is the more
// conventional production choice but requires version-sensitive ASN.1 walking; CN binding is exactly
// as authenticated (it's inside the signed certificate) and uses a stable, documented mbedTLS API
// (`mbedtls_x509_name` linked list) — swapping the encoding later does not change this seam's shape.
//
// DIRECTIONAL KEYS (C4 role-aware split): both sides export TWO independently-labeled RFC 8446 §7.5
// keying materials ("quark c2s", "quark s2c") — identical derivation on both ends, so client.send ==
// server.recv and vice versa, with NO client/server ambiguity in which physical secret is which.
//
// REVOCATION AT HANDSHAKE (S3): `mbedtls_ssl_conf_verify`'s callback computes the LEAF cert's SHA-256
// fingerprint and, if it is in the `revoked` set passed at construction, fails the handshake — this
// runs INSIDE mbedTLS's own chain verification, so a revoked-but-unexpired cert never completes a
// handshake, before any session exists (distinct from SecureTransport::sweep_revocations, which only
// covers ALREADY-open sessions from Phase 5's gossiped registry).
#pragma once

#include <array>
#include <charconv>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_aead.hpp"
#include "quark/core/handshake.hpp"

namespace quark::adapters {

namespace detail {

// One process-wide DRBG, mutex-guarded (mbedtls_ctr_drbg_context has no built-in thread safety unless
// MBEDTLS_THREADING_C is configured, which this adapter does not require). Handshakes are cold-path —
// this lock sees negligible contention.
class SharedRng {
public:
    static SharedRng& instance() {
        static SharedRng inst;
        return inst;
    }
    static int random(void* ctx, unsigned char* out, std::size_t len) {
        auto* self = static_cast<SharedRng*>(ctx);
        std::lock_guard<std::mutex> g(self->mu_);
        return mbedtls_ctr_drbg_random(&self->drbg_, out, len);
    }

private:
    SharedRng() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        static constexpr char kPers[] = "quark-adr040-mtls";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                              reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
    }
    ~SharedRng() {
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    SharedRng(const SharedRng&) = delete;
    SharedRng& operator=(const SharedRng&) = delete;

    std::mutex mu_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
};

[[nodiscard]] inline Fingerprint sha256_fingerprint(const mbedtls_x509_crt& crt) {
    Fingerprint fp{};
    mbedtls_sha256(crt.raw.p, crt.raw.len, reinterpret_cast<unsigned char*>(fp.data()), 0);
    return fp;
}

// CN format: "quark:<cluster_id>:<node_id>" (decimal). Walks the Subject DN's attribute list for the
// commonName OID — mbedtls_x509_name is a linked list of {oid, val} pairs (asn1.h).
[[nodiscard]] inline bool parse_cn_identity(const mbedtls_x509_crt& crt, NodeId& node, ClusterId& cluster) {
    for (const mbedtls_x509_name* n = &crt.subject; n != nullptr; n = n->next) {
        const std::size_t cn_oid_len = std::strlen(MBEDTLS_OID_AT_CN);
        if (n->oid.len != cn_oid_len || std::memcmp(n->oid.p, MBEDTLS_OID_AT_CN, cn_oid_len) != 0) continue;
        const std::string_view cn(reinterpret_cast<const char*>(n->val.p), n->val.len);
        constexpr std::string_view kPrefix = "quark:";
        if (cn.size() <= kPrefix.size() || cn.substr(0, kPrefix.size()) != kPrefix) return false;
        const std::string_view rest = cn.substr(kPrefix.size());
        const auto sep = rest.find(':');
        if (sep == std::string_view::npos) return false;
        std::uint64_t cluster_val = 0, node_val = 0;
        const auto r1 = std::from_chars(rest.data(), rest.data() + sep, cluster_val);
        if (r1.ec != std::errc{}) return false;
        const auto r2 = std::from_chars(rest.data() + sep + 1, rest.data() + rest.size(), node_val);
        if (r2.ec != std::errc{}) return false;
        cluster = ClusterId{cluster_val};
        node = NodeId{node_val};
        return true;
    }
    return false;
}

}  // namespace detail

class MbedtlsHandshakeEngine final : public HandshakeEngine {
public:
    MbedtlsHandshakeEngine(bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster_id,
                           IdentityMaterial::Snapshot own_identity, TrustStore::Snapshot trust,
                           std::shared_ptr<const std::unordered_set<Fingerprint>> revoked)
        : is_client_(is_client),
          self_(self),
          expected_peer_(expected_peer),
          cluster_id_(cluster_id),
          own_identity_(std::move(own_identity)),
          trust_(std::move(trust)),
          revoked_(std::move(revoked)) {
        mbedtls_x509_crt_init(&own_cert_);
        mbedtls_pk_init(&own_key_);
        mbedtls_x509_crt_init(&ca_chain_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        if (!setup()) setup_failed_ = true;
    }

    ~MbedtlsHandshakeEngine() override {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&own_cert_);
        mbedtls_pk_free(&own_key_);
        mbedtls_x509_crt_free(&ca_chain_);
    }
    MbedtlsHandshakeEngine(const MbedtlsHandshakeEngine&) = delete;
    MbedtlsHandshakeEngine& operator=(const MbedtlsHandshakeEngine&) = delete;

    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (setup_failed_) {
            failure_ = setup_failure_.empty() ? "mbedTLS handshake setup failed" : setup_failure_;
            return Step::Failed;
        }
        in_buf_.insert(in_buf_.end(), reinterpret_cast<const unsigned char*>(in.data()),
                       reinterpret_cast<const unsigned char*>(in.data()) + in.size());
        out_buf_.clear();
        const int rc = mbedtls_ssl_handshake(&ssl_);
        if (!out_buf_.empty())
            out.insert(out.end(), reinterpret_cast<const std::byte*>(out_buf_.data()),
                      reinterpret_cast<const std::byte*>(out_buf_.data()) + out_buf_.size());
        if (rc == 0) return finish() ? Step::Done : Step::Failed;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            return out.empty() ? Step::WantRead : Step::WantWrite;
        char err[160];
        mbedtls_strerror(rc, err, sizeof(err));
        failure_ = std::string("mbedtls_ssl_handshake: ") + err;
        return Step::Failed;
    }

    Result take_result() override { return std::move(result_); }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    [[nodiscard]] bool setup() {
        if (!own_identity_.current) {
            setup_failure_ = "no own TLS identity available";
            return false;
        }
        const auto& id = *own_identity_.current;
        if (mbedtls_x509_crt_parse_der(&own_cert_,
                                       reinterpret_cast<const unsigned char*>(id.cert_chain_der.data()),
                                       id.cert_chain_der.size()) != 0) {
            setup_failure_ = "failed to parse own certificate chain";
            return false;
        }
        if (mbedtls_pk_parse_key(&own_key_,
                                 reinterpret_cast<const unsigned char*>(id.private_key_der.data()),
                                 id.private_key_der.size(), nullptr, 0, &detail::SharedRng::random,
                                 &detail::SharedRng::instance()) != 0) {
            setup_failure_ = "failed to parse own private key";
            return false;
        }
        // S4: verify against the UNION of current + (still-in-window) previous trust roots.
        bool any_root = false;
        if (trust_.current && !trust_.current->ca_chain_der.empty()) {
            if (mbedtls_x509_crt_parse_der(
                    &ca_chain_, reinterpret_cast<const unsigned char*>(trust_.current->ca_chain_der.data()),
                    trust_.current->ca_chain_der.size()) != 0) {
                setup_failure_ = "failed to parse current trust root(s)";
                return false;
            }
            any_root = true;
        }
        if (trust_.previous && !trust_.previous->ca_chain_der.empty()) {
            if (mbedtls_x509_crt_parse_der(
                    &ca_chain_, reinterpret_cast<const unsigned char*>(trust_.previous->ca_chain_der.data()),
                    trust_.previous->ca_chain_der.size()) != 0) {
                setup_failure_ = "failed to parse previous (overlap-window) trust root(s)";
                return false;
            }
            any_root = true;
        }
        if (!any_root) {
            setup_failure_ = "no trust roots available (neither current nor previous)";
            return false;
        }

        if (mbedtls_ssl_config_defaults(&conf_, is_client_ ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            setup_failure_ = "mbedtls_ssl_config_defaults failed";
            return false;
        }
        mbedtls_ssl_conf_rng(&conf_, &detail::SharedRng::random, &detail::SharedRng::instance());
        // Mutual auth: both client and server REQUIRE and verify the peer's certificate (021's
        // AUTHENTICATE — a mismatched/unverifiable/revoked peer never reaches Data delivery).
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf_, &ca_chain_, nullptr);
        mbedtls_ssl_conf_verify(&conf_, &MbedtlsHandshakeEngine::verify_cb, this);
        if (mbedtls_ssl_conf_own_cert(&conf_, &own_cert_, &own_key_) != 0) {
            setup_failure_ = "mbedtls_ssl_conf_own_cert failed";
            return false;
        }
        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0) {
            setup_failure_ = "mbedtls_ssl_setup failed";
            return false;
        }
        // The client's peer authentication is NodeId/ClusterId bound from the leaf cert's CN
        // (finish()/parse_cn_identity), not a DNS hostname — deliberately opt out of mbedTLS's
        // hostname-vs-SAN check (passing NULL, not just omitting the call, records that this is an
        // informed choice rather than an oversight; see MBEDTLS_ERR_SSL_CERTIFICATE_VERIFICATION_
        // WITHOUT_HOSTNAME's doc comment). Only meaningful for the client role.
        if (is_client_) mbedtls_ssl_set_hostname(&ssl_, nullptr);
        mbedtls_ssl_set_bio(&ssl_, this, &MbedtlsHandshakeEngine::bio_send,
                            &MbedtlsHandshakeEngine::bio_recv, nullptr);
        return true;
    }

    // Runs INSIDE mbedTLS's own chain verification, once per certificate in the peer's chain
    // (including the trusted root); depth 0 is the leaf. Captures the leaf's fingerprint and rejects
    // it (S3) if it is in the revoked set — a nonzero `*flags` with VERIFY_REQUIRED makes the
    // enclosing mbedtls_ssl_handshake() call itself fail, so advance() never reaches Done for a
    // revoked peer.
    static int verify_cb(void* ctx, mbedtls_x509_crt* crt, int depth, std::uint32_t* flags) {
        auto* self = static_cast<MbedtlsHandshakeEngine*>(ctx);
        if (depth != 0) return 0;
        self->peer_fingerprint_ = detail::sha256_fingerprint(*crt);
        if (self->revoked_ && self->revoked_->contains(self->peer_fingerprint_)) {
            *flags |= MBEDTLS_X509_BADCERT_REVOKED;
        }
        return 0;
    }

    [[nodiscard]] bool finish() {
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&ssl_);
        if (!peer) {
            failure_ = "no peer certificate available after a completed handshake";
            return false;
        }
        NodeId claimed_node{};
        ClusterId claimed_cluster{};
        if (!detail::parse_cn_identity(*peer, claimed_node, claimed_cluster)) {
            failure_ = "peer certificate CN does not encode a valid quark identity";
            return false;
        }
        if (!(claimed_node == expected_peer_) || !(claimed_cluster == cluster_id_)) {
            failure_ = "peer certificate identity mismatch (NodeId/ClusterId)";
            return false;
        }

        std::array<unsigned char, MbedtlsAeadGcm::kKeyBytes> c2s{}, s2c{};
        static constexpr char kLabelC2S[] = "quark c2s";
        static constexpr char kLabelS2C[] = "quark s2c";
        if (mbedtls_ssl_export_keying_material(&ssl_, c2s.data(), c2s.size(), kLabelC2S,
                                               sizeof(kLabelC2S) - 1, nullptr, 0, 0) != 0 ||
            mbedtls_ssl_export_keying_material(&ssl_, s2c.data(), s2c.size(), kLabelS2C,
                                               sizeof(kLabelS2C) - 1, nullptr, 0, 0) != 0) {
            failure_ = "TLS keying material export failed";
            return false;
        }
        std::array<std::byte, MbedtlsAeadGcm::kKeyBytes> c2s_key{}, s2c_key{};
        std::memcpy(c2s_key.data(), c2s.data(), c2s.size());
        std::memcpy(s2c_key.data(), s2c.data(), s2c.size());
        auto c2s_cipher = std::make_shared<MbedtlsAeadGcm>(std::span<const std::byte, MbedtlsAeadGcm::kKeyBytes>(c2s_key));
        auto s2c_cipher = std::make_shared<MbedtlsAeadGcm>(std::span<const std::byte, MbedtlsAeadGcm::kKeyBytes>(s2c_key));

        result_.peer_node_id = claimed_node;
        result_.peer_cluster_id = claimed_cluster;
        result_.peer_fingerprint = peer_fingerprint_;
        if (is_client_) {
            result_.send_cipher = std::move(c2s_cipher);
            result_.recv_cipher = std::move(s2c_cipher);
        } else {
            result_.send_cipher = std::move(s2c_cipher);
            result_.recv_cipher = std::move(c2s_cipher);
        }
        return true;
    }

    static int bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
        auto* self = static_cast<MbedtlsHandshakeEngine*>(ctx);
        self->out_buf_.insert(self->out_buf_.end(), buf, buf + len);
        return static_cast<int>(len);
    }
    static int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
        auto* self = static_cast<MbedtlsHandshakeEngine*>(ctx);
        if (self->in_buf_.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        const std::size_t n = len < self->in_buf_.size() ? len : self->in_buf_.size();
        std::memcpy(buf, self->in_buf_.data(), n);
        self->in_buf_.erase(self->in_buf_.begin(), self->in_buf_.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    bool is_client_;
    NodeId self_, expected_peer_;
    ClusterId cluster_id_;
    IdentityMaterial::Snapshot own_identity_;
    TrustStore::Snapshot trust_;
    std::shared_ptr<const std::unordered_set<Fingerprint>> revoked_;

    mbedtls_x509_crt own_cert_;
    mbedtls_pk_context own_key_;
    mbedtls_x509_crt ca_chain_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;

    bool setup_failed_ = false;
    std::string setup_failure_;
    std::string failure_;
    Fingerprint peer_fingerprint_{};
    Result result_{};

    std::vector<unsigned char> in_buf_;
    std::vector<unsigned char> out_buf_;
};

class MbedtlsHandshakeEngineFactory final : public HandshakeEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<HandshakeEngine> create(
        bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster_id,
        IdentityMaterial::Snapshot own_identity, TrustStore::Snapshot trust,
        std::shared_ptr<const std::unordered_set<Fingerprint>> revoked) override {
        return std::make_unique<MbedtlsHandshakeEngine>(is_client, self, expected_peer, cluster_id,
                                                        std::move(own_identity), std::move(trust),
                                                        std::move(revoked));
    }
};

}  // namespace quark::adapters
