// Implements ADR-074 D4 (issue #36, D4c step 2): the SERVER-AUTHENTICATED channel an enrolment travels in. The joiner has no certificate yet, so this is
// TLS 1.3 with a certificate on the anchor only (the client is not asked for one), followed by TLS application data carrying the length-prefixed messages of
// enrol_wire.hpp. It is a byte-stream state machine like `MbedtlsHandshakeEngine` (mbedtls_handshake.hpp): the caller feeds inbound bytes and sends what comes out;
// no socket, no thread, no clock of its own. Which socket carries it, and the caps around it, are step 4.
//
// WHAT THE JOINER PROVES BEFORE IT SENDS ANYTHING (the join code is the only thing it trusts; ADR-074 D4 "Wire"):
//   1. the anchor's chain contains a CA certificate whose SHA-256 equals the join code's `ca_root_sha256` (the pin), and that certificate is a CA and is not
//      the leaf: the pinned certificate is the ONLY trust anchor, whatever else the anchor sends;
//   2. the leaf chains to the pin by signature, with a valid period, as `mbedtls_x509_crt_verify` judges it;
//   3. the leaf is a MEMBER certificate (`quark:`, never `quark-client:`) for exactly the join code's cluster id and the anchor's NodeId, so a client certificate or
//      any other member cannot impersonate the anchor;
//   4. the leaf is not CA-capable, its key usage (if present) allows digitalSignature and its extended key usage (if present) allows serverAuth.
// Only then does `Step::Done` come back, and `seal()` refuses to produce anything before that. (TLS 1.3's CertificateVerify, which mbedTLS checks with the
// certificate's key whatever the authmode, proves the anchor holds the leaf's private key; the certificate checks above are done by this class because
// the pin is a hash, not a certificate mbedTLS could be handed up front.)
//
// SERVER SIDE: the anchor's own identity (chain + key, like any member's) and no client certificate. Refuses a CA-capable own certificate (ADR-074 C5). Minimum
// protocol version is TLS 1.3 on both sides. Handshake input is capped (64 KiB, as the member engine); application input is capped by the caller's
// `wire::MessageReader`.
//
// The joiner's chain pin uses `mbedtls_x509_crt_verify` against a trust list holding ONLY the pinned certificate, so the verification time comes from the
// joiner's wall clock (mbedTLS): a joiner with a badly wrong clock is refused, as for any TLS client.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_enrollment.hpp"  // detail::Crt, detail::Pk
#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"   // detail::SharedRng, parse_cn_identity
#include "quark/adapters/mbedtls/mbedtls_x509_util.hpp"
#include "quark/core/enrol_wire.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark::adapters {

class EnrolChannel {
public:
    enum class Step { WantWrite, WantRead, Done, Failed };
    enum class ReadResult { Ok, Closed, Failed };

    virtual ~EnrolChannel() {
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
    }
    EnrolChannel(const EnrolChannel&) = delete;
    EnrolChannel& operator=(const EnrolChannel&) = delete;

    // Handshake. `in` is the next inbound chunk (empty on the joiner's first call, which produces the ClientHello); bytes to send are APPENDED to `out`.
    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) {
        if (done_) return Step::Done;
        if (failed_) return Step::Failed;
        if (!setup_ok_) return fail("setup failed: " + setup_failure_);
        if (in_buf_.size() + in.size() > kMaxBufferedHandshakeBytes) return fail("handshake input exceeds the buffered-bytes cap");
        feed(in);
        out_buf_.clear();
        const std::size_t before = out.size();
        const int rc = mbedtls_ssl_handshake(&ssl_);
        drain(out);
        if (rc == 0) {
            if (!verify_peer()) {
                out.resize(before);  // the joiner's last flight (its Finished) must not leave for an anchor it has just refused
                return Step::Failed;  // failure_ set by verify_peer
            }
            done_ = true;
            return Step::Done;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return out.empty() ? Step::WantRead : Step::WantWrite;
        return fail(std::string("mbedtls_ssl_handshake: ") + strerror_of(rc));
    }

    // After Done only: encrypt `plain` (one or more whole messages, already length-prefixed by the caller) and APPEND the TLS records to `out`.
    [[nodiscard]] bool seal(std::span<const std::byte> plain, std::vector<std::byte>& out) {
        if (!done_ || failed_) return false;
        out_buf_.clear();
        std::size_t off = 0;
        while (off < plain.size()) {
            const int n = mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char*>(plain.data()) + off, plain.size() - off);
            if (n > 0) {
                off += static_cast<std::size_t>(n);
            } else if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
                continue;  // the BIO never blocks on send; WANT_READ here means a post-handshake record is pending, retried
            } else {
                failed_ = true;
                failure_ = std::string("mbedtls_ssl_write: ") + strerror_of(n);
                return false;
            }
        }
        drain(out);
        return true;
    }

    // After Done only: feed inbound bytes, APPEND the decrypted application data to `plain`, and any bytes TLS itself must send (an alert) to `out`.
    [[nodiscard]] ReadResult open(std::span<const std::byte> in, std::vector<std::byte>& plain, std::vector<std::byte>& out) {
        if (!done_ || failed_) return ReadResult::Failed;
        if (in_buf_.size() + in.size() > kMaxBufferedAppBytes) {
            failed_ = true;
            failure_ = "application input exceeds the buffered-bytes cap";
            return ReadResult::Failed;
        }
        feed(in);
        out_buf_.clear();
        std::array<unsigned char, 4096> tmp{};
        ReadResult result = ReadResult::Ok;
        for (;;) {
            const int n = mbedtls_ssl_read(&ssl_, tmp.data(), tmp.size());
            if (n > 0) {
                plain.insert(plain.end(), reinterpret_cast<const std::byte*>(tmp.data()), reinterpret_cast<const std::byte*>(tmp.data()) + n);
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) break;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                result = ReadResult::Closed;
                break;
            }
            failed_ = true;
            failure_ = std::string("mbedtls_ssl_read: ") + strerror_of(n);
            result = ReadResult::Failed;
            break;
        }
        drain(out);
        return result;
    }

    // Politely close (close_notify), APPENDING the alert record to `out`.
    void close(std::vector<std::byte>& out) {
        if (!setup_ok_) return;
        out_buf_.clear();
        (void)mbedtls_ssl_close_notify(&ssl_);
        drain(out);
    }

    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] std::string_view failure_reason() const noexcept { return failure_; }

