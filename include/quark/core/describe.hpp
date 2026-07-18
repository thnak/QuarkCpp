// Implements 016-Serialization §"One description, reflection-free" — the single, reflection-free
// field description from which serialize/deserialize/fingerprint/evolution are all generated.
// Proven by ADR-016 (one QUARK_SERIALIZE describe → three Archive instantiations; constexpr
// FNV-1a fingerprint over the {(tag, wire_type)} set; no RTTI, no reflection).
//
// This header is the FOUNDATION layer: wire types + LEB128 cursors + the describe visitor
// contract + the fingerprint folder + the QUARK_SERIALIZE macro. The concrete codecs
// (canonical tagged TLV, tagless fast path, negotiation) live in wire.hpp; the durable/record
// surface (schema_version, migration, Serializer seam) lives in serialize.hpp.
//
// No <typeinfo>, no typeid, no dynamic_cast anywhere below — the codec is reflection-free
// (must build clean under -fno-rtti; the codec TU shows 0 RTTI symbols, ADR-016).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace quark {

// --- Wire types (016 §"Canonical tagged encoding") ------------------------------------------
// key = (tag << 3) | wire_type, matching protobuf's proven wire-type model (016 deliberately
// reuses it). Only the subset the core needs.
enum class WireType : std::uint8_t {
    Varint = 0,   // LEB128: ints, bools, enums
    Fixed64 = 1,  // f64 and fixed-width 64-bit
    Bytes = 2,    // length-delimited: strings, blobs, vectors, nested described types
    Fixed32 = 5,  // f32 and fixed-width 32-bit
};

namespace detail {

// --- Element category detection (drives per-type wire encoding) ------------------------------
template <class T>
struct is_std_vector : std::false_type {};
template <class U, class A>
struct is_std_vector<std::vector<U, A>> : std::true_type {};
template <class T>
inline constexpr bool is_std_vector_v = is_std_vector<std::remove_cvref_t<T>>::value;

template <class T>
struct is_std_string : std::false_type {};
template <class C, class Tr, class A>
struct is_std_string<std::basic_string<C, Tr, A>> : std::true_type {};
template <class T>
inline constexpr bool is_std_string_v = is_std_string<std::remove_cvref_t<T>>::value;

}  // namespace detail

// The wire type a member serializes to — a pure function of its static type (never its value),
// which is exactly what makes the fingerprint computable without touching any instance data.
template <class M>
consteval WireType wire_type_of() noexcept {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>) {
        return WireType::Varint;
    } else if constexpr (std::is_enum_v<U>) {
        return WireType::Varint;
    } else if constexpr (std::is_same_v<U, double>) {
        return WireType::Fixed64;
    } else if constexpr (std::is_same_v<U, float>) {
        return WireType::Fixed32;
    } else if constexpr (std::is_integral_v<U>) {
        return WireType::Varint;
    } else {
        // strings, blobs, vectors, and nested described types are all length-delimited.
        return WireType::Bytes;
    }
}

namespace detail {

// --- Bounds-checked byte writer (canonical/durable path — handles hostile input) -------------
// The canonical tagged codec is bounds-checked because it parses arbitrary durable bytes
// (snapshots/event logs that may be truncated or malicious). The tagless fast path is
// deliberately UNCHECKED and lives in wire.hpp — that asymmetry is what makes the connect-time
// negotiation load-bearing (ADR-016 controls 4/5).
class ByteWriter {
public:
    ByteWriter(std::byte* p, std::size_t n) noexcept : begin_(p), cur_(p), end_(p + n) {}

    bool put_byte(std::byte b) noexcept {
        if (cur_ >= end_) {
            ok_ = false;
            return false;
        }
        *cur_++ = b;
        return true;
    }
    bool put_bytes(const void* src, std::size_t n) noexcept {
        if (n > static_cast<std::size_t>(end_ - cur_)) {
            ok_ = false;
            return false;
        }
        std::memcpy(cur_, src, n);
        cur_ += n;
        return true;
    }
    [[nodiscard]] std::size_t written() const noexcept {
        return static_cast<std::size_t>(cur_ - begin_);
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    std::byte* begin_;
    std::byte* cur_;
    std::byte* end_;
    bool ok_ = true;
};

// --- Bounds-checked byte reader --------------------------------------------------------------
class ByteReader {
public:
    ByteReader(const std::byte* p, std::size_t n) noexcept : cur_(p), end_(p + n) {}

