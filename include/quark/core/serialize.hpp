// Implements 016-Serialization §"Schema version and fingerprint", §"Migrations", and the
// durable record surface — the top-level API over the describe/wire codecs. Proven by ADR-016
// (v1→v2→v3 migration chain on read; durable record header {type_key, schema_version};
// non-described-type-in-durable-context is a Validation error).
//
//   * SchemaVersion<T>            — the developer-bumped breaking-change version (016).
//   * QUARK_MIGRATE(From,To,fn)   — a registered upcaster; the chain From→…→Current is applied
//                                   on read of an older durable record (016 §"Migrations").
//   * DurableRecord {type_key, schema_version} + encode_record/decode_record (canonical tagged
//     ONLY — durable data NEVER uses the tagless fast path, 016).
//   * validate_serializable<T>()  — non-described type in a durable/wire context ⇒ Validation
//     error (008 seam).
//   * Serializer concept          — the seam behind which protobuf/FlatBuffers/Cap'n Proto plug
//     in as OPTIONAL adapters; the core TLV codec models it (016 §"Dependencies").
//
// Reflection-free: no typeid/dynamic_cast; the migration chain is compile-time typed recursion,
// no std::any / std::function type erasure. Builds clean under -fno-rtti.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "quark/core/describe.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/wire.hpp"

