// Conformance check for ADR-074 D4 / issue #36 (D4c step 2) over REAL crypto: the server-authenticated channel an enrolment travels in
// (`EnrolServerChannel` / `EnrolJoinerChannel`, mbedtls_enrol_channel.hpp). A test PKI is generated on the fly (the application's custodian, the only
// signer, is simulated here as in mbedtls_enrollment_check.cpp). Every refusal has the same builder's accepted case as its positive control (E1).
//
//   E1  the joiner, holding only the join code's pin, completes a TLS 1.3 handshake with the anchor WITHOUT presenting a certificate, then a request and
//       a reply cross as length-prefixed application data, in both directions, and across arbitrary chunking
//   E2  the joiner refuses (Failed, and no byte of its last flight leaves): a different CA hash; a chain without the pinned CA; a leaf issued by a rogue
//       CA behind the real root; a CLIENT certificate for the anchor's id; another cluster; another member; an expired leaf; a leaf that is CA-capable;
//       key usage without digitalSignature; extended key usage without serverAuth; a pin that names the leaf itself
//   E3  seal() refuses before the handshake is done; garbage and an oversized flood fail the server; tampering with a sealed record fails open()
//   E4  the server refuses to start with a CA-capable own certificate (ADR-074 C5)
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_enrol_channel.hpp"
#include "quark/core/enrol_wire.hpp"

using namespace quark;
using namespace quark::adapters;
using quark::adapters::detail::Pk;
using quark::adapters::detail::SharedRng;

namespace {

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

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

// --- Harness ---------------------------------------------------------------------------------------------------

struct Pair {
    std::unique_ptr<EnrolServerChannel> server;
    std::unique_ptr<EnrolJoinerChannel> joiner;
    bool joiner_failed = false, server_failed = false, both_done = false;
    Bytes joiner_last_flight_after_failure;  // bytes the joiner appended in the step that failed
};

// Drives the handshake to completion or failure. `chain_der` is what the anchor presents (leaf + whatever the test appends).
Pair handshake(const Bytes& chain_der, const Bytes& key_der, const enrol::wire::RootHash& pin, std::uint64_t pin_cluster, std::uint64_t pin_node) {
    Pair p;
    TlsIdentity id;
    id.cert_chain_der = chain_der;
    id.private_key_der = key_der;
    p.server = std::make_unique<EnrolServerChannel>(id);
    p.joiner = std::make_unique<EnrolJoinerChannel>(AnchorPin{NodeId{pin_node}, ClusterId{pin_cluster}, pin});
    Bytes to_server, to_joiner;
    EnrolChannel::Step js = p.joiner->advance({}, to_server), ss = EnrolChannel::Step::WantRead;
    for (int i = 0; i < 12; ++i) {
        if (js == EnrolChannel::Step::Failed) {
            p.joiner_failed = true;
            p.joiner_last_flight_after_failure = to_server;
            return p;
        }
        if (!to_server.empty() || ss != EnrolChannel::Step::Done) {
            Bytes in = std::move(to_server);
            to_server.clear();
            if (ss != EnrolChannel::Step::Done || !in.empty()) ss = p.server->advance(in, to_joiner);
            if (ss == EnrolChannel::Step::Failed) {
                p.server_failed = true;
                return p;
            }
        }
        if (js == EnrolChannel::Step::Done && ss == EnrolChannel::Step::Done) break;
        Bytes in = std::move(to_joiner);
        to_joiner.clear();
        if (js != EnrolChannel::Step::Done) js = p.joiner->advance(in, to_server);
        if (js == EnrolChannel::Step::Done && ss == EnrolChannel::Step::Done) break;
    }
    if (js == EnrolChannel::Step::Failed) {
        p.joiner_failed = true;
        p.joiner_last_flight_after_failure = to_server;
    }
    p.both_done = (js == EnrolChannel::Step::Done && ss == EnrolChannel::Step::Done);
    return p;
}

struct Anchor {
    Ca root{"quark-test-root"};
    Leaf leaf;
    Bytes chain;  // leaf + root
    enrol::wire::RootHash pin;
    explicit Anchor(const CertSpec* spec = nullptr, std::uint64_t cluster = kCluster, std::uint64_t node = kAnchor) {
        leaf = make_leaf(root, spec ? *spec : good_spec(cn_for(cluster, node)));
        chain = concat(leaf.cert, root.cert);
        pin = sha256_of(root.cert);
    }
};

bool joiner_refuses(const Pair& p) {
    const bool r = p.joiner_failed && p.joiner_last_flight_after_failure.empty() && !p.both_done;
    return r;
}

// For certificates mbedTLS's own server side already refuses: no session, and nothing sent by the joiner after the failure.
bool no_session(const Pair& p) { return !p.both_done && p.joiner_last_flight_after_failure.empty(); }

}  // namespace

