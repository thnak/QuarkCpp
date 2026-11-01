// A minimal test PKI (self-signed CA + CN-encoded leaf certs) over mbedTLS's own X.509 writer, for the cross-mode check
// (pinned_crossmode_check.cpp). Copied verbatim from mbedtls_handshake_check.cpp (lines "Minimal test PKI" .. make_leaf) rather
// than shared, so the existing TLS-mode check stays untouched by the pinned-key work (#40: TLS/CA behaviour unchanged).
#pragma once
#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

namespace test_pki {

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
// mbedtls_handshake.hpp's detail::parse_cn_identity).
[[nodiscard]] GeneratedCert make_leaf(Rng& rng, const GeneratedCert& ca_cert, mbedtls_pk_context& ca_key,
                                      std::uint64_t cluster_id, std::uint64_t node_id) {
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
    const std::string subject_dn =
        "CN=quark:" + std::to_string(cluster_id) + ":" + std::to_string(node_id);
    mbedtls_x509write_crt_set_subject_name(&crt, subject_dn.c_str());
    mbedtls_x509write_crt_set_issuer_name(&crt, issuer_dn);
    unsigned char serial[8];
    for (int i = 0; i < 8; ++i) serial[i] = static_cast<unsigned char>((node_id >> (i * 8)) & 0xFF);
    if (serial[0] == 0) serial[0] = 1;
    mbedtls_x509write_crt_set_serial_raw(&crt, serial, 8);
    mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20351231235959");
    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

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
}  // namespace test_pki
