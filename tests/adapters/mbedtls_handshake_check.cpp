// Conformance check for ADR-040's real handshake: `quark::adapters::MbedtlsHandshakeEngine` — a REAL
// TLS 1.3 mutual handshake (no mocks), generating a self-signed test CA and per-node leaf certs on the
// fly via mbedTLS's own X.509 writer, then driving a client and server engine against each other by
// hand (feeding each side's `advance()` output into the other's `advance()` input) until both reach
// Done or one reaches Failed — proving: real loopback TLS1.3 round-trip (C1-equivalent), directional
// keys actually interoperate (C4's role-aware split — closes the fatal client/server key-split bug
// class the ADR record flags), NodeId/ClusterId binding via the leaf cert's CN, mismatched-CA
// rejection (mutual-auth gate before any session), and revoked-fingerprint rejection at handshake time
// (S3). Only built when QUARK_WITH_MBEDTLS=ON.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

using namespace quark;
using namespace quark::adapters;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// --- Minimal test PKI: a self-signed CA + CN-encoded leaf certs, all via mbedTLS's own writer. -----
class Rng {
public:
    Rng() {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
        static constexpr char kPers[] = "quark-test-pki";
        mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                              reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
    }
    ~Rng() {
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }
    static int f_rng(void* ctx, unsigned char* out, std::size_t len) {
        return mbedtls_ctr_drbg_random(static_cast<mbedtls_ctr_drbg_context*>(ctx), out, len);
    }
    [[nodiscard]] mbedtls_ctr_drbg_context* ctx() { return &drbg_; }

private:
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
};

struct GeneratedCert {
    std::vector<std::byte> cert_der;
    std::vector<std::byte> key_der;
};

void gen_ec_key(mbedtls_pk_context& pk, Rng& rng) {
    mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), &Rng::f_rng, rng.ctx());
}

[[nodiscard]] std::vector<std::byte> tail_bytes(std::vector<unsigned char>& buf, int written_len) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(buf.data() + buf.size() - static_cast<std::size_t>(written_len)),
                                  reinterpret_cast<const std::byte*>(buf.data() + buf.size()));
}

// Self-signed CA (subject == issuer, CA:true). Returns the cert and leaves `ca_key` populated so
// callers can sign leaf certs with it.
[[nodiscard]] GeneratedCert make_ca(Rng& rng, mbedtls_pk_context& ca_key, const char* cn) {
    gen_ec_key(ca_key, rng);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &ca_key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &ca_key);
    const std::string dn = std::string("CN=") + cn;
    mbedtls_x509write_crt_set_subject_name(&crt, dn.c_str());
    mbedtls_x509write_crt_set_issuer_name(&crt, dn.c_str());
    const unsigned char serial[] = {1};
    mbedtls_x509write_crt_set_serial_raw(&crt, const_cast<unsigned char*>(serial), 1);
    mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20351231235959");
    mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);

    std::vector<unsigned char> cert_buf(4096);
    const int cert_len = mbedtls_x509write_crt_der(&crt, cert_buf.data(), cert_buf.size(), &Rng::f_rng, rng.ctx());
    std::vector<unsigned char> key_buf(4096);
    const int key_len = mbedtls_pk_write_key_der(&ca_key, key_buf.data(), key_buf.size());
    mbedtls_x509write_crt_free(&crt);

    GeneratedCert out;
    out.cert_der = tail_bytes(cert_buf, cert_len);
    out.key_der = tail_bytes(key_buf, key_len);
    return out;
}

