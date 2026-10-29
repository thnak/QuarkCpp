// Conformance check for ADR-067 D4 (and C5) over REAL crypto: `MbedtlsEnrolCrypto` (enrollment.hpp's
// EnrolCrypto seam), the joiner's key/CSR/MAC helpers and `validate_identity_not_root`
// (mbedtls_enrollment.hpp). A test PKI is generated on the fly; the application's custodian — the only
// signer, which Quark never is (ADR-067 D1) — is simulated HERE, in the test, from the IssueRequest alone
// (the SPKI, never the CSR). Proves: HMAC compare; CSR proof-of-possession (tampered, truncated, trailing,
// non-P-256, non-SHA-256 refused); every issued-certificate rule (CN exact and single, SPKI, not
// CA-capable, validity bounds, chains only to a CONFIGURED root by signature not name, intermediates
// allowed but checked, overlap-window root); end-to-end Enrollment with real crypto; concurrent use;
// and C5 (root keys, current and previous, and CA-capable identities) at startup and in the handshake.
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include "quark/adapters/mbedtls/mbedtls_enrollment.hpp"
#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "quark/core/enrollment.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

using namespace quark;
using namespace quark::adapters;

namespace {

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

using Bytes = std::vector<std::byte>;
using quark::adapters::detail::Pk;
using quark::adapters::detail::SharedRng;

// --- Test PKI -------------------------------------------------------------------------------------

void gen_key(mbedtls_pk_context& pk, mbedtls_ecp_group_id curve = MBEDTLS_ECP_DP_SECP256R1) {
    mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_gen_key(curve, mbedtls_pk_ec(pk), &SharedRng::random, &SharedRng::instance());
}

Bytes key_der_of(mbedtls_pk_context& pk) {
    std::vector<unsigned char> buf(1024);
    const int len = mbedtls_pk_write_key_der(&pk, buf.data(), buf.size());
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
    return Bytes(tail, tail + len);
}

Bytes spki_der_of(const mbedtls_pk_context& pk) {
    quark::adapters::detail::Spki s;
    if (!quark::adapters::detail::spki_of(pk, s)) return {};
    const auto b = s.bytes();
    return Bytes(reinterpret_cast<const std::byte*>(b.data()), reinterpret_cast<const std::byte*>(b.data()) + b.size());
}

// "YYYYMMDDhhmmss" (UTC) for mbedTLS's writer; civil_from_days after H. Hinnant.
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
    std::snprintf(buf, sizeof(buf), "%04lld%02lld%02lld%02lld%02lld%02lld", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs / 60) % 60), static_cast<long long>(secs % 60));
    return buf;
}

struct CertSpec {
    std::string subject;
    int is_ca = 0;
    unsigned int key_usage = 0;  // 0: no keyUsage extension
    std::int64_t not_before = 0, not_after = 0;
};

Bytes write_cert(mbedtls_pk_context& subject_key, mbedtls_pk_context& issuer_key, const std::string& issuer_dn,
                 const CertSpec& spec) {
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
    std::vector<unsigned char> buf(4096);
    const int len = mbedtls_x509write_crt_der(&w, buf.data(), buf.size(), &SharedRng::random, &SharedRng::instance());
    mbedtls_x509write_crt_free(&w);
    if (len <= 0) return {};
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
    return Bytes(tail, tail + len);
}

const std::int64_t kNow = static_cast<std::int64_t>(enrol::Enrollment::system_unix_now());
constexpr std::int64_t kDay = 86400;

// A self-signed root. Owns its key — in the test only, standing in for the application's custodian.
struct Ca {
    Pk key;
    std::string dn;
    Bytes cert;
    explicit Ca(const char* cn) : dn(std::string("CN=") + cn) {
        gen_key(key.v);
        cert = write_cert(key.v, key.v, dn,
                          {dn, 1, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN, kNow - kDay, kNow + 3650 * kDay});
    }
    // The custodian's job: a certificate for a public key it was given as an SPKI.
    Bytes issue(std::span<const std::byte> spki, const CertSpec& spec) {
        Pk subject;
        if (mbedtls_pk_parse_public_key(&subject.v, reinterpret_cast<const unsigned char*>(spki.data()), spki.size()) != 0)
            return {};
        return write_cert(subject.v, key.v, dn, spec);
    }
};

