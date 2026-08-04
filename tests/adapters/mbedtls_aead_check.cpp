// Conformance check for ADR-040's real cipher: `quark::adapters::MbedtlsAeadGcm` (AES-128-GCM) against
// the same Aead contract MockCipher satisfies (aead.hpp) — seal/open round-trip, tamper/wrong-aad/
// wrong-key rejection, and the fixed-buffer seal_into/open_into forms. Only built when
// QUARK_WITH_MBEDTLS=ON (cmake/QuarkSecurityAdapters.cmake).
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quark/adapters/mbedtls/mbedtls_aead.hpp"

using namespace quark;
using namespace quark::adapters;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

[[nodiscard]] std::array<std::byte, MbedtlsAeadGcm::kKeyBytes> make_key(unsigned char seed) {
    std::array<std::byte, MbedtlsAeadGcm::kKeyBytes> k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(seed + i);
    return k;
}
}  // namespace

int main() {
    bool ok = true;
    const auto key = make_key(0x11);
    MbedtlsAeadGcm cipher{std::span<const std::byte, MbedtlsAeadGcm::kKeyBytes>(key)};

    const std::vector<std::byte> aad = {std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> plaintext = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
                                              std::byte{'l'}, std::byte{'o'}};

    // --- vector-based seal/open round-trip ------------------------------------------------------
    std::vector<std::byte> sealed;
    cipher.seal(42, aad, plaintext, sealed);
    check(sealed.size() == plaintext.size() + MbedtlsAeadGcm::kTagBytes, "sealed size == plaintext + tag", ok);
    check(sealed != plaintext, "ciphertext differs from plaintext", ok);

    std::vector<std::byte> opened;
    check(cipher.open(42, aad, sealed, opened), "open() succeeds with correct nonce/aad", ok);
    check(opened == plaintext, "opened plaintext matches original", ok);

    // --- CONTROL: tampered ciphertext byte rejected -----------------------------------------------
    {
        std::vector<std::byte> tampered = sealed;
        tampered[0] ^= std::byte{0xFF};
        std::vector<std::byte> out;
        check(!cipher.open(42, aad, tampered, out), "CONTROL: tampered ciphertext rejected", ok);
        check(out.empty(), "CONTROL: rejected open leaves out empty", ok);
    }

    // --- CONTROL: wrong AAD rejected ---------------------------------------------------------------
    {
        std::vector<std::byte> wrong_aad = {std::byte{9}, std::byte{9}, std::byte{9}};
        std::vector<std::byte> out;
        check(!cipher.open(42, wrong_aad, sealed, out), "CONTROL: wrong AAD rejected", ok);
    }

    // --- CONTROL: wrong key rejected -----------------------------------------------------------
    {
        const auto other_key = make_key(0x99);
        MbedtlsAeadGcm other_cipher{std::span<const std::byte, MbedtlsAeadGcm::kKeyBytes>(other_key)};
        std::vector<std::byte> out;
        check(!other_cipher.open(42, aad, sealed, out), "CONTROL: wrong key rejected", ok);
    }

    // --- CONTROL: wrong nonce rejected ------------------------------------------------------------
    {
        std::vector<std::byte> out;
        check(!cipher.open(43, aad, sealed, out), "CONTROL: wrong nonce rejected", ok);
    }

    // --- fixed-buffer seal_into/open_into (non-allocating forms) --------------------------------
    {
        std::array<std::byte, 64> buf{};
        const std::size_t written =
            cipher.seal_into(7, aad, plaintext, std::span<std::byte>(buf.data(), buf.size()));
        check(written == plaintext.size() + MbedtlsAeadGcm::kTagBytes, "seal_into wrote plaintext+tag bytes", ok);

        std::array<std::byte, 64> out{};
        std::size_t out_len = 0;
        check(cipher.open_into(7, aad, std::span<const std::byte>(buf.data(), written),
                               std::span<std::byte>(out.data(), out.size()), out_len),
              "open_into succeeds", ok);
        check(out_len == plaintext.size(), "open_into recovered the right length", ok);
        check(std::memcmp(out.data(), plaintext.data(), plaintext.size()) == 0,
              "open_into recovered the right bytes", ok);
    }

    check(cipher.tag_size() == MbedtlsAeadGcm::kTagBytes, "tag_size() reports 16", ok);

    std::printf("mbedtls_aead_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
