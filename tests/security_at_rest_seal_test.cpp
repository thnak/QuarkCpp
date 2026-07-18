// Tests 020-Security §5 — at-rest envelope encryption composes with 016 encoding: canonical tagged
// bytes are produced FIRST (encode_record, so schema evolution is untouched), THEN sealed. seal/open
// round-trips and the decoded state is identical. The FENCING TOKEN is covered by the AEAD tag (put in
// the associated data), so a stale token CANNOT be spliced onto fresh ciphertext.
//
// CONTROL (adversarial, must FIRE): take FRESH ciphertext (sealed under fence=5) and attempt to open it
// while presenting a STALE fence token (=3) — the AAD differs, the tag fails, open is REJECTED. A second
// control tampers a ciphertext byte and shows open likewise fails. Uses the MOCK cipher (aead.hpp) — NOT
// real crypto; it only exercises the framing.
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/at_rest.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {
struct AccountSnap {
    std::uint64_t id;
    std::uint64_t balance;
    std::uint32_t version;
};
QUARK_SERIALIZE(AccountSnap, (1, id), (2, balance), (3, version));

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    MockCipher cipher(0x5EC0DE);  // NOT real crypto — mock, framing only
    const AccountSnap state{0xA11CE, 4200, 3};
    const TypeKey tk{fingerprint_v<AccountSnap>};

    // (1) Produce the canonical 016 bytes FIRST — this is exactly what the store would persist plaintext.
    std::vector<std::byte> canonical(256);
    auto enc = encode_record(state, tk, canonical.data(), canonical.size());
    check(enc.has_value(), "encode_record produced canonical tagged bytes", ok);
    canonical.resize(*enc);

    const FenceToken fresh{5};
    const SeqNo seq{42};

    // (2) Seal AFTER encoding; the fence + seq are bound into the tag.
    std::vector<std::byte> sealed = seal_at_rest(cipher, fresh, seq, std::span<const std::byte>(canonical));
    check(sealed.size() > canonical.size(), "sealed record carries ciphertext + tag (larger)", ok);
    check(sealed.size() != canonical.size() &&
              std::memcmp(sealed.data(), canonical.data(), canonical.size()) != 0,
          "sealed bytes are not the plaintext canonical bytes", ok);

    // (3) Round-trip: open with the correct fence+seq, decode, compare.
    {
        auto opened = open_at_rest(cipher, fresh, seq, std::span<const std::byte>(sealed));
        check(opened.has_value(), "open with the correct fence+seq succeeds", ok);
        check(opened->size() == canonical.size() &&
                  std::memcmp(opened->data(), canonical.data(), canonical.size()) == 0,
              "opened bytes equal the original canonical bytes", ok);
        auto decoded = decode_record<AccountSnap>(opened->data(), opened->size());
        check(decoded.has_value() && decoded->id == state.id && decoded->balance == state.balance &&
                  decoded->version == state.version,
              "decoded state round-trips through seal/open (schema evolution intact)", ok);
    }

    // (4) CONTROL: STALE-TOKEN SPLICE. Present fence=3 against ciphertext sealed under fence=5. -------
    {
        const FenceToken stale{3};
        auto spliced = open_at_rest(cipher, stale, seq, std::span<const std::byte>(sealed));
        check(!spliced.has_value() && spliced.error().code == errc::serialization,
              "CONTROL: stale fencing-token splice onto fresh ciphertext is REJECTED (tag fails)", ok);
    }
    // A wrong seq is likewise rejected (seq is also in the AAD).
    {
        auto wrong_seq = open_at_rest(cipher, fresh, SeqNo{43}, std::span<const std::byte>(sealed));
        check(!wrong_seq.has_value(), "CONTROL: a mismatched seq is rejected (AAD-bound)", ok);
    }

    // (5) CONTROL: ciphertext tamper. Flip one payload byte ⇒ open fails. --------------------------
    {
        std::vector<std::byte> tampered = sealed;
        tampered[0] ^= std::byte{0xFF};
        auto opened = open_at_rest(cipher, fresh, seq, std::span<const std::byte>(tampered));
        check(!opened.has_value(), "CONTROL: a tampered ciphertext byte is rejected", ok);
    }

    // (6) A record sealed under fence=6 opens ONLY with fence=6 — proving the binding is specific, not a
    //     blanket rejection. This makes the splice control non-vacuous.
    {
        const FenceToken f6{6};
        std::vector<std::byte> s6 = seal_at_rest(cipher, f6, seq, std::span<const std::byte>(canonical));
        check(open_at_rest(cipher, f6, seq, std::span<const std::byte>(s6)).has_value(),
              "sealed-under-6 opens under 6", ok);
        check(!open_at_rest(cipher, fresh, seq, std::span<const std::byte>(s6)).has_value(),
              "sealed-under-6 does NOT open under 5 (fence is specific)", ok);
    }

    std::printf("security_at_rest_seal_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