namespace quark {

// --- Schema version (016) --------------------------------------------------------------------
// The developer-bumped breaking-change version. Distinct from the fingerprint: the fingerprint
// negotiates wire layout (fast, automatic); schema_version headers every durable record and
// drives the migration chain on read. Default 1; override with QUARK_SCHEMA_VERSION.
template <class T>
struct SchemaVersion {
    static constexpr std::uint32_t value = 1;
};
template <class T>
inline constexpr std::uint32_t schema_version_v = SchemaVersion<T>::value;

// --- Migration chain (016 §"Migrations") -----------------------------------------------------
// A breaking evolution is handled by a registered upcaster From→To. QUARK_MIGRATE specialises
// Migration<From> with the next type and the transform. On read, read_migrated walks the chain
// From→…→Current (compile-time typed; no type erasure, no RTTI).
template <class From>
struct Migration;  // primary left undefined; QUARK_MIGRATE specialises it.

// Fold a decoded old value forward to `Target` by applying successive registered upcasters.
template <class Target, class From>
Target migrate_to(const From& value) {
    if constexpr (std::is_same_v<Target, From>) {
        return value;
    } else {
        return migrate_to<Target>(Migration<From>::upcast(value));
    }
}

// --- The durable record header (012): every snapshot/event record is prefixed ----------------
struct DurableRecord {
    TypeKey type{};
    std::uint32_t schema_version = 0;
};

// --- Validation seam (008): a non-described type in a durable/wire context is a Validation error
template <class T>
[[nodiscard]] constexpr result<void> validate_serializable() noexcept {
    if constexpr (Described<T>) {
        return {};
    } else {
        return fail(errc::validation, "type has no QUARK_SERIALIZE description");
    }
}

// --- Durable encode: header {type_key, schema_version} + canonical tagged body (NEVER tagless)
template <Described T>
[[nodiscard]] result<std::size_t> encode_record(const T& obj, TypeKey type, std::byte* buf,
                                                std::size_t cap) {
    detail::ByteWriter w(buf, cap);
    // Header is fixed-width little-endian so it is readable without knowing the body schema.
    if (!detail::put_fixed_le<std::uint64_t>(w, type.value)) {
        return fail(errc::serialization, "record buffer too small (header)");
    }
    if (!detail::put_fixed_le<std::uint32_t>(w, schema_version_v<T>)) {
        return fail(errc::serialization, "record buffer too small (header)");
    }
    detail::TaggedWriter aw{w};
    quark_describe(aw, const_cast<T&>(obj));
    if (!w.ok()) return fail(errc::serialization, "record buffer too small (body)");
    return w.written();
}

// Read just the header (type_key + schema_version) — cheap dispatch before choosing the body
// type / migration entry point (012/008 drift detection).
[[nodiscard]] inline result<DurableRecord> peek_record_header(const std::byte* buf,
                                                              std::size_t len) {
    detail::ByteReader r(buf, len);
    DurableRecord hdr{};
    std::uint64_t tk;
    if (!detail::get_fixed_le<std::uint64_t>(r, tk)) {
        return fail(errc::serialization, "truncated record header");
    }
    if (!detail::get_fixed_le<std::uint32_t>(r, hdr.schema_version)) {
        return fail(errc::serialization, "truncated record header");
    }
    hdr.type = TypeKey{tk};
    return hdr;
}

inline constexpr std::size_t durable_header_size = sizeof(std::uint64_t) + sizeof(std::uint32_t);

// Decode a record whose body is exactly the current schema of `T` (no migration needed). The
// stored header `type_key` MUST equal the durable key for `T` (its 016 fingerprint); a mismatch
// means the bytes are some OTHER type and we refuse to decode them as `T` — defense-in-depth against
// silently reinterpreting a record with a layout-compatible but semantically-different type.
template <Described T>
[[nodiscard]] result<T> decode_record(const std::byte* buf, std::size_t len) {
    auto hdr = peek_record_header(buf, len);
    if (!hdr) return std::unexpected(hdr.error());
    if (hdr->type != TypeKey{fingerprint_v<T>})
        return fail(errc::serialization, "durable record type_key mismatch (decoding as the wrong type)");
    T out{};
    auto rc = decode_tagged(buf + durable_header_size, len - durable_header_size, out);
    if (!rc) return std::unexpected(rc.error());
    return out;
}

// --- Migrated read: apply the chain v→v+1→…→current on read (016 §"Migrations") --------------
// Walks the compile-time type chain starting from `Head` (the oldest version). When the record's
// schema_version matches a link, that link's type is decoded and folded forward to `Current`.
// Event logs rely on this: old event shapes stay readable indefinitely.
template <class Current, class Head>
[[nodiscard]] result<Current> read_from_chain(TypeKey type_key, std::uint32_t version,
                                              const std::byte* body, std::size_t body_len) {
    if (schema_version_v<Head> == version) {
        // Defense-in-depth (012/016): a record CLAIMING this schema version must carry the durable
        // key (fingerprint) of the type at that version, or the bytes are a different type and we
        // refuse to decode+migrate them as `Head`.
        if (type_key != TypeKey{fingerprint_v<Head>})
            return fail(errc::serialization, "durable record type_key mismatch at its schema version");
        Head old{};
        auto rc = decode_tagged(body, body_len, old);
        if (!rc) return std::unexpected(rc.error());
        return migrate_to<Current>(old);
    }
    if constexpr (!std::is_same_v<Head, Current>) {
        return read_from_chain<Current, typename Migration<Head>::to>(type_key, version, body, body_len);
    } else {
        return fail(errc::serialization, "unknown schema version (not in migration chain)");
    }
}

// Read a durable record of any version in the chain [Oldest … Current] and return it upcast to
// `Current`. `Current` must be the newest type; `Oldest` the chain head.
template <class Current, class Oldest>
[[nodiscard]] result<Current> read_migrated(const std::byte* buf, std::size_t len) {
    auto hdr = peek_record_header(buf, len);
    if (!hdr) return std::unexpected(hdr.error());
    return read_from_chain<Current, Oldest>(hdr->type, hdr->schema_version, buf + durable_header_size,
                                            len - durable_header_size);
}

// --- Serializer seam (016 §"Dependencies") ---------------------------------------------------
// protobuf/FlatBuffers/Cap'n Proto are OPTIONAL adapters that model this concept and plug in
// behind it (010). The core self-contained TLV codec (below) models it too; nothing here links
// a heavy dependency. This is the extension point, not a core requirement.
template <class S, class T>
concept Serializer = requires(const S& s, const T& value, std::byte* buf, std::size_t cap,
                              const std::byte* in, std::size_t len, T& out) {
    { s.encode(value, buf, cap) } -> std::same_as<result<std::size_t>>;
    { s.decode(in, len, out) } -> std::same_as<result<void>>;
};

// The built-in canonical codec as a Serializer — the default adapter, std-only, no dependency.
struct CanonicalTlvSerializer {
    template <Described T>
    [[nodiscard]] result<std::size_t> encode(const T& value, std::byte* buf,
                                             std::size_t cap) const {
        return encode_tagged(value, buf, cap);
    }
    template <Described T>
    [[nodiscard]] result<void> decode(const std::byte* in, std::size_t len, T& out) const {
        return decode_tagged(in, len, out);
    }
};

}  // namespace quark

// --- QUARK_SCHEMA_VERSION / QUARK_MIGRATE (namespace-scope registration macros) ---------------
//
//   QUARK_SCHEMA_VERSION(OrderV2, 2);
//   QUARK_MIGRATE(OrderV1, OrderV2, [](const OrderV1& o) { return OrderV2{...}; });
//
// 016 spells the migrate example as QUARK_MIGRATE(Order, from_v, to_v, lambda); we key the chain
// on the concrete From/To *types* the lambda already names (OrderV1→OrderV2) — the versions live
// in SchemaVersion<T>, so the chain is fully typed and reflection-free. Use at global scope.
#define QUARK_SCHEMA_VERSION(Type, N)             \
    template <>                                    \
    struct quark::SchemaVersion<Type> {            \
        static constexpr std::uint32_t value = (N); \
    }

#define QUARK_MIGRATE(FromType, ToType, ...)                            \
    template <>                                                         \
    struct quark::Migration<FromType> {                                \
        using to = ToType;                                             \
        static ToType upcast(const FromType& x) { return (__VA_ARGS__)(x); } \
    }