    bool get_byte(std::byte& b) noexcept {
        if (cur_ >= end_) {
            ok_ = false;
            return false;
        }
        b = *cur_++;
        return true;
    }
    bool get_bytes(void* dst, std::size_t n) noexcept {
        if (n > static_cast<std::size_t>(end_ - cur_)) {
            ok_ = false;
            return false;
        }
        std::memcpy(dst, cur_, n);
        cur_ += n;
        return true;
    }
    bool skip(std::size_t n) noexcept {
        if (n > static_cast<std::size_t>(end_ - cur_)) {
            ok_ = false;
            return false;
        }
        cur_ += n;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(end_ - cur_);
    }
    [[nodiscard]] const std::byte* cursor() const noexcept { return cur_; }
    [[nodiscard]] bool at_end() const noexcept { return cur_ >= end_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    const std::byte* cur_;
    const std::byte* end_;
    bool ok_ = true;
};

// --- LEB128 varints (016 §"Canonical tagged encoding") ---------------------------------------
inline bool put_uleb(ByteWriter& w, std::uint64_t v) noexcept {
    for (;;) {
        auto low = static_cast<std::uint8_t>(v & 0x7Fu);
        v >>= 7;
        if (v != 0) {
            if (!w.put_byte(static_cast<std::byte>(low | 0x80u))) return false;
        } else {
            return w.put_byte(static_cast<std::byte>(low));
        }
    }
}

inline bool get_uleb(ByteReader& r, std::uint64_t& out) noexcept {
    std::uint64_t result = 0;
    for (int i = 0; i < 10; ++i) {
        std::byte b{};
        if (!r.get_byte(b)) return false;
        const auto byte = std::to_integer<std::uint8_t>(b);
        result |= (static_cast<std::uint64_t>(byte & 0x7Fu) << (7 * i));
        if ((byte & 0x80u) == 0) {
            out = result;
            return true;
        }
    }
    return false;  // overlong / corrupt varint
}

// ZigZag maps signed ints to unsigned so small-magnitude negatives stay short under LEB128.
inline std::uint64_t zigzag(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}
inline std::int64_t unzigzag(std::uint64_t u) noexcept {
    return static_cast<std::int64_t>((u >> 1) ^ (~(u & 1u) + 1u));
}

// --- Fixed-width little-endian encode (canonical byte order is LE; 016) ----------------------
template <class UInt>
inline bool put_fixed_le(ByteWriter& w, UInt value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    std::byte tmp[sizeof(UInt)];
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        tmp[i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    return w.put_bytes(tmp, sizeof(UInt));
}
template <class UInt>
inline bool get_fixed_le(ByteReader& r, UInt& value) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    std::byte tmp[sizeof(UInt)];
    if (!r.get_bytes(tmp, sizeof(UInt))) return false;
    UInt v = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        v |= static_cast<UInt>(static_cast<UInt>(std::to_integer<std::uint8_t>(tmp[i])) << (8 * i));
    }
    value = v;
    return true;
}

}  // namespace detail

// --- The describe-visitor probe + the Described concept --------------------------------------
// A type is Described iff a quark_describe(Ar&, T&) visitor exists for it (emitted by
// QUARK_SERIALIZE). Using a non-described type in a durable/wire context is a Validation error
// (008 seam — see serialize.hpp validate_serializable()).
namespace detail {
struct DescribeProbe {
    template <class M>
    constexpr void field(std::uint32_t, M&) noexcept {}
};
}  // namespace detail

template <class T>
concept Described = requires(detail::DescribeProbe& p, T& v) { quark_describe(p, v); };

// --- Constexpr fingerprint folder (016 §"Schema version and fingerprint") --------------------
// FNV-1a over the ordered {(tag, wire_type)} set. Order-sensitive on purpose: the fingerprint
// gates the tagless fast path, which bulk-copies fields in declared order, so two peers whose
// fields differ in order MUST get distinct fingerprints and fall back to tagged (ADR-016).
class FingerprintFolder {
public:
    static constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
    static constexpr std::uint64_t fnv_prime = 1099511628211ULL;