int main() {
    // ---------------------------------------------------------------- E1 positive control and data
    {
        Anchor a;
        Pair p = handshake(a.chain, a.leaf.key_der, a.pin, kCluster, kAnchor);
        check(p.both_done, "E1 the joiner (no certificate, pin only) completes a TLS 1.3 handshake with the anchor");
        if (p.both_done) {
            enrol::EnrolRequest rq;
            for (std::size_t i = 0; i < rq.ticket_id.size(); ++i) rq.ticket_id[i] = static_cast<std::byte>(i + 1);
            for (std::size_t i = 0; i < rq.tag.size(); ++i) rq.tag[i] = static_cast<std::byte>(0x80 + i);
            rq.csr_der = Bytes(700, std::byte{0x30});
            const Bytes req_msg = *enrol::wire::encode_request(rq);
            Bytes record;
            check(p.joiner->seal(enrol::wire::frame_message(req_msg), record) && !record.empty(), "E1 the joiner seals a framed request");
            // The ciphertext must not contain the request in the clear.
            bool leaked = false;
            for (std::size_t i = 0; i + 32 <= record.size() && !leaked; ++i)
                leaked = std::memcmp(record.data() + i, req_msg.data() + 17, 32) == 0;  // the MAC tag
            check(!leaked, "E1 the MAC tag is not visible in the sealed record");

            // Deliver in chunks of 7 bytes to the server.
            enrol::wire::MessageReader rd;
            Bytes plain, alert, got;
            bool read_ok = true;
            for (std::size_t at = 0; at < record.size(); at += 7) {
                const std::size_t n = std::min<std::size_t>(7, record.size() - at);
                read_ok = read_ok && p.server->open(std::span<const std::byte>(record).subspan(at, n), plain, alert) == EnrolChannel::ReadResult::Ok;
                if (!plain.empty()) {
                    read_ok = read_ok && rd.feed(plain);
                    plain.clear();
                }
            }
            check(read_ok && rd.next(got) && got == req_msg, "E1 the server reads the whole request across 7-byte chunks");

            const Bytes reply_msg = *enrol::wire::encode_reply(enrol::wire::WireReply{enrol::WireStatus::Issued, Bytes(900, std::byte{0x31})});
            Bytes reply_record;
            check(p.server->seal(enrol::wire::frame_message(reply_msg), reply_record), "E1 the server seals a framed reply");
            Bytes back, a2;
            enrol::wire::MessageReader rd2;
            const auto rr = p.joiner->open(reply_record, back, a2);
            Bytes got2;
            check(rr == EnrolChannel::ReadResult::Ok && rd2.feed(back) && rd2.next(got2) && got2 == reply_msg, "E1 the joiner reads the reply");
        }
    }

    // ---------------------------------------------------------------- E2 the joiner refuses
    {
        Anchor a;
        // a different CA hash
        auto wrong = a.pin;
        wrong[0] ^= std::byte{1};
        check(joiner_refuses(handshake(a.chain, a.leaf.key_der, wrong, kCluster, kAnchor)), "E2 a different CA hash is refused");
        // a chain without the pinned CA (leaf only)
        check(joiner_refuses(handshake(a.leaf.cert, a.leaf.key_der, a.pin, kCluster, kAnchor)), "E2 a chain that does not contain the pinned CA is refused");
        // a leaf issued by a rogue CA, sent behind the REAL root (whose hash matches the pin)
        {
            Ca rogue("rogue-ca");
            Leaf l = make_leaf(rogue, good_spec(cn_for(kCluster, kAnchor)));
            check(joiner_refuses(handshake(concat(l.cert, a.root.cert), l.key_der, a.pin, kCluster, kAnchor)),
                  "E2 a leaf from a rogue CA behind the real root is refused (signature, not name)");
        }
        // a CLIENT certificate carrying the anchor's id with the client bit
        {
            const std::uint64_t client_node = kAnchor | (1ull << 63);
            CertSpec s = good_spec(cn_for(kCluster, client_node));
            Anchor c(&s);
            check(joiner_refuses(handshake(c.chain, c.leaf.key_der, c.pin, kCluster, kAnchor)), "E2 a client certificate cannot stand in for the anchor");
            check(joiner_refuses(handshake(c.chain, c.leaf.key_der, c.pin, kCluster, client_node)), "E2 ... not even when the pin names the client id");
        }
        // another cluster, another member
        {
            Anchor other_cluster(nullptr, kCluster + 1, kAnchor);
            check(joiner_refuses(handshake(other_cluster.chain, other_cluster.leaf.key_der, other_cluster.pin, kCluster, kAnchor)),
                  "E2 a certificate for another cluster is refused");
            Anchor other_node(nullptr, kCluster, kAnchor + 1);
            check(joiner_refuses(handshake(other_node.chain, other_node.leaf.key_der, other_node.pin, kCluster, kAnchor)),
                  "E2 a certificate for another member is refused");
        }
        // an expired leaf
        {
            CertSpec s = good_spec(cn_for(kCluster, kAnchor));
            s.not_before = kNow - 10 * kDay;
            s.not_after = kNow - kDay;
            Anchor e(&s);
            check(joiner_refuses(handshake(e.chain, e.leaf.key_der, e.pin, kCluster, kAnchor)), "E2 an expired leaf is refused");
        }
        // key usage without digitalSignature
        {
            CertSpec s = good_spec(cn_for(kCluster, kAnchor));
            s.key_usage = MBEDTLS_X509_KU_KEY_ENCIPHERMENT;
            Anchor e(&s);
            // mbedTLS as the SERVER refuses such a certificate itself (TLS 1.3 signs with it), so the joiner never sees it: the property tested is that no
            // session forms and the joiner sends nothing. The joiner's own key-usage and EKU branches in verify_peer() are defence in depth against a
            // server that is not mbedTLS; no such server exists in this repo to reach them (see the same limit noted in ADR-074 for the CA:TRUE peer).
            check(no_session(handshake(e.chain, e.leaf.key_der, e.pin, kCluster, kAnchor)), "E2 key usage without digitalSignature: no session forms");
        }
        // extended key usage without serverAuth
        {
            CertSpec s = good_spec(cn_for(kCluster, kAnchor));
            s.eku_oid = MBEDTLS_OID_CLIENT_AUTH;
            s.eku_len = MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH);
            Anchor e(&s);
            check(no_session(handshake(e.chain, e.leaf.key_der, e.pin, kCluster, kAnchor)), "E2 extended key usage without serverAuth: no session forms");
            CertSpec ok_spec = good_spec(cn_for(kCluster, kAnchor));
            ok_spec.eku_oid = MBEDTLS_OID_SERVER_AUTH;
            ok_spec.eku_len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
            Anchor g(&ok_spec);
            check(handshake(g.chain, g.leaf.key_der, g.pin, kCluster, kAnchor).both_done, "E2 CONTROL: serverAuth present is accepted");
        }
        // a pin that names the leaf itself
        check(joiner_refuses(handshake(a.chain, a.leaf.key_der, sha256_of(a.leaf.cert), kCluster, kAnchor)), "E2 a pin equal to the leaf is refused (the pin must be a CA)");
        // positive control again, after all the refusals above (no shared state)
        check(handshake(a.chain, a.leaf.key_der, a.pin, kCluster, kAnchor).both_done, "E2 CONTROL: the builder's accepted case still completes");
    }

    // ---------------------------------------------------------------- E3 lifecycle and abuse
    {
        Anchor a;
        TlsIdentity id;
        id.cert_chain_der = a.chain;
        id.private_key_der = a.leaf.key_der;
        EnrolJoinerChannel early(AnchorPin{NodeId{kAnchor}, ClusterId{kCluster}, a.pin});
        Bytes out;
        check(!early.seal(Bytes(10, std::byte{1}), out) && out.empty(), "E3 seal() before the handshake is done refuses and produces nothing");

        EnrolServerChannel srv(id);
        Bytes junk(300, std::byte{0x17}), reply;
        const auto st = srv.advance(junk, reply);
        check(st == EnrolChannel::Step::Failed, "E3 garbage fails the server's handshake");

        EnrolServerChannel srv2(id);
        bool failed = false;
        for (int i = 0; i < 200 && !failed; ++i) {
            Bytes chunk(1000, std::byte{0x16}), r;
            failed = srv2.advance(chunk, r) == EnrolChannel::Step::Failed;
        }
        check(failed, "E3 an endless flood fails the server (bounded buffer)");

        Pair p = handshake(a.chain, a.leaf.key_der, a.pin, kCluster, kAnchor);
        if (p.both_done) {
            Bytes record;
            (void)p.joiner->seal(enrol::wire::frame_message(Bytes(64, std::byte{7})), record);
            Bytes tampered = record;
            tampered[tampered.size() / 2] ^= std::byte{0x40};
            Bytes plain, alert;
            check(p.server->open(tampered, plain, alert) == EnrolChannel::ReadResult::Failed && plain.empty(), "E3 a tampered record fails open() and yields no data");
            Pair q = handshake(a.chain, a.leaf.key_der, a.pin, kCluster, kAnchor);
            Bytes rec2, plain2, alert2;
            (void)q.joiner->seal(enrol::wire::frame_message(Bytes(64, std::byte{7})), rec2);
            check(q.server->open(rec2, plain2, alert2) == EnrolChannel::ReadResult::Ok && plain2.size() == 68, "E3 CONTROL: the untampered record opens");
        }
    }

    // ---------------------------------------------------------------- E4 C5
    {
        CertSpec s = good_spec(cn_for(kCluster, kAnchor));
        s.is_ca = 1;
        s.key_usage = MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
        Anchor c(&s);
        TlsIdentity id;
        id.cert_chain_der = c.chain;
        id.private_key_der = c.leaf.key_der;
        EnrolServerChannel srv(id);
        Bytes out;
        check(srv.advance(Bytes(5, std::byte{0}), out) == EnrolChannel::Step::Failed, "E4 the server refuses to run with a CA-capable certificate (C5)");
        Anchor g;
        TlsIdentity gid;
        gid.cert_chain_der = g.chain;
        gid.private_key_der = g.leaf.key_der;
        EnrolServerChannel good(gid);
        Bytes out2;
        check(good.advance(Bytes(5, std::byte{0}), out2) == EnrolChannel::Step::Failed, "E4 (sanity) a server with a good certificate fails on garbage, not on setup");
        check(std::string(good.failure_reason()).find("setup") == std::string::npos, "E4 CONTROL: the good server's failure is not a setup failure");
        check(std::string(srv.failure_reason()).find("setup") != std::string::npos, "E4 the CA-capable server's failure IS a setup failure");
    }

    std::printf(g_ok ? "mbedtls_enrol_channel_check: all checks passed\n" : "mbedtls_enrol_channel_check: FAILED\n");
    return g_ok ? 0 : 1;
}