protected:
    EnrolChannel() {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
    }

    // Derived classes fill the config (server or client defaults, auth mode, certificates), then call finish_setup().
    [[nodiscard]] bool base_config(int endpoint) {
        if (mbedtls_ssl_config_defaults(&conf_, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            setup_failure_ = "mbedtls_ssl_config_defaults failed";
            return false;
        }
        mbedtls_ssl_conf_rng(&conf_, &detail::SharedRng::random, &detail::SharedRng::instance());
        mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_3);
        return true;
    }
    [[nodiscard]] bool finish_setup() {
        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0) {
            setup_failure_ = "mbedtls_ssl_setup failed";
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, this, &EnrolChannel::bio_send, &EnrolChannel::bio_recv, nullptr);
        setup_ok_ = true;
        return true;
    }
    virtual bool verify_peer() { return true; }

    Step fail(std::string why) {
        failed_ = true;
        failure_ = std::move(why);
        return Step::Failed;
    }

    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    bool setup_ok_ = false;
    std::string setup_failure_;
    std::string failure_;
    bool failed_ = false;

private:
    static constexpr std::size_t kMaxBufferedHandshakeBytes = 64 * 1024;
    static constexpr std::size_t kMaxBufferedAppBytes = 2 * (4 + enrol::wire::kMaxMessageBytes) + 4096;

    void feed(std::span<const std::byte> in) {
        in_buf_.insert(in_buf_.end(), reinterpret_cast<const unsigned char*>(in.data()), reinterpret_cast<const unsigned char*>(in.data()) + in.size());
    }
    void drain(std::vector<std::byte>& out) {
        out.insert(out.end(), reinterpret_cast<const std::byte*>(out_buf_.data()), reinterpret_cast<const std::byte*>(out_buf_.data()) + out_buf_.size());
        out_buf_.clear();
    }
    static std::string strerror_of(int rc) {
        char err[160];
        mbedtls_strerror(rc, err, sizeof(err));
        return err;
    }
    static int bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
        auto* self = static_cast<EnrolChannel*>(ctx);
        self->out_buf_.insert(self->out_buf_.end(), buf, buf + len);
        return static_cast<int>(len);
    }
    static int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
        auto* self = static_cast<EnrolChannel*>(ctx);
        if (self->in_buf_.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        const std::size_t n = len < self->in_buf_.size() ? len : self->in_buf_.size();
        std::memcpy(buf, self->in_buf_.data(), n);
        self->in_buf_.erase(self->in_buf_.begin(), self->in_buf_.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    bool done_ = false;
    std::vector<unsigned char> in_buf_;
    std::vector<unsigned char> out_buf_;
};

// The anchor's end: a member certificate and key, no client certificate asked for.
class EnrolServerChannel final : public EnrolChannel {
public:
    explicit EnrolServerChannel(const TlsIdentity& own) {
        if (own.cert_chain_der.empty()) {
            setup_failure_ = "no own certificate chain";
            return;
        }
        if (!detail::parse_der_concat(cert_.v, own.cert_chain_der)) {
            setup_failure_ = "failed to parse own certificate chain";
            return;
        }
        if (mbedtls_pk_parse_key(&key_.v, reinterpret_cast<const unsigned char*>(own.private_key_der.data()), own.private_key_der.size(), nullptr, 0,
                                 &detail::SharedRng::random, &detail::SharedRng::instance()) != 0) {
            setup_failure_ = "failed to parse own private key";
            return;
        }
        if (detail::ca_capable(cert_.v)) {  // ADR-074 C5: a node never runs with CA key material
            setup_failure_ = "own certificate is CA-capable (ADR-074 C5)";
            return;
        }
        if (!base_config(MBEDTLS_SSL_IS_SERVER)) return;
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        if (mbedtls_ssl_conf_own_cert(&conf_, &cert_.v, &key_.v) != 0) {
            setup_failure_ = "mbedtls_ssl_conf_own_cert failed";
            return;
        }
        (void)finish_setup();
    }

private:
    detail::Crt cert_;
    detail::Pk key_;
};

// What the join code tells the joiner about the anchor.
struct AnchorPin {
    NodeId anchor_node{};
    ClusterId cluster{};
    enrol::wire::RootHash ca_sha256{};
};

// The joiner's end: no certificate of its own, and the anchor is accepted only as described in the banner.
class EnrolJoinerChannel final : public EnrolChannel {
public:
    explicit EnrolJoinerChannel(const AnchorPin& pin) : pin_(pin) {
        if (!base_config(MBEDTLS_SSL_IS_CLIENT)) return;
        // The chain is judged in verify_peer() against the pin (a hash), so mbedTLS's own verification is off; TLS 1.3's CertificateVerify still runs.
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        if (!finish_setup()) return;
        mbedtls_ssl_set_hostname(&ssl_, nullptr);
    }

    // After Done only: the pinned CA certificate the anchor's chain was verified against (DER). The joiner checks what the anchor ISSUES against it.
    [[nodiscard]] const std::vector<std::byte>& pinned_ca_der() const noexcept { return ca_der_; }

private:
    bool verify_peer() override {
        const mbedtls_x509_crt* leaf = mbedtls_ssl_get_peer_cert(&ssl_);
        if (leaf == nullptr) return refuse("no anchor certificate");
        // 1. the pin: a CA certificate other than the leaf whose SHA-256 is the join code's.
        const mbedtls_x509_crt* ca = nullptr;
        for (const mbedtls_x509_crt* c = leaf->next; c != nullptr; c = c->next) {
            std::array<unsigned char, 32> h{};
            mbedtls_sha256(c->raw.p, c->raw.len, h.data(), 0);
            if (std::memcmp(h.data(), pin_.ca_sha256.data(), h.size()) == 0) {
                ca = c;
                break;
            }
        }
        if (ca == nullptr) return refuse("the anchor's chain does not contain the pinned CA certificate");
        if (mbedtls_x509_crt_get_ca_istrue(ca) != 1) return refuse("the pinned certificate is not a CA");
        // 2. chain by signature to the pin alone.
        detail::Crt trust;
        if (mbedtls_x509_crt_parse_der(&trust.v, ca->raw.p, ca->raw.len) != 0) return refuse("cannot parse the pinned certificate");
        std::uint32_t flags = 0;
        if (mbedtls_x509_crt_verify(const_cast<mbedtls_x509_crt*>(leaf), &trust.v, nullptr, nullptr, &flags, nullptr, nullptr) != 0 || flags != 0)
            return refuse("the anchor's certificate does not chain to the pinned CA");
        // 3. member certificate for exactly this cluster and anchor.
        NodeId node{};
        ClusterId cluster{};
        if (!detail::parse_cn_identity(*leaf, node, cluster)) return refuse("the anchor's certificate CN is not a quark identity");
        if (is_client_node(node)) return refuse("the anchor's certificate is a client certificate");
        if (!(node == pin_.anchor_node) || !(cluster == pin_.cluster)) return refuse("the anchor's certificate is for another node or cluster");
        // 4. an end-entity certificate usable for TLS service.
        if (detail::ca_capable(*leaf)) return refuse("the anchor's certificate is CA-capable");
        if (mbedtls_x509_crt_has_ext_type(leaf, MBEDTLS_X509_EXT_KEY_USAGE) &&
            mbedtls_x509_crt_check_key_usage(leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0)
            return refuse("the anchor's certificate key usage lacks digitalSignature");
        if (mbedtls_x509_crt_has_ext_type(leaf, MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) &&
            mbedtls_x509_crt_check_extended_key_usage(leaf, MBEDTLS_OID_SERVER_AUTH, MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0)
            return refuse("the anchor's certificate extended key usage lacks serverAuth");
        ca_der_.assign(reinterpret_cast<const std::byte*>(ca->raw.p), reinterpret_cast<const std::byte*>(ca->raw.p) + ca->raw.len);
        return true;
    }
    bool refuse(const char* why) {
        failed_ = true;
        failure_ = why;
        return false;
    }

    AnchorPin pin_;
    std::vector<std::byte> ca_der_;
};

}  // namespace quark::adapters
