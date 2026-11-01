// Conformance of quark::adapters::SodiumAeadChaCha20Poly1305 to the Aead seam (aead.hpp), the same checks
// mbedtls_aead_check.cpp runs against the AES-GCM adapter: seal/open and seal_into/open_into round-trips (including an
// empty plaintext), the two forms agree byte for byte, and every tamper (ciphertext, tag, aad, nonce, key, truncation)
// makes open fail with the output left empty/untouched. Cross-check: the ciphertext equals a direct libsodium call with
// the documented nonce layout (4 zero bytes || LE64), so the adapter is the cipher it claims to be.
// Implements ADR-068 claim A1; 020-Security §2.
#include <array>
#include <cstdio>
#include <vector>

#include "pinned_test_util.hpp"
#include "quark/adapters/sodium/sodium_aead.hpp"

using namespace quark;
using namespace quark::adapters;
using quark::test::check;

namespace {
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> b;
    for (int x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    std::array<std::byte, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::byte>(i * 7 + 1);
    SodiumAeadChaCha20Poly1305 c{std::span<const std::byte, 32>(key)};
    check(c.tag_size() == 16, "tag size is 16", ok);

    const auto pt = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    const auto aad = bytes({0xAA, 0xBB, 0xCC});
    const std::uint64_t nonce = 0x8000000000000007ULL;  // lane bit set + seq 7, like secure_transport's nonce_for

    std::vector<std::byte> sealed;
    c.seal(nonce, aad, pt, sealed);
    check(sealed.size() == pt.size() + 16, "sealed = plaintext + tag", ok);

    std::vector<std::byte> opened;
    check(c.open(nonce, aad, sealed, opened) && opened == pt, "open recovers the plaintext", ok);

    // seal_into / open_into agree with seal / open.
    std::vector<std::byte> buf(pt.size() + 16);
    check(c.seal_into(nonce, aad, pt, buf) == buf.size() && buf == sealed, "seal_into == seal byte for byte", ok);
    std::vector<std::byte> out(pt.size());
    std::size_t out_len = 0;
    check(c.open_into(nonce, aad, buf, out, out_len) && out_len == pt.size() && out == pt, "open_into round-trips", ok);

    // Empty plaintext (an authenticated empty record) round-trips.
    {
        std::vector<std::byte> s2, o2;
        c.seal(1, aad, {}, s2);
        check(s2.size() == 16 && c.open(1, aad, s2, o2) && o2.empty(), "empty plaintext round-trips", ok);
        std::size_t n = 99;
        check(c.open_into(1, aad, s2, std::span<std::byte>{}, n) && n == 0, "open_into with an empty output span", ok);
    }

    // Cross-check against libsodium directly with the documented nonce layout.
    {
        unsigned char npub[12] = {0};
        for (int b = 0; b < 8; ++b) npub[4 + b] = static_cast<unsigned char>((nonce >> (8 * b)) & 0xFF);
        std::vector<unsigned char> ref(pt.size() + 16);
        unsigned long long clen = 0;
        crypto_aead_chacha20poly1305_ietf_encrypt(ref.data(), &clen, noise::uc(pt.data()), pt.size(), noise::uc(aad.data()),
                                                  aad.size(), nullptr, npub, noise::uc(key.data()));
        check(clen == sealed.size() && std::memcmp(ref.data(), sealed.data(), sealed.size()) == 0,
              "ciphertext equals libsodium's with nonce = 4 zero bytes || LE64", ok);
    }

    // Controls: each tamper must be rejected, and the output left empty.
    auto must_fail = [&](const char* what, std::uint64_t n, const std::vector<std::byte>& a, std::vector<std::byte> s) {
        std::vector<std::byte> o{std::byte{1}};
        const bool r = c.open(n, a, s, o);
        check(!r && o.empty(), what, ok);
    };
    {
        auto s = sealed;
        s[0] ^= std::byte{1};
        must_fail("CONTROL: a flipped ciphertext byte is rejected", nonce, aad, s);
        s = sealed;
        s.back() ^= std::byte{1};
        must_fail("CONTROL: a flipped tag byte is rejected", nonce, aad, s);
        auto a2 = aad;
        a2[0] ^= std::byte{1};
        must_fail("CONTROL: a different aad is rejected", nonce, a2, sealed);
        must_fail("CONTROL: a different nonce is rejected", nonce + 1, aad, sealed);
        must_fail("CONTROL: the other lane (top nonce bit) is rejected", nonce ^ 0x8000000000000000ULL, aad, sealed);
        s = sealed;
        s.pop_back();
        must_fail("CONTROL: a truncated record is rejected", nonce, aad, s);
        must_fail("CONTROL: a record shorter than a tag is rejected", nonce, aad, std::vector<std::byte>(15));
        std::array<std::byte, 32> key2 = key;
        key2[31] ^= std::byte{1};
        SodiumAeadChaCha20Poly1305 other{std::span<const std::byte, 32>(key2)};
        std::vector<std::byte> o;
        check(!other.open(nonce, aad, sealed, o), "CONTROL: a different key is rejected", ok);
        // open_into leaves out_len untouched on failure.
        std::size_t n = 12345;
        std::vector<std::byte> bad = sealed;
        bad[2] ^= std::byte{1};
        check(!c.open_into(nonce, aad, bad, out, n) && n == 12345, "CONTROL: open_into failure leaves out_len untouched", ok);
    }

    std::printf(ok ? "sodium_aead_check: OK\n" : "sodium_aead_check: FAILED\n");
    return ok ? 0 : 1;
}
