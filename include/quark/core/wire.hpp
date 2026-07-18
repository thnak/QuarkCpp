// Implements 016-Serialization §"Canonical tagged encoding" + §"Wire fast path" — the two
// concrete codecs driven by the ONE describe() field list, plus the connect-time negotiation.
// Proven by ADR-016 (tagged LEB128 TLV round-trips + additive evolution; tagless packed
// near-memcpy encode; fingerprint + ABI/endian gate is load-bearing).
//
//   * Canonical tagged TLV  — key=(tag<<3)|wire_type, LEB128, little-endian, layout-independent,
//     BOUNDS-CHECKED (parses hostile/durable bytes). The only form used for durable data.
//   * Tagless packed fast path — values in tag order, no keys, trivially-copyable runs bulk
//     copied. UNCHECKED by design (trusts the connect-time gate) — near-memcpy speed. NEVER used
//     for durable data.
//   * negotiate() — tagless IFF per-type fingerprint AND platform/ABI/endian tag both match;
//     else canonical tagged + a 009 warning seam. THIS is what keeps the unchecked tagless path
//     safe; forcing tagless past a mismatch demonstrably corrupts (ADR-016 controls 4/5).
//
// No typeid/dynamic_cast — reflection-free, builds clean under -fno-rtti (ADR-016).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "quark/core/describe.hpp"
#include "quark/core/error.hpp"
#include "pal/pal.hpp"

namespace quark {

namespace detail {

// --- Category predicates for a member's static type ------------------------------------------
template <class U>
concept ScalarVarint =
    std::is_same_v<U, bool> || std::is_enum_v<U> ||
    (std::is_integral_v<U> && !std::is_same_v<U, bool>);

// ============================================================================================
// Canonical tagged TLV — SIZE pass (needed to length-prefix nested/length-delimited fields)
// ============================================================================================
constexpr std::size_t uleb_size(std::uint64_t v) noexcept {
    std::size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        ++n;
    }
    return n;
}

template <class T>
std::size_t tagged_object_size(const T& obj);  // fwd

template <class M>
std::size_t tagged_value_size(const M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>) {
        return 1;
    } else if constexpr (std::is_enum_v<U>) {
        return uleb_size(static_cast<std::uint64_t>(static_cast<std::underlying_type_t<U>>(m)));
    } else if constexpr (std::is_same_v<U, double>) {
        return 8;
    } else if constexpr (std::is_same_v<U, float>) {
        return 4;
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_signed_v<U>) {
            return uleb_size(zigzag(static_cast<std::int64_t>(m)));
        } else {
            return uleb_size(static_cast<std::uint64_t>(m));
        }
    } else if constexpr (is_std_string_v<U>) {
        return uleb_size(m.size()) + m.size();
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        std::size_t content = 0;
        for (const auto& e : m) {
            if constexpr (Described<E>) {
                const std::size_t os = tagged_object_size(e);
                content += uleb_size(os) + os;
            } else {
                content += tagged_value_size(e);
            }
        }
        return uleb_size(content) + content;
    } else {  // nested described type — length-delimited
        const std::size_t os = tagged_object_size(m);
        return uleb_size(os) + os;
    }
}

struct TaggedSizer {
    std::size_t total = 0;
    template <class M>
    void field(std::uint32_t tag, M& m) {
        const auto key = (static_cast<std::uint64_t>(tag) << 3) |
                         static_cast<std::uint64_t>(wire_type_of<M>());
        total += uleb_size(key) + tagged_value_size(m);
    }
};

template <class T>
std::size_t tagged_object_size(const T& obj) {
    TaggedSizer s;
    quark_describe(s, const_cast<T&>(obj));  // describe reads only; const_cast is confined here
    return s.total;
}

// ============================================================================================
// Canonical tagged TLV — WRITE pass
// ============================================================================================
template <class T>
bool tagged_write_object(ByteWriter& w, const T& obj);  // fwd