// Leaf cert signed by `ca_key`/`ca_cert`, CN = "quark:<cluster_id>:<node_id>" (parsed by
// mbedtls_handshake.hpp's detail::parse_cn_identity). `subject_override` replaces the whole Subject DN
// and `is_ca` sets basicConstraints CA:TRUE — both only for the ADR-067 negative controls.
[[nodiscard]] GeneratedCert make_leaf(Rng& rng, const GeneratedCert& ca_cert, mbedtls_pk_context& ca_key,
                                      std::uint64_t cluster_id, std::uint64_t node_id,
                                      const std::string& subject_override = {}, int is_ca = 0) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    gen_ec_key(pk, rng);

    mbedtls_x509_crt ca_parsed;
    mbedtls_x509_crt_init(&ca_parsed);
    mbedtls_x509_crt_parse_der(&ca_parsed, reinterpret_cast<const unsigned char*>(ca_cert.cert_der.data()),
                               ca_cert.cert_der.size());
    char issuer_dn[256];
    mbedtls_x509_dn_gets(issuer_dn, sizeof(issuer_dn), &ca_parsed.subject);

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &ca_key);
    const std::string subject_dn = !subject_override.empty()
                                       ? subject_override
                                       : "CN=quark:" + std::to_string(cluster_id) + ":" + std::to_string(node_id);
    mbedtls_x509write_crt_set_subject_name(&crt, subject_dn.c_str());
    mbedtls_x509write_crt_set_issuer_name(&crt, issuer_dn);
    unsigned char serial[8];
    for (int i = 0; i < 8; ++i) serial[i] = static_cast<unsigned char>((node_id >> (i * 8)) & 0xFF);
    if (serial[0] == 0) serial[0] = 1;
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, 8);
    mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20351231235959");
    mbedtls_x509write_crt_set_basic_constraints(&crt, is_ca, -1);

    std::vector<unsigned char> cert_buf(4096);
    const int cert_len = mbedtls_x509write_crt_der(&crt, cert_buf.data(), cert_buf.size(), &Rng::f_rng, rng.ctx());
    std::vector<unsigned char> key_buf(4096);
    const int key_len = mbedtls_pk_write_key_der(&pk, key_buf.data(), key_buf.size());

    mbedtls_x509write_crt_free(&crt);
    mbedtls_x509_crt_free(&ca_parsed);
    mbedtls_pk_free(&pk);

    GeneratedCert out;
    out.cert_der = tail_bytes(cert_buf, cert_len);
    out.key_der = tail_bytes(key_buf, key_len);
    return out;
}

// Drives `client`/`server` engines against each other until both reach a terminal step (Done/Failed),
// feeding each side's produced bytes as the other's next `advance()` input. Bounded round count as a
// hang backstop (a real TLS1.3 1-RTT handshake needs only a handful).
struct DriveResult {
    HandshakeEngine::Step client_step = HandshakeEngine::Step::WantRead;
    HandshakeEngine::Step server_step = HandshakeEngine::Step::WantRead;
    int rounds = 0;
};
[[nodiscard]] DriveResult drive(HandshakeEngine& client, HandshakeEngine& server) {
    DriveResult r;
    std::vector<std::byte> client_to_server, server_to_client, scratch;
    r.client_step = client.advance(std::span<const std::byte>{}, client_to_server);  // client speaks first
    constexpr int kMaxRounds = 20;
    while (r.rounds++ < kMaxRounds) {
        if (r.client_step == HandshakeEngine::Step::Done || r.client_step == HandshakeEngine::Step::Failed)
            if (r.server_step == HandshakeEngine::Step::Done || r.server_step == HandshakeEngine::Step::Failed)
                break;
        if (!client_to_server.empty()) {
            scratch.clear();
            r.server_step = server.advance(client_to_server, scratch);
            client_to_server.clear();
            server_to_client.insert(server_to_client.end(), scratch.begin(), scratch.end());
        }
        if (!server_to_client.empty()) {
            scratch.clear();
            r.client_step = client.advance(server_to_client, scratch);
            server_to_client.clear();
            client_to_server.insert(client_to_server.end(), scratch.begin(), scratch.end());
        }
        if (client_to_server.empty() && server_to_client.empty() &&
            r.client_step != HandshakeEngine::Step::Done && r.server_step != HandshakeEngine::Step::Done)
            break;  // both stalled with nothing to exchange — a bug, not a hang; stop and let checks fail
    }
    return r;
}
}  // namespace