CertSpec leaf_spec(const std::string& cn) {
    return {"CN=" + cn, 0, MBEDTLS_X509_KU_DIGITAL_SIGNATURE, kNow - 60, kNow + 30 * kDay};
}

Bytes concat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// --- Sections ------------------------------------------------------------------------------------

Bytes csr_signed_with(mbedtls_pk_context& key, mbedtls_md_type_t md) {
    mbedtls_x509write_csr w;
    mbedtls_x509write_csr_init(&w);
    mbedtls_x509write_csr_set_key(&w, &key);
    mbedtls_x509write_csr_set_md_alg(&w, md);
    mbedtls_x509write_csr_set_subject_name(&w, "CN=x");
    std::vector<unsigned char> buf(4096);
    const int len = mbedtls_x509write_csr_der(&w, buf.data(), buf.size(), &SharedRng::random, &SharedRng::instance());
    mbedtls_x509write_csr_free(&w);
    if (len <= 0) return {};
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - len);
    return Bytes(tail, tail + len);
}

void hmac_section(const MbedtlsEnrolCrypto& crypto) {
    enrol::TicketSecret secret{};
    enrol::TicketId id{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<std::byte>(i * 7 + 1);
    id[0] = std::byte{0x42};
    const Bytes csr{std::byte{1}, std::byte{2}, std::byte{3}};
    const ClusterId cluster{7};
    const auto tag = compute_enrol_mac(secret, cluster, id, csr);
    check(tag.has_value(), "the joiner computes a MAC");
    if (!tag) return;
    const auto msg = enrol::enrol_mac_message(cluster, id, csr);
    check(crypto.hmac_equal(secret, msg, *tag), "hmac_equal accepts the joiner's tag");
    enrol::MacTag flipped = *tag;
    flipped[31] ^= std::byte{1};
    check(!crypto.hmac_equal(secret, msg, flipped), "hmac_equal refuses a one-bit change");
    check(!crypto.hmac_equal(secret, msg, std::span<const std::byte>(*tag).first(31)), "hmac_equal refuses a short tag");
    enrol::TicketSecret other = secret;
    other[0] ^= std::byte{1};
    check(!crypto.hmac_equal(other, msg, *tag), "hmac_equal refuses another secret");
    check(!crypto.hmac_equal(secret, enrol::enrol_mac_message(ClusterId{8}, id, csr), *tag),
          "a tag for one cluster does not verify for another");
}

void csr_section(const MbedtlsEnrolCrypto& crypto) {
    const auto key = generate_identity_key();
    check(key.has_value(), "the joiner generates a P-256 key");
    if (!key) return;
    const auto csr = make_enrol_csr(*key);
    check(csr.has_value(), "the joiner builds a CSR");
    if (!csr) return;
    Bytes spki;
    check(crypto.csr_public_key(*csr, spki), "a genuine CSR passes proof of possession");
    Pk parsed;
    check(quark::adapters::detail::parse_private_key(parsed.v, *key) && spki == spki_der_of(parsed.v),
          "the SPKI returned is the joiner's public key");

    // Tamper inside the signed CertificationRequestInfo (the placeholder subject's text).
    Bytes tampered = *csr;
    const std::string marker = "quark-enrol";
    bool found = false;
    for (std::size_t i = 0; i + marker.size() <= tampered.size() && !found; ++i)
        if (std::equal(marker.begin(), marker.end(), reinterpret_cast<const char*>(tampered.data() + i))) {
            tampered[i] = std::byte{'Q'};
            found = true;
        }
    Bytes out;
    check(found && !crypto.csr_public_key(tampered, out), "a CSR whose signed body was altered is refused");
    check(!crypto.csr_public_key(std::span<const std::byte>(*csr).first(csr->size() - 1), out), "a truncated CSR is refused");
    check(!crypto.csr_public_key(concat(*csr, Bytes{std::byte{0}}), out), "a CSR with trailing bytes is refused");
    check(!crypto.csr_public_key({}, out), "an empty CSR is refused");
    check(!crypto.csr_public_key(Bytes(5000, std::byte{0x30}), out), "an oversized CSR is refused");

    // A P-384 key: well-formed and correctly self-signed, but not the cluster's curve.
    Pk p384;
    gen_key(p384.v, MBEDTLS_ECP_DP_SECP384R1);
    check(!make_enrol_csr(key_der_of(p384.v)), "the joiner helper refuses a non-P-256 key");
    const Bytes p384_csr = csr_signed_with(p384.v, MBEDTLS_MD_SHA256);
    check(!p384_csr.empty() && !crypto.csr_public_key(p384_csr, out), "a P-384 CSR is refused");
    // A P-256 key, correctly self-signed, but with ecdsa-with-SHA384: only ecdsa-with-SHA256 is accepted.
    const Bytes sha384_csr = csr_signed_with(parsed.v, MBEDTLS_MD_SHA384);
    check(!sha384_csr.empty() && !crypto.csr_public_key(sha384_csr, out), "a CSR signed with SHA-384 is refused");
}

void issued_section(Ca& ca, Ca& rogue, const std::shared_ptr<const TrustStore>& trust) {
    const MbedtlsEnrolCrypto crypto(trust);
    Pk joiner;
    gen_key(joiner.v);
    const Bytes spki = spki_der_of(joiner.v);
    const std::string cn = format_quark_cn(ClusterId{7}, NodeId{5});
    const enrol::IssuedExpectation expect{cn, spki, static_cast<std::uint64_t>(kNow), 365 * kDay};
    auto ok = [&](const Bytes& chain) { return crypto.issued_ok(expect, chain); };

    check(ok(ca.issue(spki, leaf_spec(cn))), "control: a correct certificate is accepted");
    CertSpec no_ku = leaf_spec(cn);
    no_ku.key_usage = 0;
    check(ok(ca.issue(spki, no_ku)), "control: a leaf without keyUsage is accepted");

    check(!ok(ca.issue(spki, leaf_spec(format_quark_cn(ClusterId{7}, NodeId{6})))), "another NodeId's CN is refused");
    check(!ok(ca.issue(spki, leaf_spec(format_quark_cn(ClusterId{8}, NodeId{5})))), "another cluster's CN is refused");
    {
        // mbedTLS's Subject writer keeps one value per OID, so a two-CN certificate cannot be generated;
        // issued_ok reads the CN through detail::single_cn, driven here over a hand-linked attribute list.
        const auto buf = [](std::string_view v) {
            mbedtls_asn1_buf b{};
            b.len = v.size();
            b.p = reinterpret_cast<unsigned char*>(const_cast<char*>(v.data()));
            return b;
        };
        const std::string_view cn_oid(MBEDTLS_OID_AT_CN, std::strlen(MBEDTLS_OID_AT_CN));
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        mbedtls_x509_name second{};
        second.oid = buf(cn_oid);
        second.val = buf(cn);
        crt.subject.oid = buf(cn_oid);
        crt.subject.val = buf(cn);
        std::string_view got;
        check(quark::adapters::detail::single_cn(crt, got) && got == cn, "control: one CN is read");
        crt.subject.next = &second;
        check(!quark::adapters::detail::single_cn(crt, got), "a Subject with two CNs (even equal ones) is refused");
        crt.subject.next = nullptr;  // stack-owned attributes: never hand them to mbedtls_x509_crt_free
        mbedtls_x509_crt_free(&crt);
    }
    CertSpec org = leaf_spec(cn);
    org.subject = "CN=" + cn + ",O=Quark";
    check(ok(ca.issue(spki, org)), "control: other Subject attributes do not matter");

    Pk other;
    gen_key(other.v);
    check(!ok(ca.issue(spki_der_of(other.v), leaf_spec(cn))), "a certificate for another key is refused");

    CertSpec is_ca = leaf_spec(cn);
    is_ca.is_ca = 1;
    check(!ok(ca.issue(spki, is_ca)), "a CA:TRUE leaf is refused");
    CertSpec cert_sign = leaf_spec(cn);
    cert_sign.key_usage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN;
    check(!ok(ca.issue(spki, cert_sign)), "keyCertSign is refused");
    CertSpec crl_sign = leaf_spec(cn);
    crl_sign.key_usage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_CRL_SIGN;
    check(!ok(ca.issue(spki, crl_sign)), "cRLSign is refused");

    CertSpec too_long = leaf_spec(cn);
    too_long.not_after = kNow + 366 * kDay;
    check(!ok(ca.issue(spki, too_long)), "validity beyond max_validity_s is refused");
    CertSpec at_bound = leaf_spec(cn);
    at_bound.not_after = kNow + 365 * kDay;
    check(ok(ca.issue(spki, at_bound)), "control: validity exactly max_validity_s is accepted");
    CertSpec future = leaf_spec(cn);
    future.not_before = kNow + 3600;
    check(!ok(ca.issue(spki, future)), "notBefore beyond the skew is refused");
    CertSpec in_skew = leaf_spec(cn);
    in_skew.not_before = kNow + 120;
    check(ok(ca.issue(spki, in_skew)), "control: notBefore within the skew is accepted");
    CertSpec inverted = leaf_spec(cn);
    inverted.not_before = kNow + 120;
    inverted.not_after = kNow + 60;
    check(!ok(ca.issue(spki, inverted)), "notBefore after notAfter is refused");
    CertSpec expired = leaf_spec(cn);
    expired.not_before = kNow - 10 * kDay;
    expired.not_after = kNow - kDay;
    check(!ok(ca.issue(spki, expired)), "an expired certificate is refused");

    // `rogue` has the SAME distinguished name as the trusted root, so only the signature tells them apart.
    const Bytes rogue_leaf = rogue.issue(spki, leaf_spec(cn));
    check(!ok(rogue_leaf), "a certificate from an untrusted CA with the trusted root's name is refused");
    check(!ok(concat(rogue_leaf, rogue.cert)), "a root carried inside the returned chain is never trusted");
    check(!ok(concat(ca.issue(spki, leaf_spec(cn)), Bytes{std::byte{0x30}, std::byte{0x01}})),
          "a chain with trailing garbage is refused");
    check(!ok({}), "an empty chain is refused");

    // Through an intermediate the custodian sends along: accepted, as long as it chains to a configured root.
    Pk subject;
    mbedtls_pk_parse_public_key(&subject.v, reinterpret_cast<const unsigned char*>(spki.data()), spki.size());
    Pk inter_key;
    gen_key(inter_key.v);
    const std::string inter_dn = "CN=Quark Issuing CA";
    const Bytes inter_cert = write_cert(inter_key.v, ca.key.v, ca.dn,
                                        {inter_dn, 1, MBEDTLS_X509_KU_KEY_CERT_SIGN, kNow - kDay, kNow + 365 * kDay});
    const Bytes via_inter = write_cert(subject.v, inter_key.v, inter_dn, leaf_spec(cn));
    check(ok(concat(via_inter, inter_cert)), "control: a leaf through an intermediate to a configured root is accepted");
    check(!ok(via_inter), "the same leaf without its intermediate is refused");
    // The leaf-only time callback masks nothing above the leaf: an expired intermediate still fails.
    const Bytes old_inter = write_cert(inter_key.v, ca.key.v, ca.dn,
                                       {inter_dn, 1, MBEDTLS_X509_KU_KEY_CERT_SIGN, kNow - 10 * kDay, kNow - kDay});
    check(!ok(concat(via_inter, old_inter)), "an expired intermediate is refused");
    // An end-entity certificate (CA:FALSE) cannot act as an intermediate.
    const Bytes not_ca_inter = write_cert(inter_key.v, ca.key.v, ca.dn, {inter_dn, 0, 0, kNow - kDay, kNow + 365 * kDay});
    check(!ok(concat(via_inter, not_ca_inter)), "a leaf signed by a non-CA certificate is refused");
}

void overlap_section(Ca& old_ca, Ca& new_ca) {
    static std::int64_t now_ns = 1'000;
    auto trust = std::make_shared<TrustStore>(std::make_shared<const TrustedRoots>(TrustedRoots{old_ca.cert}));
    auto clock = [](void* ctx) noexcept { return *static_cast<std::int64_t*>(ctx); };
    const MbedtlsEnrolCrypto crypto(trust, MbedtlsEnrolCrypto::kDefaultSkewS, clock, &now_ns);
    Pk joiner;
    gen_key(joiner.v);
    const Bytes spki = spki_der_of(joiner.v);
    const std::string cn = format_quark_cn(ClusterId{7}, NodeId{9});
    const enrol::IssuedExpectation expect{cn, spki, static_cast<std::uint64_t>(kNow), 365 * kDay};
    const Bytes by_old = old_ca.issue(spki, leaf_spec(cn));
    trust->rotate(std::make_shared<const TrustedRoots>(TrustedRoots{new_ca.cert}), now_ns, /*grace_ns=*/1'000);
    check(crypto.issued_ok(expect, by_old), "the previous root is still trusted within the overlap window");
    check(crypto.issued_ok(expect, new_ca.issue(spki, leaf_spec(cn))), "the new root is trusted");

    // C5 covers the overlap window too: the previous root's key, at startup and in the handshake engine.
    const TlsIdentity old_root_key{
        old_ca.issue(spki_der_of(old_ca.key.v), leaf_spec(format_quark_cn(ClusterId{7}, NodeId{1}))),
        key_der_of(old_ca.key.v), {}};
    check(!validate_identity_not_root(old_root_key, trust->snapshot(now_ns)),
          "C5: the previous (overlap-window) root's key is refused at startup");
    IdentityMaterial identity(std::make_shared<const TlsIdentity>(old_root_key));
    MbedtlsHandshakeEngineFactory factory;
    auto engine =
        factory.create(true, NodeId{1}, NodeId{2}, ClusterId{7}, identity.snapshot(0), trust->snapshot(now_ns), nullptr);
    Bytes out;
    check(engine->advance({}, out) == HandshakeEngine::Step::Failed &&
              std::string_view(engine->failure_reason()).find("C5") != std::string_view::npos,
          "C5: the handshake engine refuses the previous root's key");

    now_ns += 2'000;
    check(!crypto.issued_ok(expect, by_old), "the previous root is not trusted after the window");
}

// Enrollment end to end: a joiner's real key, CSR and MAC; a custodian that signs from the IssueRequest.
void end_to_end_section(Ca& ca, const std::shared_ptr<const TrustStore>& trust) {
    const ClusterId cluster{7};
    auto crypto = std::make_shared<const MbedtlsEnrolCrypto>(trust);
    enum class Mode { Good, CaTrue };
    Mode mode = Mode::Good;
    std::optional<enrol::IssueRequest> seen;
    enrol::Config cfg;
    cfg.cluster = cluster;
    enrol::Enrollment enr(cfg, crypto, [&](enrol::IssueRequest req, enrol::Enrollment::Completion done) {
        const NodeId node{req.role == enrol::Role::Client ? (kClientNodeBit | 11) : 11};
        CertSpec spec = leaf_spec(format_quark_cn(req.cluster, node));
        if (mode == Mode::CaTrue) spec.is_ca = 1;
        seen = req;
        done(enrol::Issued{node, ca.issue(req.spki_der, spec)});
    });
    const enrol::Ticks t0 = 1'000;

    auto present = [&](const enrol::Ticket& t, const Bytes& csr, const enrol::TicketSecret& secret) {
        enrol::EnrolRequest req;
        req.ticket_id = t.id;
        req.csr_der = csr;
        req.source = "198.51.100.0/24";
        const auto tag = compute_enrol_mac(secret, cluster, t.id, csr);
        check(tag.has_value(), "the joiner MACs its CSR");
        if (tag) req.tag = *tag;
        std::optional<enrol::Reply> reply;
        enr.submit(std::move(req), [&](enrol::Reply r) { reply = std::move(r); }, t0);
        return reply;
    };

    for (const enrol::Role role : {enrol::Role::Member, enrol::Role::Client}) {
        auto ticket = enr.issue_ticket(role, 60'000'000'000, t0);
        const auto key = generate_identity_key();
        const auto csr = key ? make_enrol_csr(*key) : result<Bytes>{};
        check(ticket && key && csr, "a ticket is issued and the joiner prepares key and CSR");
        if (!ticket || !key || !csr) return;
        const auto reply = present(*ticket, *csr, ticket->secret);
        check(reply && reply->status == enrol::Status::Issued, "a genuine presentation is issued");
        check(seen && !seen->spki_der.empty(), "the custodian is given the SPKI");
        if (!reply || reply->status != enrol::Status::Issued) continue;

        // The joiner installs what came back: its own key plus the issued chain — usable as a node identity.
        TlsIdentity id{reply->cert_chain_der, *key, {}};
        check(static_cast<bool>(validate_identity_not_root(id, trust->snapshot(0))),
              "the enrolled identity passes the C5 startup check");
        quark::adapters::detail::Crt leaf;
        NodeId node{};
        ClusterId got_cluster{};
        check(mbedtls_x509_crt_parse_der(&leaf.v, reinterpret_cast<const unsigned char*>(reply->cert_chain_der.data()),
                                         reply->cert_chain_der.size()) == 0 &&
                  quark::adapters::detail::parse_cn_identity(leaf.v, node, got_cluster) && got_cluster == cluster &&
                  is_client_node(node) == (role == enrol::Role::Client),
              "the issued certificate parses as the ticket's role and cluster in the handshake's own parser");
        const auto again = present(*ticket, *csr, ticket->secret);
        check(again && again->status == enrol::Status::TicketUsed, "the ticket is single use");
    }

    {
        auto ticket = enr.issue_ticket(enrol::Role::Member, 60'000'000'000, t0);
        const auto key = generate_identity_key();
        const auto csr = key ? make_enrol_csr(*key) : result<Bytes>{};
        check(ticket && key && csr, "setup for the negative cases");
        if (!ticket || !key || !csr) return;
        enrol::TicketSecret wrong = ticket->secret;
        wrong[0] ^= std::byte{1};
        const auto wrong_mac = present(*ticket, *csr, wrong);
        check(wrong_mac && wrong_mac->status == enrol::Status::BadMac, "a wrong secret is refused (BadMac)");
        Bytes tampered = *csr;
        tampered[tampered.size() / 2] ^= std::byte{0x01};
        const auto bad = present(*ticket, tampered, ticket->secret);
        check(bad && bad->status == enrol::Status::BadCsr, "a correctly MACed but broken CSR is refused (BadCsr)");
        mode = Mode::CaTrue;
        const auto r = present(*ticket, *csr, ticket->secret);
        check(r && r->status == enrol::Status::IssuedCertInvalid, "a custodian returning CA:TRUE is caught");
        const auto after = present(*ticket, *csr, ticket->secret);
        check(after && after->status == enrol::Status::TicketUsed, "and the ticket stays consumed (fail closed)");
    }
}

// One crypto object serves every presentation and completion thread: run its checks concurrently. A data
// race would surface under TSan; here every concurrent answer must still be right.
void concurrency_section(Ca& ca, const std::shared_ptr<const TrustStore>& trust) {
    const MbedtlsEnrolCrypto crypto(trust);
    const auto key = generate_identity_key();
    const auto csr = key ? make_enrol_csr(*key) : result<Bytes>{};
    check(key && csr, "setup for the concurrency case");
    if (!key || !csr) return;
    Bytes spki;
    check(crypto.csr_public_key(*csr, spki), "setup: the CSR's SPKI");
    const std::string cn = format_quark_cn(ClusterId{7}, NodeId{3});
    const Bytes good = ca.issue(spki, leaf_spec(cn));
    CertSpec bad_spec = leaf_spec(cn);
    bad_spec.is_ca = 1;
    const Bytes bad = ca.issue(spki, bad_spec);
    const enrol::IssuedExpectation expect{cn, spki, static_cast<std::uint64_t>(kNow), 365 * kDay};
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                Bytes s;
                if (!crypto.csr_public_key(*csr, s) || s != spki) ++wrong;
                if (!crypto.issued_ok(expect, good)) ++wrong;
                if (crypto.issued_ok(expect, bad)) ++wrong;
            }
        });
    for (auto& th : threads) th.join();
    check(wrong.load() == 0, "concurrent checks on one MbedtlsEnrolCrypto all give the right answer");
}

void c5_section(Ca& ca, const std::shared_ptr<const TrustStore>& trust) {
    const TrustStore::Snapshot roots = trust->snapshot(0);
    const TlsIdentity ca_as_node{ca.cert, key_der_of(ca.key.v), {}};
    check(!validate_identity_not_root(ca_as_node, roots), "C5: the CA certificate+key as node identity is refused");

    // A leaf certificate re-issued for the CA's own key: the key is still the root's.
    const Bytes ca_spki = spki_der_of(ca.key.v);
    const TlsIdentity ca_key_leaf{ca.issue(ca_spki, leaf_spec(format_quark_cn(ClusterId{7}, NodeId{1}))),
                                  key_der_of(ca.key.v), {}};
    check(!validate_identity_not_root(ca_key_leaf, roots), "C5: a leaf carrying the root's key is refused");

    Pk node_key;
    gen_key(node_key.v);
    // The certificate's key is checked as well as the private key held: a root-key certificate presented
    // with some other private key is refused, not left to fail later in the handshake.
    const TlsIdentity root_cert_other_key{ca_key_leaf.cert_chain_der, key_der_of(node_key.v), {}};
    check(!validate_identity_not_root(root_cert_other_key, roots),
          "C5: a certificate carrying the root's key is refused whatever private key accompanies it");

    // An issuing intermediate's certificate and key ("CA cert as node cert", one level down).
    Pk inter_key;
    gen_key(inter_key.v);
    const TlsIdentity inter_as_node{
        write_cert(inter_key.v, ca.key.v, ca.dn,
                   {"CN=" + format_quark_cn(ClusterId{7}, NodeId{1}), 1, MBEDTLS_X509_KU_KEY_CERT_SIGN, kNow - kDay,
                    kNow + 365 * kDay}),
        key_der_of(inter_key.v), {}};
    check(!validate_identity_not_root(inter_as_node, roots), "C5: an intermediate CA's certificate and key are refused");
    CertSpec crl_only = leaf_spec(format_quark_cn(ClusterId{7}, NodeId{1}));
    crl_only.key_usage = MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_CRL_SIGN;
    const TlsIdentity crl_signer{ca.issue(spki_der_of(node_key.v), crl_only), key_der_of(node_key.v), {}};
    check(!validate_identity_not_root(crl_signer, roots), "C5: a CRL-signing identity is refused");

    const TlsIdentity node{ca.issue(spki_der_of(node_key.v), leaf_spec(format_quark_cn(ClusterId{7}, NodeId{1}))),
                           key_der_of(node_key.v), {}};
    check(static_cast<bool>(validate_identity_not_root(node, roots)), "C5 control: an ordinary node identity passes");
    // The private key held is checked on its own: the root's key behind an ordinary certificate is refused.
    const TlsIdentity root_key_other_cert{node.cert_chain_der, key_der_of(ca.key.v), {}};
    check(!validate_identity_not_root(root_key_other_cert, roots),
          "C5: the root's private key is refused whatever certificate accompanies it");

    // The handshake engine refuses CA key material too, whatever the caller skipped at startup. Each case
    // trips one rule only: the key held, the certificate's key, or a CA-capable certificate.
    MbedtlsHandshakeEngineFactory factory;
    Bytes out;
    for (const TlsIdentity* bad : {&root_key_other_cert, &root_cert_other_key, &inter_as_node, &crl_signer}) {
        IdentityMaterial bad_identity(std::make_shared<const TlsIdentity>(*bad));
        auto engine = factory.create(true, NodeId{1}, NodeId{2}, ClusterId{7}, bad_identity.snapshot(0), roots, nullptr);
        check(engine->advance({}, out) == HandshakeEngine::Step::Failed &&
                  std::string_view(engine->failure_reason()).find("C5") != std::string_view::npos,
              "C5: the handshake engine refuses an identity that is CA key material");
    }
    IdentityMaterial good_identity(std::make_shared<const TlsIdentity>(node));
    auto good = factory.create(true, NodeId{1}, NodeId{2}, ClusterId{7}, good_identity.snapshot(0), roots, nullptr);
    check(good->advance({}, out) != HandshakeEngine::Step::Failed, "C5 control: an ordinary identity starts a handshake");
}

}  // namespace

int main() {
    Ca ca("Quark Test Root");
    Ca rogue("Quark Test Root");  // the same name as the trusted root, a different key
    Ca next("Quark Next Root");
    check(!ca.cert.empty() && !rogue.cert.empty() && !next.cert.empty(), "test PKI generated");
    const auto trust = std::make_shared<const TrustStore>(std::make_shared<const TrustedRoots>(TrustedRoots{ca.cert}));
    const MbedtlsEnrolCrypto crypto(trust);

    hmac_section(crypto);
    csr_section(crypto);
    issued_section(ca, rogue, trust);
    overlap_section(ca, next);
    end_to_end_section(ca, trust);
    concurrency_section(ca, trust);
    c5_section(ca, trust);

    std::printf("mbedtls_enrollment_check: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
