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
#include <optional>
#include <span>
#include <utility>
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
    [[nodiscard]] virtual const Aead& data_key(ActorId id) const noexcept = 0;
};

// The default single-key ring (dev / single-tenant): one process cipher for every actor. NOT a KMS.
class StaticKeyring final : public Keyring {
public:
    explicit StaticKeyring(const Aead& cipher) noexcept : cipher_(&cipher) {}
    [[nodiscard]] const Aead& data_key(ActorId) const noexcept override { return *cipher_; }

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

// ============================================================================================
// Store-level context (P4): a durable record also needs the ACTOR and the RECORD KIND in
// the AAD, or a record could be spliced between actors that share a key (StaticKeyring) or between
// a snapshot and an event with the same seq. `fence` is bound for snapshots (the record carries it);
// events bind actor+seq (the store's event record has no fence). Nonce = an injective hash of the
// same context, so a record can only be opened at the position it was sealed for.
// ============================================================================================
enum class AtRestKind : std::uint8_t { Snapshot = 1, Event = 2 };

struct AtRestContext {
    ActorId actor{};
    AtRestKind kind = AtRestKind::Event;
    SeqNo seq = 0;
    FenceToken fence{};  // snapshot only; events are position-bound by actor+seq
};

[[nodiscard]] inline std::vector<std::byte> at_rest_aad(const AtRestContext& ctx) {
    std::vector<std::byte> aad;
    const auto put = [&aad](std::uint64_t v) {
        for (int b = 0; b < 8; ++b) aad.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
    };
    put(ctx.actor.type.value);
    put(ctx.actor.key);
    put(static_cast<std::uint64_t>(ctx.kind));
    put(ctx.seq);
    put(ctx.fence.value);
    return aad;
}

[[nodiscard]] inline std::uint64_t at_rest_nonce(const AtRestContext& ctx) noexcept {
    std::uint64_t h = detail::hash_combine(ctx.actor.type.value, ctx.actor.key);
    h = detail::hash_combine(h, static_cast<std::uint64_t>(ctx.kind));
    h = detail::hash_combine(h, ctx.seq);
    h = detail::hash_combine(h, ctx.fence.value);
    return detail::splitmix64(h);
}

[[nodiscard]] inline std::vector<std::byte> seal_at_rest(const Aead& cipher, const AtRestContext& ctx,
                                                         std::span<const std::byte> canonical) {
    const std::vector<std::byte> aad = at_rest_aad(ctx);
    std::vector<std::byte> out;
    cipher.seal(at_rest_nonce(ctx), std::span<const std::byte>(aad), canonical, out);
    return out;
}

[[nodiscard]] inline result<std::vector<std::byte>> open_at_rest(const Aead& cipher,
                                                                 const AtRestContext& ctx,
                                                                 std::span<const std::byte> sealed) {
    const std::vector<std::byte> aad = at_rest_aad(ctx);
    std::vector<std::byte> out;
    if (!cipher.open(at_rest_nonce(ctx), std::span<const std::byte>(aad), sealed, out))
        return fail(errc::serialization,
                    "at-rest AEAD open failed (tamper / wrong actor/seq/fence / wrong key)");
    return out;
}

// ============================================================================================
// EncryptedStore<S> — a Store decorator that seals every durable payload at the record layer.
// Fencing, strict seq, atomic batch and snapshot<=tail are delegated UNCHANGED; only the opaque
// `record` bytes are transformed. Canonical 016 bytes are produced by the layer above first, so
// schema evolution/migration is untouched. Wrap FileStore/SqliteStore/RocksStore the same way.
// The default (unwrapped) adapters remain plaintext; see PersistenceAdapters.md for the decision.
// ============================================================================================
template <class S>
class EncryptedStore {
public:
    EncryptedStore(S& inner, const Keyring& keys) noexcept : inner_(&inner), keys_(&keys) {}
    EncryptedStore(const EncryptedStore&) = delete;
    EncryptedStore& operator=(const EncryptedStore&) = delete;

    [[nodiscard]] S& inner() noexcept { return *inner_; }
    [[nodiscard]] const S& inner() const noexcept { return *inner_; }

    [[nodiscard]] FenceToken acquire_fence(ActorId id) { return inner_->acquire_fence(id); }
    [[nodiscard]] SeqNo last_seq(ActorId id) const noexcept { return inner_->last_seq(id); }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const {
        auto raw = inner_->load_snapshot(id);
        if (!raw) return std::unexpected(raw.error());
        if (!raw->has_value()) return std::optional<SnapshotRecord>{};
        const SnapshotRecord& rec = **raw;
        const AtRestContext ctx{id, AtRestKind::Snapshot, rec.through_seq, rec.fence};
        auto opened = open_at_rest(keys_->data_key(id), ctx, rec.record);
        if (!opened) return std::unexpected(opened.error());
        SnapshotRecord out = rec;
        out.record = std::move(*opened);
        return std::optional<SnapshotRecord>{std::move(out)};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok,
                                             const SnapshotRecord& snap) {
        const AtRestContext ctx{id, AtRestKind::Snapshot, snap.through_seq, tok};
        SnapshotRecord sealed = snap;
        sealed.fence = tok;
        sealed.record = seal_at_rest(keys_->data_key(id), ctx, snap.record);
        return inner_->save_snapshot(id, tok, sealed);
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        const AtRestContext ctx{id, AtRestKind::Event, seq, FenceToken{}};
        const std::vector<std::byte> sealed = seal_at_rest(keys_->data_key(id), ctx, bytes);
        return inner_->append(id, tok, seq, std::span<const std::byte>(sealed));
    }

    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        std::vector<EventRecord> sealed;
        sealed.reserve(batch.size());
        for (const EventRecord& r : batch) {
            const AtRestContext ctx{id, AtRestKind::Event, r.seq, FenceToken{}};
            sealed.push_back(EventRecord{r.seq, seal_at_rest(keys_->data_key(id), ctx, r.record)});
        }
        return inner_->append_batch(id, tok, std::span<const EventRecord>(sealed));
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) const {
        auto raw = inner_->read_log(id, from);
        if (!raw) return std::unexpected(raw.error());
        EventCursor out;
        out.entries.reserve(raw->entries.size());
        for (const EventRecord& r : raw->entries) {
            const AtRestContext ctx{id, AtRestKind::Event, r.seq, FenceToken{}};
            auto opened = open_at_rest(keys_->data_key(id), ctx, r.record);
            if (!opened) return std::unexpected(opened.error());
            out.entries.push_back(EventRecord{r.seq, std::move(*opened)});
        }
        return out;
    }

private:
    S* inner_;
    const Keyring* keys_;
};

static_assert(Store<EncryptedStore<InMemoryStore>>,
              "EncryptedStore must satisfy the 012 Store concept");

}  // namespace quark
