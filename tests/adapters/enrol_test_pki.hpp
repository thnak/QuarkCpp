// Shared test PKI and anchor fixture for the enrolment checks (ADR-074 D4c): a generated CA, leaves, a simulated custodian (the application's only
// signer), a `World` = anchor identity + trust store + real `Enrollment` over `MbedtlsEnrolCrypto` (or an approve-everything crypto for the joiner-side
// defence tests). Header-only, tests only; every function lives in an anonymous namespace of the including check.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_enrol_session.hpp"
#include "quark/core/enrol_wire.hpp"

namespace enrol_test {
using namespace quark;
using namespace quark::adapters;
using quark::adapters::detail::Pk;
using quark::adapters::detail::SharedRng;

using Bytes = std::vector<std::byte>;

// --- Test PKI (as mbedtls_enrollment_check.cpp) ----------------------------------------------------------------

void gen_key(mbedtls_pk_context& pk) {
    mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), &SharedRng::random, &SharedRng::instance());
}
Bytes key_der_of(mbedtls_pk_context& pk) {
    std::vector<unsigned char> buf(1024);
    const int len = mbedtls_pk_write_key_der(&pk, buf.data(), buf.size());
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
    return Bytes(tail, tail + len);
}
std::string x509_time(std::int64_t unix_s) {
    const std::int64_t days = (unix_s >= 0 ? unix_s : unix_s - 86399) / 86400;
    const std::int64_t secs = unix_s - days * 86400;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld%02lld%02lld%02lld%02lld%02lld", static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d),
                  static_cast<long long>(secs / 3600), static_cast<long long>((secs / 60) % 60), static_cast<long long>(secs % 60));
    return buf;
}

struct CertSpec {
    std::string subject;
    int is_ca = 0;
    unsigned int key_usage = 0;     // 0: no keyUsage extension
    const char* eku_oid = nullptr;  // nullptr: no extendedKeyUsage extension (a single OID otherwise)
    std::size_t eku_len = 0;
    std::int64_t not_before = 0, not_after = 0;
};

Bytes write_cert(mbedtls_pk_context& subject_key, mbedtls_pk_context& issuer_key, const std::string& issuer_dn, const CertSpec& spec) {
    static unsigned char serial = 1;
    ++serial;
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, &issuer_key);
    mbedtls_x509write_crt_set_subject_name(&w, spec.subject.c_str());
    mbedtls_x509write_crt_set_issuer_name(&w, issuer_dn.c_str());
    unsigned char sn[1] = {serial};
    mbedtls_x509write_crt_set_serial_raw(&w, sn, 1);
    mbedtls_x509write_crt_set_validity(&w, x509_time(spec.not_before).c_str(), x509_time(spec.not_after).c_str());
    mbedtls_x509write_crt_set_basic_constraints(&w, spec.is_ca, -1);
    if (spec.key_usage != 0) mbedtls_x509write_crt_set_key_usage(&w, spec.key_usage);
    mbedtls_asn1_sequence eku{};
    if (spec.eku_oid != nullptr) {
        eku.buf.tag = MBEDTLS_ASN1_OID;
        eku.buf.len = spec.eku_len;
        eku.buf.p = reinterpret_cast<unsigned char*>(const_cast<char*>(spec.eku_oid));
        eku.next = nullptr;
        mbedtls_x509write_crt_set_ext_key_usage(&w, &eku);
    }
    std::vector<unsigned char> buf(4096);
    const int len = mbedtls_x509write_crt_der(&w, buf.data(), buf.size(), &SharedRng::random, &SharedRng::instance());
    mbedtls_x509write_crt_free(&w);
    if (len <= 0) return {};
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
    return Bytes(tail, tail + len);
}

const std::int64_t kNow = static_cast<std::int64_t>(enrol::Enrollment::system_unix_now());
constexpr std::int64_t kDay = 86400;

struct Ca {
    Pk key;
    std::string dn;
    Bytes cert;
    explicit Ca(const char* cn) : dn(std::string("CN=") + cn) {
        gen_key(key.v);
        cert = write_cert(key.v, key.v, dn, {dn, 1, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN, nullptr, 0, kNow - kDay, kNow + 3650 * kDay});
    }
};

// A leaf with its own key; returns {chain DER (leaf only), private key DER}.
struct Leaf {
    Bytes cert;
    Bytes key_der;
};
Leaf make_leaf(Ca& ca, const CertSpec& spec) {
    Pk key;
    gen_key(key.v);
    return {write_cert(key.v, ca.key.v, ca.dn, spec), key_der_of(key.v)};
}

Bytes concat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

enrol::wire::RootHash sha256_of(const Bytes& der) {
    enrol::wire::RootHash h{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(der.data()), der.size(), reinterpret_cast<unsigned char*>(h.data()), 0);
    return h;
}

constexpr std::uint64_t kCluster = 0x0A0B0C0D0E0F1011ull;
constexpr std::uint64_t kAnchor = 0x1234567890ABCDEull;  // bit 63 clear: a member id
std::string cn_for(std::uint64_t cluster, std::uint64_t node) { return format_quark_cn(ClusterId{cluster}, NodeId{node}); }