template <class M>
bool tagged_write_value(ByteWriter& w, const M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>) {
        return put_uleb(w, m ? 1u : 0u);
    } else if constexpr (std::is_enum_v<U>) {
        return put_uleb(w, static_cast<std::uint64_t>(static_cast<std::underlying_type_t<U>>(m)));
    } else if constexpr (std::is_same_v<U, double>) {
        return put_fixed_le<std::uint64_t>(w, std::bit_cast<std::uint64_t>(m));
    } else if constexpr (std::is_same_v<U, float>) {
        return put_fixed_le<std::uint32_t>(w, std::bit_cast<std::uint32_t>(m));
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_signed_v<U>) {
            return put_uleb(w, zigzag(static_cast<std::int64_t>(m)));
        } else {
            return put_uleb(w, static_cast<std::uint64_t>(m));
        }
    } else if constexpr (is_std_string_v<U>) {
        if (!put_uleb(w, m.size())) return false;
        return w.put_bytes(m.data(), m.size());
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        std::size_t content = 0;
        for (const auto& e : m) {
            if constexpr (Described<E>) {
                const std::size_t os = tagged_object_size(e);
                content += uleb_size(os) + os;
            } else {
                content += tagged_value_size(e);
            }
        }
        if (!put_uleb(w, content)) return false;
        for (const auto& e : m) {
            if constexpr (Described<E>) {
                if (!put_uleb(w, tagged_object_size(e))) return false;
                if (!tagged_write_object(w, e)) return false;
            } else {
                if (!tagged_write_value(w, e)) return false;
            }
        }
        return true;
    } else {  // nested described type
        if (!put_uleb(w, tagged_object_size(m))) return false;
        return tagged_write_object(w, m);
    }
}

struct TaggedWriter {
    ByteWriter& w;
    template <class M>
    void field(std::uint32_t tag, M& m) {
        const auto key = (static_cast<std::uint64_t>(tag) << 3) |
                         static_cast<std::uint64_t>(wire_type_of<M>());
        if (!put_uleb(w, key)) return;
        (void)tagged_write_value(w, m);
    }
};

template <class T>
bool tagged_write_object(ByteWriter& w, const T& obj) {
    TaggedWriter aw{w};
    quark_describe(aw, const_cast<T&>(obj));
    return w.ok();
}

// ============================================================================================
// Canonical tagged TLV — READ pass (additive evolution: unknown tags skipped, missing default)
// ============================================================================================
template <class T>
bool tagged_read_object(ByteReader& r, T& out, std::size_t limit);  // fwd

inline bool tagged_skip_value(ByteReader& r, WireType wt) {
    switch (wt) {
        case WireType::Varint: {
            std::uint64_t tmp;
            return get_uleb(r, tmp);
        }
        case WireType::Fixed64:
            return r.skip(8);
        case WireType::Fixed32:
            return r.skip(4);
        case WireType::Bytes: {
            std::uint64_t len;
            if (!get_uleb(r, len)) return false;
            return r.skip(len);
        }
    }
    return false;
}