    template <class M>
    constexpr void field(std::uint32_t tag, M&) noexcept {
        // Fold the tag (4 bytes, LE) then the wire type — never the member value.
        for (int i = 0; i < 4; ++i) {
            fold(static_cast<std::uint8_t>(tag >> (8 * i)));
        }
        fold(static_cast<std::uint8_t>(wire_type_of<M>()));
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return hash_; }

private:
    constexpr void fold(std::uint8_t byte) noexcept {
        hash_ ^= byte;
        hash_ *= fnv_prime;
    }
    std::uint64_t hash_ = fnv_offset;
};

// The compile-time fingerprint of a described type. Requires the type be default-constructible
// in a constant expression (satisfied by PODs and by std::string/std::vector members, which are
// constexpr-empty-constructible in C++20/23). The folder never reads member values.
template <Described T>
constexpr std::uint64_t compute_fingerprint() noexcept {
    FingerprintFolder folder;
    T sample{};
    quark_describe(folder, sample);
    return folder.value();
}

template <Described T>
inline constexpr std::uint64_t fingerprint_v = compute_fingerprint<T>();

}  // namespace quark

// ============================================================================================
// QUARK_SERIALIZE(Type, (tag, member)...) — the one reflection-free description.
//
// Expands to a single templated visitor:
//     template <class Ar> constexpr void quark_describe(Ar& ar, Type& v) {
//         ar.field(1u, v.id); ar.field(2u, v.customer); ...
//     }
// found by ADL. `Ar` is instantiated as the tagged writer/reader, the tagless writer/reader,
// the fingerprint folder, and the size counter — the SAME field list drives all of them.
// ============================================================================================

// Apply the field macro to one `(tag, member)` pair. `QUARK_APPLY_FIELD_ (1, id)` re-parses as
// a macro call `QUARK_APPLY_FIELD_(1, id)`.
#define QUARK_APPLY_FIELD(pair) QUARK_APPLY_FIELD_ pair
#define QUARK_APPLY_FIELD_(tag, member) ar.field(static_cast<std::uint32_t>(tag), v.member);

// Variadic FOR_EACH over up to 24 `(tag, member)` pairs (gcc/clang; the QUARK_FE_EXPAND wrapper
// also keeps it correct under MSVC's non-conforming preprocessor).
#define QUARK_FE_EXPAND(x) x
#define QUARK_FE_0(WHAT)
#define QUARK_FE_1(WHAT, X) WHAT(X)
#define QUARK_FE_2(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_1(WHAT, __VA_ARGS__))
#define QUARK_FE_3(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_2(WHAT, __VA_ARGS__))
#define QUARK_FE_4(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_3(WHAT, __VA_ARGS__))
#define QUARK_FE_5(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_4(WHAT, __VA_ARGS__))
#define QUARK_FE_6(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_5(WHAT, __VA_ARGS__))
#define QUARK_FE_7(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_6(WHAT, __VA_ARGS__))
#define QUARK_FE_8(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_7(WHAT, __VA_ARGS__))
#define QUARK_FE_9(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_8(WHAT, __VA_ARGS__))
#define QUARK_FE_10(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_9(WHAT, __VA_ARGS__))
#define QUARK_FE_11(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_10(WHAT, __VA_ARGS__))
#define QUARK_FE_12(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_11(WHAT, __VA_ARGS__))
#define QUARK_FE_13(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_12(WHAT, __VA_ARGS__))
#define QUARK_FE_14(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_13(WHAT, __VA_ARGS__))
#define QUARK_FE_15(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_14(WHAT, __VA_ARGS__))
#define QUARK_FE_16(WHAT, X, ...) WHAT(X) QUARK_FE_EXPAND(QUARK_FE_15(WHAT, __VA_ARGS__))

#define QUARK_FE_GET(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, \
                     NAME, ...)                                                             \
    NAME
#define QUARK_FOR_EACH(WHAT, ...)                                                               \
    QUARK_FE_EXPAND(QUARK_FE_GET(__VA_ARGS__, QUARK_FE_16, QUARK_FE_15, QUARK_FE_14,            \
                                 QUARK_FE_13, QUARK_FE_12, QUARK_FE_11, QUARK_FE_10, QUARK_FE_9, \
                                 QUARK_FE_8, QUARK_FE_7, QUARK_FE_6, QUARK_FE_5, QUARK_FE_4,     \
                                 QUARK_FE_3, QUARK_FE_2, QUARK_FE_1)(WHAT, __VA_ARGS__))

#define QUARK_SERIALIZE(Type, ...)                             \
    template <class Ar>                                        \
    constexpr void quark_describe(Ar& ar, Type& v) {           \
        (void)ar;                                              \
        (void)v;                                               \
        QUARK_FOR_EACH(QUARK_APPLY_FIELD, __VA_ARGS__)         \
    }