CertSpec good_spec(const std::string& cn) {
    return {"CN=" + cn, 0, MBEDTLS_X509_KU_DIGITAL_SIGNATURE, nullptr, 0, kNow - 60, kNow + 30 * kDay};
}


// --- The custodian (the application's only signer), simulated -----------------------------------------------------

Bytes issue_for(Ca& ca, std::span<const std::byte> spki, const CertSpec& spec) {
    Pk subject;
    if (mbedtls_pk_parse_public_key(&subject.v, reinterpret_cast<const unsigned char*>(spki.data()), spki.size()) != 0) return {};
    return write_cert(subject.v, ca.key.v, ca.dn, spec);
}

constexpr std::uint64_t kNode = 0x55ull;  // the member id the custodian hands out

// An EnrolCrypto that approves every issuance: the anchor of S4 that "trusts its custodian", so the joiner's own checks are the only defence.
struct LenientCrypto final : enrol::EnrolCrypto {
    explicit LenientCrypto(std::shared_ptr<TrustStore> t) : real(std::move(t)) {}
    bool hmac_equal(std::span<const std::byte> s, std::span<const std::byte> m, std::span<const std::byte> t) const override { return real.hmac_equal(s, m, t); }
    bool csr_public_key(std::span<const std::byte> c, std::vector<std::byte>& k) const override { return real.csr_public_key(c, k); }
    bool issued_ok(const enrol::IssuedExpectation&, std::span<const std::byte>) const override { return true; }
    MbedtlsEnrolCrypto real;
};

// Everything one test needs: an anchor identity under a root, the trust store the Enrollment verifies against, and the join code material.
struct World {
    enum class Fault { None, OtherKey, OtherCluster, CaLeaf, Rogue };

    Ca root{"quark-test-root"};
    Ca rogue{"quark-rogue-root"};
    Leaf anchor_leaf;
    TlsIdentity anchor_id;
    enrol::wire::RootHash pin;
    std::shared_ptr<TrustStore> trust;
    Fault fault = Fault::None;
    std::function<void(enrol::IssueRequest, enrol::Enrollment::Completion)> custodian_override;
    std::shared_ptr<const enrol::EnrolCrypto> crypto;
    std::unique_ptr<enrol::Enrollment> enr;
    std::atomic<std::uint64_t> clock{1'000};

    explicit World(bool lenient_crypto = false) {
        anchor_leaf = make_leaf(root, good_spec(cn_for(kCluster, kAnchor)));
        anchor_id.cert_chain_der = concat(anchor_leaf.cert, root.cert);
        anchor_id.private_key_der = anchor_leaf.key_der;
        pin = sha256_of(root.cert);
        trust = std::make_shared<TrustStore>(std::make_shared<const TrustedRoots>(TrustedRoots{root.cert}));
        if (lenient_crypto)
            crypto = std::make_shared<LenientCrypto>(trust);
        else
            crypto = std::make_shared<MbedtlsEnrolCrypto>(trust);
        enrol::Config cfg;
        cfg.cluster = ClusterId{kCluster};
        enr = std::make_unique<enrol::Enrollment>(cfg, crypto, [this](enrol::IssueRequest req, enrol::Enrollment::Completion done) {
            if (custodian_override) return custodian_override(std::move(req), std::move(done));
            done(sign(req));
        });
    }

    // What a (possibly faulty) custodian returns for `req`.
    enrol::Issued sign(const enrol::IssueRequest& req) {
        const NodeId node{req.role == enrol::Role::Client ? (kClientNodeBit | kNode) : kNode};
        std::uint64_t cluster = req.cluster.value;
        CertSpec spec = good_spec(cn_for(cluster, node.value));
        std::vector<std::byte> spki = req.spki_der;
        if (fault == Fault::OtherCluster) spec = good_spec(cn_for(cluster + 1, node.value));
        if (fault == Fault::CaLeaf) spec.is_ca = 1;
        if (fault == Fault::OtherKey) {
            Pk other;
            gen_key(other.v);
            std::array<unsigned char, 1024> buf{};
            const int len = mbedtls_pk_write_pubkey_der(&other.v, buf.data(), buf.size());
            const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
            spki.assign(tail, tail + len);
        }
        Ca& signer = fault == Fault::Rogue ? rogue : root;
        return enrol::Issued{node, issue_for(signer, spki, spec)};
    }

    enrol::wire::JoinCode code(const enrol::Ticket& t) const {
        enrol::wire::JoinCode c;
        c.anchor_addr = 0x7F000001u;
        c.anchor_port = 4242;
        c.anchor_node = NodeId{kAnchor};
        c.cluster = ClusterId{kCluster};
        c.ca_root_sha256 = pin;
        c.ticket_id = t.id;
        c.ticket_secret = t.secret;
        return c;
    }

    std::unique_ptr<EnrolAnchorSession> anchor(std::function<void()> on_ready = {}) {
        return std::make_unique<EnrolAnchorSession>(anchor_id, *enr, "198.51.100.0/24", [this] { return clock.load(); }, std::move(on_ready));
    }
};

}  // namespace enrol_test