template <class M>
bool tagged_read_value(ByteReader& r, M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_same_v<U, bool>) {
        std::uint64_t v;
        if (!get_uleb(r, v)) return false;
        m = (v != 0);
        return true;
    } else if constexpr (std::is_enum_v<U>) {
        std::uint64_t v;
        if (!get_uleb(r, v)) return false;
        m = static_cast<U>(static_cast<std::underlying_type_t<U>>(v));
        return true;
    } else if constexpr (std::is_same_v<U, double>) {
        std::uint64_t bits;
        if (!get_fixed_le<std::uint64_t>(r, bits)) return false;
        m = std::bit_cast<double>(bits);
        return true;
    } else if constexpr (std::is_same_v<U, float>) {
        std::uint32_t bits;
        if (!get_fixed_le<std::uint32_t>(r, bits)) return false;
        m = std::bit_cast<float>(bits);
        return true;
    } else if constexpr (std::is_integral_v<U>) {
        std::uint64_t v;
        if (!get_uleb(r, v)) return false;
        if constexpr (std::is_signed_v<U>) {
            m = static_cast<U>(unzigzag(v));
        } else {
            m = static_cast<U>(v);
        }
        return true;
    } else if constexpr (is_std_string_v<U>) {
        std::uint64_t len;
        if (!get_uleb(r, len)) return false;
        if (len > r.remaining()) return false;
        m.assign(reinterpret_cast<const char*>(r.cursor()), static_cast<std::size_t>(len));
        return r.skip(len);
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        std::uint64_t content;
        if (!get_uleb(r, content)) return false;
        if (content > r.remaining()) return false;
        ByteReader sub(r.cursor(), static_cast<std::size_t>(content));
        m.clear();
        while (!sub.at_end()) {
            E elem{};
            if constexpr (Described<E>) {
                std::uint64_t os;
                if (!get_uleb(sub, os)) return false;
                if (!tagged_read_object(sub, elem, static_cast<std::size_t>(os))) return false;
            } else {
                if (!tagged_read_value(sub, elem)) return false;
            }
            m.push_back(std::move(elem));
        }
        return r.skip(content);
    } else {  // nested described type
        std::uint64_t os;
        if (!get_uleb(r, os)) return false;
        return tagged_read_object(r, m, static_cast<std::size_t>(os));
    }
}

template <class T>
struct TaggedFieldDecoder {
    ByteReader& r;
    std::uint32_t want_tag;
    bool handled = false;
    bool ok = true;
    template <class M>
    void field(std::uint32_t tag, M& m) {
        if (handled || tag != want_tag) return;
        handled = true;
        if (!tagged_read_value(r, m)) ok = false;
    }
};

// Read a described object out of `r`, consuming exactly `limit` bytes (a nested/length-delimited
// object). `out` must be default-initialised by the caller so missing tags keep their defaults
// (forward compatibility). Unknown tags are skipped (backward compatibility).
template <class T>
bool tagged_read_object(ByteReader& r, T& out, std::size_t limit) {
    if (limit > r.remaining()) return false;
    ByteReader sub(r.cursor(), limit);
    while (!sub.at_end()) {
        std::uint64_t key;
        if (!get_uleb(sub, key)) return false;
        const auto tag = static_cast<std::uint32_t>(key >> 3);
        const auto wt = static_cast<WireType>(static_cast<std::uint8_t>(key & 0x7u));
        TaggedFieldDecoder<T> dec{sub, tag};
        quark_describe(dec, out);
        if (!dec.ok) return false;
        if (!dec.handled) {
            if (!tagged_skip_value(sub, wt)) return false;  // unknown tag -> skip (evolution)
        }
    }
    return r.skip(limit);
}

}  // namespace detail

// ============================================================================================
// Public canonical (tagged) codec — the ONLY form used for durable data (snapshot/event/wire
// fallback). Bounds-checked. Additive evolution both directions.
// ============================================================================================

// Encode `obj` into `[buf, buf+cap)`. Returns the byte length on success, or a serialization
// error if the buffer is too small.
template <Described T>
[[nodiscard]] result<std::size_t> encode_tagged(const T& obj, std::byte* buf, std::size_t cap) {
    detail::ByteWriter w(buf, cap);
    detail::TaggedWriter aw{w};
    quark_describe(aw, const_cast<T&>(obj));
    if (!w.ok()) return fail(errc::serialization, "encode buffer too small");
    return w.written();
}

// Decode `[buf, buf+len)` into `out` (default-constructed by the caller). Unknown tags are
// skipped; absent tags keep their defaults.
template <Described T>
[[nodiscard]] result<void> decode_tagged(const std::byte* buf, std::size_t len, T& out) {
    detail::ByteReader r(buf, len);
    // Top-level object occupies the whole buffer.
    if (!detail::tagged_read_object(r, out, len)) {
        return fail(errc::serialization, "malformed tagged stream");
    }
    return {};
}

