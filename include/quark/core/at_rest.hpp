// Implements 020-Security §5 (data at rest) — OPTIONAL envelope encryption for 012 persistence records
// (WAL events + snapshots). It composes cleanly with 016's encoding: the canonical tagged bytes are
// produced FIRST (encode_record — so schema evolution/migration is entirely unchanged), THEN sealed
// before hitting the disk record. The store still sees opaque bytes; only the OWNER (holding the data
// key from the Keyring) can open them.
//
// FENCING TOKEN UNDER THE AEAD TAG (020 §5 / 012 / 017): the fencing token (and the commit seq) are put
// in the AEAD ASSOCIATED DATA, so they are covered by the record's tag. A stale token therefore CANNOT
// be spliced onto fresh ciphertext — opening fresh ciphertext while presenting an OLD fence token yields
// a different AAD, the tag fails, and the record is rejected. (A control test proves the splice fails.)
//
// KEYRING SEAM (020 §5): the per-actor/per-shard DATA KEY is wrapped by a key-encryption key from a
// `Keyring` — a KMS, an OS keystore, or a static key from the `SecretSource`. Here the seam returns the
// data-key AEAD directly; KEK-wrapping / rotation / a real KMS adapter are DEFERRED behind it. The
// default `StaticKeyring` holds one process key (dev / single-tenant); it is NOT a substitute for a KMS.
//
// THE HONEST EXCEPTION (020): the AEAD is the seam (aead.hpp). `MockCipher` is NOT real crypto; a
// production build seals with a vetted AES-GCM/ChaCha20-Poly1305 adapter. At-rest encryption links that
// adapter ONLY when configured — a plaintext-at-rest store links zero crypto.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"  // FenceToken, SeqNo
#include "quark/detail/hash.hpp"

namespace quark {

// ============================================================================================
// The Keyring seam (020 §5). Returns the DATA-KEY AEAD for an actor/shard. A real implementation
// unwraps a per-actor data key with a KEK from a KMS / OS keystore; this seam hands back the ready
// cipher. Rotation + KEK-wrapping are DEFERRED behind it (020 Open questions: online key rotation).
// ============================================================================================
class Keyring {
public:
    virtual ~Keyring() = default;
    // The data-key cipher for `id`. Non-owning; valid for the store's lifetime.
    [[nodiscard]] virtual const Aead& data_key(ActorId id) noexcept = 0;
};

// The default single-key ring (dev / single-tenant): one process cipher for every actor. NOT a KMS.
class StaticKeyring final : public Keyring {
public:
    explicit StaticKeyring(const Aead& cipher) noexcept : cipher_(&cipher) {}
    [[nodiscard]] const Aead& data_key(ActorId) noexcept override { return *cipher_; }

private:
    const Aead* cipher_;
};

// The at-rest AAD: {fence, seq}. Both are integrity-relevant (012/017) and MUST be covered by the tag
// so neither can be swapped onto other ciphertext. Deterministic little-endian encoding.
[[nodiscard]] inline std::vector<std::byte> at_rest_aad(FenceToken fence, SeqNo seq) {
    std::vector<std::byte> aad;
    for (int b = 0; b < 8; ++b) aad.push_back(static_cast<std::byte>((fence.value >> (b * 8)) & 0xFF));
    for (int b = 0; b < 8; ++b) aad.push_back(static_cast<std::byte>((seq >> (b * 8)) & 0xFF));
    return aad;
}

// A per-record nonce: fold the record identity (fence, seq) into a unique value. Strictly-increasing
// seq guarantees uniqueness within an actor's log; fence separates activations.
[[nodiscard]] inline std::uint64_t at_rest_nonce(FenceToken fence, SeqNo seq) noexcept {
    return detail::splitmix64(detail::hash_combine(fence.value, seq));
}

// Seal already-canonical (016) bytes for disk. `canonical` is the output of encode_record (produced
// BEFORE this call, so evolution is unaffected). Returns {ciphertext || tag}; the fence token + seq are
// bound into the tag via the AAD. `cipher` is the data-key AEAD from the Keyring.
[[nodiscard]] inline std::vector<std::byte> seal_at_rest(const Aead& cipher, FenceToken fence, SeqNo seq,
                                                         std::span<const std::byte> canonical) {
    std::vector<std::byte> aad = at_rest_aad(fence, seq);
    std::vector<std::byte> out;
    cipher.seal(at_rest_nonce(fence, seq), std::span<const std::byte>(aad), canonical, out);
    return out;
}

// Open a sealed record, presenting the fence token + seq the reader believes are current. If the sealed
// bytes were sealed under a DIFFERENT fence (a stale-token splice) or a different seq, the AAD differs,
// the tag fails, and this returns an error — the stale-token splice is rejected. On success the
// recovered CANONICAL 016 bytes are returned for decode_record/read_migrated (schema evolution intact).
[[nodiscard]] inline result<std::vector<std::byte>> open_at_rest(const Aead& cipher, FenceToken fence,
                                                                 SeqNo seq,
                                                                 std::span<const std::byte> sealed) {
    std::vector<std::byte> aad = at_rest_aad(fence, seq);
    std::vector<std::byte> out;
    if (!cipher.open(at_rest_nonce(fence, seq), std::span<const std::byte>(aad), sealed, out))
        return fail(errc::serialization,
                    "at-rest AEAD open failed (tamper / stale fencing-token splice / wrong key)");
    return out;
}

}  // namespace quark
