// Implements ADR-040 / ADR-067 D1 — shared X.509 helpers for the mbedTLS adapters, so the TLS handshake
// (mbedtls_handshake.hpp) and the revocation verifier (mbedtls_trust_anchor.hpp) parse a trust bundle the
// same way and therefore trust exactly the same roots.
#pragma once

#include <cstddef>
#include <span>

#include <mbedtls/x509_crt.h>

namespace quark::adapters::detail {

// Append every certificate of a concatenation of DER certificates (each self-delimiting) to `chain`.
// `mbedtls_x509_crt_parse_der` parses ONE certificate per call, so a multi-root bundle must be walked.
// Fails closed: false on any certificate that does not parse, or on empty input.
[[nodiscard]] inline bool parse_der_concat(mbedtls_x509_crt& chain, std::span<const std::byte> der) {
    if (der.empty()) return false;
    std::size_t off = 0;
    while (off < der.size()) {
        if (mbedtls_x509_crt_parse_der(&chain, reinterpret_cast<const unsigned char*>(der.data() + off),
                                       der.size() - off) != 0)
            return false;
        const mbedtls_x509_crt* last = &chain;
        while (last->next != nullptr) last = last->next;
        if (last->raw.len == 0) return false;
        off += last->raw.len;
    }
    return true;
}

}  // namespace quark::adapters::detail