// ============================================================================================
// Tagless packed wire fast path — UNCHECKED. Values in tag order, no keys, trivial runs bulk
// copied. Safe ONLY between fingerprint+ABI-matched peers (negotiate()). NEVER durable.
// ============================================================================================
namespace detail {

template <class T>
std::byte* tagless_write_object(std::byte* p, const T& obj);  // fwd

template <class M>
std::byte* tagless_write_value(std::byte* p, const M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_trivially_copyable_v<U> && !is_std_vector_v<U> && !is_std_string_v<U> &&
                  !Described<U>) {
        // Scalar / trivially-copyable run: raw native-layout bulk copy (the near-memcpy core).
        std::memcpy(p, &m, sizeof(U));
        return p + sizeof(U);
    } else if constexpr (is_std_string_v<U>) {
        const auto n = static_cast<std::uint32_t>(m.size());
        std::memcpy(p, &n, sizeof(n));
        p += sizeof(n);
        std::memcpy(p, m.data(), m.size());
        return p + m.size();
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        const auto n = static_cast<std::uint32_t>(m.size());
        std::memcpy(p, &n, sizeof(n));
        p += sizeof(n);
        if constexpr (std::is_trivially_copyable_v<E> && !Described<E>) {
            std::memcpy(p, m.data(), m.size() * sizeof(E));
            return p + m.size() * sizeof(E);
        } else {
            for (const auto& e : m) p = tagless_write_value(p, e);
            return p;
        }
    } else {  // nested described type
        return tagless_write_object(p, m);
    }
}

template <class T>
struct TaglessWriter {
    std::byte* p;
    template <class M>
    void field(std::uint32_t, M& m) {
        p = tagless_write_value(p, m);
    }
};

template <class T>
std::byte* tagless_write_object(std::byte* p, const T& obj) {
    TaglessWriter<T> w{p};
    quark_describe(w, const_cast<T&>(obj));
    return w.p;
}

template <class T>
const std::byte* tagless_read_object(const std::byte* p, T& obj);  // fwd

template <class M>
const std::byte* tagless_read_value(const std::byte* p, M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_trivially_copyable_v<U> && !is_std_vector_v<U> && !is_std_string_v<U> &&
                  !Described<U>) {
        std::memcpy(&m, p, sizeof(U));  // UNCHECKED read — trusts the negotiation gate
        return p + sizeof(U);
    } else if constexpr (is_std_string_v<U>) {
        std::uint32_t n;
        std::memcpy(&n, p, sizeof(n));
        p += sizeof(n);
        m.assign(reinterpret_cast<const char*>(p), n);
        return p + n;
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        std::uint32_t n;
        std::memcpy(&n, p, sizeof(n));
        p += sizeof(n);
        m.resize(n);
        if constexpr (std::is_trivially_copyable_v<E> && !Described<E>) {
            std::memcpy(m.data(), p, static_cast<std::size_t>(n) * sizeof(E));
            return p + static_cast<std::size_t>(n) * sizeof(E);
        } else {
            for (std::uint32_t i = 0; i < n; ++i) p = tagless_read_value(p, m[i]);
            return p;
        }
    } else {  // nested described type
        return tagless_read_object(p, m);
    }
}

template <class T>
struct TaglessReader {
    const std::byte* p;
    template <class M>
    void field(std::uint32_t, M& m) {
        p = tagless_read_value(p, m);
    }
};

template <class T>
const std::byte* tagless_read_object(const std::byte* p, T& obj) {
    TaglessReader<T> r{p};
    quark_describe(r, obj);
    return r.p;
}

template <class T>
std::size_t tagless_object_size(const T& obj);  // fwd