int main() {
    bool ok = true;
    Rng rng;
    const ClusterId cluster{777};

    mbedtls_pk_context ca_key;
    mbedtls_pk_init(&ca_key);
    const GeneratedCert ca = make_ca(rng, ca_key, "quark-test-ca");
    const GeneratedCert leaf1 = make_leaf(rng, ca, ca_key, cluster.value, 1);
    const GeneratedCert leaf2 = make_leaf(rng, ca, ca_key, cluster.value, 2);
    // ADR-067 D1/D3 negative controls, all validly chained to `ca` and presented as n2 (or a client id).
    const std::string cl = std::to_string(cluster.value);
    const NodeId client_id{kClientNodeBit | 2};
    const GeneratedCert leaf_trailing = make_leaf(rng, ca, ca_key, cluster.value, 2, "CN=quark:" + cl + ":2x");
    const GeneratedCert leaf_ca_flag = make_leaf(rng, ca, ca_key, cluster.value, 2, {}, 1);
    const GeneratedCert leaf_client =
        make_leaf(rng, ca, ca_key, cluster.value, 0, "CN=quark-client:" + cl + ":" + std::to_string(client_id.value));
    const GeneratedCert leaf_client_member_id = make_leaf(rng, ca, ca_key, cluster.value, 0, "CN=quark-client:" + cl + ":2");
    mbedtls_pk_free(&ca_key);

    auto id1 = std::make_shared<const TlsIdentity>(TlsIdentity{leaf1.cert_der, leaf1.key_der, {}});
    auto id2 = std::make_shared<const TlsIdentity>(TlsIdentity{leaf2.cert_der, leaf2.key_der, {}});
    auto trust = std::make_shared<const TrustedRoots>(TrustedRoots{ca.cert_der});
    IdentityMaterial identity1(id1), identity2(id2);
    TrustStore trust_store(trust);

    const NodeId n1{1}, n2{2};

    // --- Real end-to-end handshake: n1 (client, lower id) <-> n2 (server). --------------------------
    {
        MbedtlsHandshakeEngineFactory factory;
        auto client = factory.create(true, n1, n2, cluster, identity1.snapshot(0), trust_store.snapshot(0), nullptr);
        auto server = factory.create(false, n2, n1, cluster, identity2.snapshot(0), trust_store.snapshot(0), nullptr);
        const DriveResult r = drive(*client, *server);

        check(r.client_step == HandshakeEngine::Step::Done, "client reaches Done", ok);
        check(r.server_step == HandshakeEngine::Step::Done, "server reaches Done", ok);
        if (r.client_step == HandshakeEngine::Step::Done && r.server_step == HandshakeEngine::Step::Done) {
            HandshakeEngine::Result cr = client->take_result();
            HandshakeEngine::Result sr = server->take_result();
            check(cr.peer_node_id == n2, "client learned peer NodeId == n2", ok);
            check(cr.peer_cluster_id == cluster, "client learned peer ClusterId", ok);
            check(sr.peer_node_id == n1, "server learned peer NodeId == n1", ok);
            check(cr.send_cipher && cr.recv_cipher && sr.send_cipher && sr.recv_cipher,
                  "all four directional ciphers are non-null", ok);

            // C4: directional keys actually interoperate — client.send must decrypt under server.recv,
            // and server.send must decrypt under client.recv (NOT the same cipher in both directions).
            std::vector<std::byte> sealed, opened;
            const std::vector<std::byte> aad = {std::byte{1}}, msg = {std::byte{'h'}, std::byte{'i'}};
            cr.send_cipher->seal(1, aad, msg, sealed);
            check(sr.recv_cipher->open(1, aad, sealed, opened) && opened == msg,
                  "client.send_cipher interops with server.recv_cipher", ok);

            sealed.clear();
            opened.clear();
            sr.send_cipher->seal(1, aad, msg, sealed);
            check(cr.recv_cipher->open(1, aad, sealed, opened) && opened == msg,
                  "server.send_cipher interops with client.recv_cipher", ok);

            // The two directions must be genuinely DIFFERENT keys (a same-key bug would still pass the
            // interop check above, so verify cross-direction opening FAILS).
            sealed.clear();
            cr.send_cipher->seal(2, aad, msg, sealed);
            std::vector<std::byte> wrong_direction;
            check(!cr.recv_cipher->open(2, aad, sealed, wrong_direction),
                  "client's own send/recv keys are genuinely different (not accidentally equal)", ok);
        }
    }

    // --- CONTROL: mismatched CA — peer's leaf isn't signed by a trust root we recognize. -------------
    {
        mbedtls_pk_context other_ca_key;
        mbedtls_pk_init(&other_ca_key);
        const GeneratedCert other_ca = make_ca(rng, other_ca_key, "quark-other-ca");
        const GeneratedCert rogue_leaf = make_leaf(rng, other_ca, other_ca_key, cluster.value, 2);
        mbedtls_pk_free(&other_ca_key);

        auto rogue_identity_blob =
            std::make_shared<const TlsIdentity>(TlsIdentity{rogue_leaf.cert_der, rogue_leaf.key_der, {}});
        IdentityMaterial rogue_identity(rogue_identity_blob);

        MbedtlsHandshakeEngineFactory factory;
        auto client = factory.create(true, n1, n2, cluster, identity1.snapshot(0), trust_store.snapshot(0), nullptr);
        auto server =
            factory.create(false, n2, n1, cluster, rogue_identity.snapshot(0), trust_store.snapshot(0), nullptr);
        const DriveResult r = drive(*client, *server);
        check(r.client_step == HandshakeEngine::Step::Failed || r.server_step == HandshakeEngine::Step::Failed,
              "CONTROL: mismatched-CA handshake fails on at least one side (mutual-auth gate)", ok);
    }

    // --- CONTROL: revoked-but-unexpired leaf is rejected at handshake time (S3). ---------------------
    {
        MbedtlsHandshakeEngineFactory factory;
        auto client = factory.create(true, n1, n2, cluster, identity1.snapshot(0), trust_store.snapshot(0), nullptr);
        // n2's leaf fingerprint is unknown to the test in advance (computed inside the adapter from
        // the DER), so instead revoke by re-deriving it the same way the adapter does: SHA-256 of the
        // leaf DER.
        Fingerprint revoked_fp{};
        {
            mbedtls_x509_crt tmp;
            mbedtls_x509_crt_init(&tmp);
            mbedtls_x509_crt_parse_der(&tmp, reinterpret_cast<const unsigned char*>(leaf2.cert_der.data()),
                                       leaf2.cert_der.size());
            mbedtls_sha256(tmp.raw.p, tmp.raw.len, reinterpret_cast<unsigned char*>(revoked_fp.data()), 0);
            mbedtls_x509_crt_free(&tmp);
        }
        auto revoked = std::make_shared<const std::unordered_set<Fingerprint>>(
            std::unordered_set<Fingerprint>{revoked_fp});
        // The CLIENT verifies the SERVER's (n2's, revoked) leaf — pass `revoked` on the side that
        // will actually run verify_cb against that certificate.
        auto client_with_revocation =
            factory.create(true, n1, n2, cluster, identity1.snapshot(0), trust_store.snapshot(0), revoked);
        auto server =
            factory.create(false, n2, n1, cluster, identity2.snapshot(0), trust_store.snapshot(0), nullptr);
        const DriveResult r = drive(*client_with_revocation, *server);
        check(r.client_step == HandshakeEngine::Step::Failed || r.server_step == HandshakeEngine::Step::Failed,
              "CONTROL: revoked-but-unexpired leaf certificate is rejected (S3)", ok);
    }

    // --- ADR-067: the client (n1) verifies a server presenting a CA-chained but non-conforming leaf. ---
    // Each must fail on the client for the stated reason; the first block above is the positive control.
    const auto refused_for = [&](const GeneratedCert& server_leaf, NodeId expected_peer, std::string_view reason,
                                 const char* what) {
        auto blob = std::make_shared<const TlsIdentity>(TlsIdentity{server_leaf.cert_der, server_leaf.key_der, {}});
        IdentityMaterial server_identity(blob);
        MbedtlsHandshakeEngineFactory factory;
        auto client =
            factory.create(true, n1, expected_peer, cluster, identity1.snapshot(0), trust_store.snapshot(0), nullptr);
        auto server = factory.create(false, expected_peer, n1, cluster, server_identity.snapshot(0),
                                     trust_store.snapshot(0), nullptr);
        const DriveResult r = drive(*client, *server);
        const std::string_view got = client->failure_reason();
        const bool refused = r.client_step == HandshakeEngine::Step::Failed && got.find(reason) != std::string_view::npos;
        check(refused, what, ok);
        if (!refused)
            std::fprintf(stderr, "    (client step %d, reason: %.*s)\n", static_cast<int>(r.client_step),
                         static_cast<int>(got.size()), got.data());
    };
    refused_for(leaf_trailing, n2, "CN does not encode", "ADR-067: trailing bytes after the node id are refused");
    refused_for(leaf_ca_flag, n2, "mbedtls_ssl_handshake", "ADR-067: a leaf claiming CA:TRUE is refused");
    refused_for(leaf_client_member_id, n2, "CN does not encode",
                "ADR-067: a client CN carrying a MEMBER id is refused (disjoint id spaces)");

    // --- ADR-067 D3 positive: a client identity (TLS client, as SecureTransport's role rule makes it)
    // handshakes with member n2, and n2 learns the client id — bit set — from the certificate. -------
    {
        auto blob = std::make_shared<const TlsIdentity>(TlsIdentity{leaf_client.cert_der, leaf_client.key_der, {}});
        IdentityMaterial client_identity(blob);
        MbedtlsHandshakeEngineFactory factory;
        auto client =
            factory.create(true, client_id, n2, cluster, client_identity.snapshot(0), trust_store.snapshot(0), nullptr);
        auto server =
            factory.create(false, n2, client_id, cluster, identity2.snapshot(0), trust_store.snapshot(0), nullptr);
        const DriveResult r = drive(*client, *server);
        check(r.client_step == HandshakeEngine::Step::Done && r.server_step == HandshakeEngine::Step::Done,
              "ADR-067: client identity <-> member handshake completes", ok);
        if (r.server_step == HandshakeEngine::Step::Done) {
            const HandshakeEngine::Result sr = server->take_result();
            check(sr.peer_node_id == client_id && is_client_node(sr.peer_node_id),
                  "ADR-067: member learns the client id (client bit set) from the certificate", ok);
        }
    }

    // --- ADR-067: exactly one CN. mbedTLS's Subject writer keeps one value per OID, so a two-CN
    // certificate cannot be generated here; drive detail::parse_cn_identity over a hand-linked Subject
    // attribute list instead (CN, O, CN), with the single-CN list as the positive control.
    {
        const auto buf = [](std::string_view s) {
            mbedtls_asn1_buf b{};
            b.len = s.size();
            b.p = reinterpret_cast<unsigned char*>(const_cast<char*>(s.data()));
            return b;
        };
        const std::string_view cn_oid(MBEDTLS_OID_AT_CN, std::strlen(MBEDTLS_OID_AT_CN));
        const std::string_view o_oid(MBEDTLS_OID_AT_ORGANIZATION, std::strlen(MBEDTLS_OID_AT_ORGANIZATION));
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        mbedtls_x509_name second_cn{}, org{};
        second_cn.oid = buf(cn_oid);
        second_cn.val = buf("quark:777:3");
        org.oid = buf(o_oid);
        org.val = buf("quark");
        crt.subject.oid = buf(cn_oid);
        crt.subject.val = buf("quark:777:2");
        crt.subject.next = &org;

        NodeId node{};
        ClusterId cl_id{};
        check(adapters::detail::parse_cn_identity(crt, node, cl_id) && node == n2 && cl_id == cluster,
              "ADR-067 control: one CN (plus a non-CN attribute) parses", ok);
        org.next = &second_cn;
        check(!adapters::detail::parse_cn_identity(crt, node, cl_id),
              "ADR-067: a Subject with two CNs is refused", ok);
        crt.subject.next = nullptr;  // the attributes are stack-owned: never hand them to mbedtls_x509_crt_free
        mbedtls_x509_crt_free(&crt);
    }

    std::printf("mbedtls_handshake_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