template <class M>
std::size_t tagless_value_size(const M& m) {
    using U = std::remove_cvref_t<M>;
    if constexpr (std::is_trivially_copyable_v<U> && !is_std_vector_v<U> && !is_std_string_v<U> &&
                  !Described<U>) {
        return sizeof(U);
    } else if constexpr (is_std_string_v<U>) {
        return sizeof(std::uint32_t) + m.size();
    } else if constexpr (is_std_vector_v<U>) {
        using E = std::remove_cvref_t<typename U::value_type>;
        if constexpr (std::is_trivially_copyable_v<E> && !Described<E>) {
            return sizeof(std::uint32_t) + m.size() * sizeof(E);
        } else {
            std::size_t n = sizeof(std::uint32_t);
            for (const auto& e : m) n += tagless_value_size(e);
            return n;
        }
    } else {
        return tagless_object_size(m);
    }
}

template <class T>
struct TaglessSizer {
    std::size_t total = 0;
    template <class M>
    void field(std::uint32_t, M& m) {
        total += tagless_value_size(m);
    }
};

template <class T>
std::size_t tagless_object_size(const T& obj) {
    TaglessSizer<T> s;
    quark_describe(s, const_cast<T&>(obj));
    return s.total;
}

}  // namespace detail

// Tagless encode into a caller-provided buffer. Returns bytes written. The buffer MUST be sized
// for the type (tagless is unchecked); callers size it via tagless_size(). 0-alloc: no growth,
// no heap — writes at fixed offsets by bulk copy (ADR-016 near-memcpy fast path).
template <Described T>
std::size_t encode_tagless(const T& obj, std::byte* buf) noexcept {
    std::byte* end = detail::tagless_write_object(buf, obj);
    return static_cast<std::size_t>(end - buf);
}

// Tagless decode from a matched-schema, matched-ABI peer's bytes. UNCHECKED by design.
template <Described T>
std::size_t decode_tagless(const std::byte* buf, T& out) noexcept {
    const std::byte* end = detail::tagless_read_object(buf, out);
    return static_cast<std::size_t>(end - buf);
}

// The exact tagless size of an object (for buffer provisioning). For an all-trivial POD this is
// the packed sum of member sizes (near-sizeof).
template <Described T>
std::size_t tagless_size(const T& obj) noexcept {
    return detail::tagless_object_size(obj);
}

// ============================================================================================
// Connect-time negotiation (016 §"Wire fast path") — THE load-bearing gate.
// ============================================================================================
enum class WireMode : std::uint8_t {
    Tagless,  // fingerprint AND ABI/endian both match — packed fast path
    Tagged,   // mismatch — canonical layout-independent tagged form + a 009 warning
};

// 009 observability seam: a fallback warning hook. Null by default (no-op). Wired by the
// transport/telemetry layer (010/009), not by the codec.
using WireWarnFn = void (*)(std::string_view msg);
inline WireWarnFn g_wire_fallback_warn = nullptr;

// The per-connection descriptor a peer advertises for a type.
struct PeerSchema {
    std::uint64_t fingerprint = 0;
    std::uint32_t abi_tag = 0;
};

// Choose the wire encoding for `T` against a peer's advertised schema. Tagless IFF the per-type
// fingerprint matches AND the platform/ABI/endian tag matches; otherwise canonical tagged +
// a warning. Bypassing this (forcing tagless past a mismatch) demonstrably corrupts — ADR-016
// controls 4 and 5 — which is the whole reason the gate exists.
template <Described T>
[[nodiscard]] WireMode negotiate(const PeerSchema& peer) noexcept {
    if (peer.fingerprint == fingerprint_v<T> && peer.abi_tag == pal::platform_abi_tag) {
        return WireMode::Tagless;
    }
    if (g_wire_fallback_warn != nullptr) {
        g_wire_fallback_warn("wire fast-path disabled (fingerprint/ABI mismatch); using tagged");
    }
    return WireMode::Tagged;
}

// The local schema this node advertises for `T` at connect.
template <Described T>
[[nodiscard]] PeerSchema local_schema() noexcept {
    return PeerSchema{fingerprint_v<T>, pal::platform_abi_tag};
}

}  // namespace quark
