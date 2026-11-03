// Implements 010-Distribution §"Transport seam" wire framing — the byte layout a `MessageFrame`
// takes on a TCP stream, plus the stream reassembler that turns TCP's boundary-less byte stream back
// into discrete frames. This is the "length-prefixed framing" 010 mandates ("minimal length-prefixed
// framing over TCP"), factored out of tcp_transport.hpp so it is unit-testable with NO sockets.
//
// LAYOUT (little-endian, matching the std-only SWIM control codec in cluster.hpp): each frame on the
// wire is `[u32 body_len][body]`. The u32 length prefix is what makes reassembly possible — a reader
// that has fewer than `4 + body_len` bytes has a partial frame and waits; a reader with more has one or
// more whole frames back-to-back (TCP coalescing) and extracts them in a loop. The body carries every
// MessageFrame header field a receiving node needs to route + decode WITHOUT a per-message schema
// exchange (that is the connect-time 016 negotiation; `mode` records its per-type outcome), then the
// 016-serialized payload. All integers are fixed-width LE so the frame is endian-stable on the wire
// (the x86-64-only ABI tag still gates the tagless PAYLOAD fast path; the FRAME HEADER is always
// portable LE so a header can be parsed before the payload mode is even known).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "quark/core/transport.hpp"  // MessageFrame, FrameKind, WireMode, NodeId/ActorId/TypeKey, Principal

namespace quark::net {

// The largest body a single frame may declare. A peer (or a corrupt/adversarial stream) that prefixes a
// larger length is a protocol error — the reassembler rejects the stream rather than buffering it.
// 4 MiB is far above any actor control message; oversized payloads are a chunking/streaming concern
// (024), not a single-frame concern. The old 64 MiB limit let one connection force a ~128 MiB peak
// (reassembler buffer + MessageFrame payload) per frame.
inline constexpr std::uint32_t max_frame_body = 4u * 1024u * 1024u;

// The fixed MessageFrame header at the start of every body (everything encode_frame_body writes before
// the payload): from, to, target.type, target.key, msg_type (5 × u64), mode + kind (2 × u8), deadline,
// trace_id, principal.subject, principal.rights (4 × u64), payload length (u32) = 78 bytes. The body cap
// above covers THIS header, so a consumer must not treat `max_frame_body` as a payload limit.
inline constexpr std::uint32_t frame_body_header_size = 5 * 8 + 2 + 4 * 8 + 4;  // 78

// Largest payload a frame may carry under a body cap of `max_body`. `overhead` is extra per-frame bytes a
// decorator places inside the payload (SecureTransport's sequence prefix + AEAD tag — see below). Returns
// 0 when the cap cannot hold even an empty frame.
[[nodiscard]] constexpr std::uint32_t max_payload_for_body_cap(std::uint32_t max_body,
                                                              std::uint32_t overhead = 0) noexcept {
    const std::uint32_t fixed = frame_body_header_size + overhead;
    return max_body > fixed ? max_body - fixed : 0;
}

// SecureTransport's sealed payload is [u64 sequence number][ciphertext][AEAD tag], so a sealed frame's
// body is the 78-byte header + 8 + plaintext + tag. `tag_size` is the session Aead's tag_size().
inline constexpr std::uint32_t sealed_seq_prefix_size = 8;

[[nodiscard]] constexpr std::uint32_t max_sealed_payload_for_body_cap(
    std::uint32_t max_body, std::uint32_t tag_size) noexcept {
    return max_payload_for_body_cap(max_body, sealed_seq_prefix_size + tag_size);
}

// Wire offset of the one-byte FrameKind: [u32 body_len][from u64][to u64][target.type u64][target.key u64]
// [msg_type u64][mode u8][kind u8]... Simple consumers (TcpTransport's Authenticate purge and its
// data-bearing marker) need to classify a queued encoded frame without decoding it again.
inline constexpr std::size_t frame_kind_offset = 4 + 5 * 8 + 1;  // 45

namespace detail {

// GCC's inliner sometimes loses track of std::vector<std::byte>'s growth-reallocation pointer
// through push_back and misattributes the old buffer's free() as a mismatched/non-heap free — a
// known false positive (the buffer is always the vector's own prior heap allocation). Silenced
// locally rather than restructured: there is no allocation pattern here that avoids the warning,
// since it fires on the flow-insensitive growth branch regardless of whether it's ever taken.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
inline void put_u8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(std::byte{v}); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
inline void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) put_u8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) put_u8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}

// A bounds-checked LE reader over a byte span. `ok` latches false on the first over-read; every getter
// past that point returns 0, so a truncated/malformed body decodes to `ok == false` and is dropped —
// never UB. This is the cold decode path, not the hot local send.
struct Reader {
    const std::byte* p;
    const std::byte* end;
    bool ok = true;
    [[nodiscard]] std::uint8_t u8() noexcept {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint8_t>(*p++);
    }
    [[nodiscard]] std::uint32_t u32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        return v;
    }
    [[nodiscard]] std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
};

}  // namespace detail

// Serialize the frame BODY (no length prefix) — the field order the Reader below mirrors exactly.
inline void encode_frame_body(const MessageFrame& f, std::vector<std::byte>& out) {
    detail::put_u64(out, f.from.value);
    detail::put_u64(out, f.to.value);
    detail::put_u64(out, f.target.type.value);
    detail::put_u64(out, f.target.key);
    detail::put_u64(out, f.msg_type.value);
    detail::put_u8(out, static_cast<std::uint8_t>(f.mode));
    detail::put_u8(out, static_cast<std::uint8_t>(f.kind));
    detail::put_u64(out, static_cast<std::uint64_t>(f.deadline_ns));
    detail::put_u64(out, f.trace_id);
    detail::put_u64(out, f.principal.subject);
    detail::put_u64(out, f.principal.rights);
    detail::put_u32(out, static_cast<std::uint32_t>(f.payload.size()));
    out.insert(out.end(), f.payload.begin(), f.payload.end());
}

// Serialize a whole wire record: `[u32 body_len][body]`. This is exactly what goes onto the socket.
[[nodiscard]] inline std::vector<std::byte> encode_frame(const MessageFrame& f) {
    std::vector<std::byte> body;
    encode_frame_body(f, body);
    std::vector<std::byte> out;
    out.reserve(4 + body.size());
    detail::put_u32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Decode a frame BODY (the bytes AFTER the u32 length prefix). Returns nullopt if the body is truncated
// or the declared payload length runs past the body — a malformed frame is dropped, never UB.
[[nodiscard]] inline std::optional<MessageFrame> decode_frame_body(std::span<const std::byte> body) {
    detail::Reader r{body.data(), body.data() + body.size()};
    MessageFrame f;
    f.from.value = r.u64();
    f.to.value = r.u64();
    f.target.type.value = r.u64();
    f.target.key = r.u64();
    f.msg_type.value = r.u64();
    f.mode = static_cast<WireMode>(r.u8());
    f.kind = static_cast<FrameKind>(r.u8());
    f.deadline_ns = static_cast<std::int64_t>(r.u64());
    f.trace_id = r.u64();
    f.principal.subject = r.u64();
    f.principal.rights = r.u64();
    const std::uint32_t plen = r.u32();
    if (!r.ok) return std::nullopt;
    if (static_cast<std::size_t>(r.end - r.p) < plen) return std::nullopt;  // payload runs past body
    f.payload.assign(r.p, r.p + plen);
    return f;
}

// ---- Pinned wire: the anonymous-inbound rule (#45, ADR-072; fuzzed by #50) --------------------------
// An accepted connection that no handshake has identified yet may send exactly ONE frame: an Authenticate
// opener of at most kAnonOpenerMax bytes with from == to == 0 (the pinned wire carries no ids). Anything else
// closes the connection. This is the whole decision, with no socket in it, so TcpTransport and the fuzz target
// share it (a narrow test seam: the transport's receive loop calls exactly this).
inline constexpr std::size_t kAnonOpenerMax = 1024;  // a Noise IK Msg1 is 154 bytes

[[nodiscard]] inline bool anonymous_opener_admissible(const MessageFrame& f, bool opener_already_seen) noexcept {
    return !opener_already_seen && f.kind == FrameKind::Authenticate && f.from.value == 0 && f.to.value == 0 &&
           f.payload.size() <= kAnonOpenerMax;
}

// ================================================================================================
// FrameStream — the "stream" reassembler (010): feed it whatever bytes a recv() delivered (a partial
// frame, exactly one frame, or several coalesced frames), and it emits each COMPLETE frame in order,
// buffering the incomplete tail until more bytes arrive. This is what makes TCP's boundary-less byte
// stream behave like the discrete message boundary the actor layer expects. Ownership of framing is
// ABOVE the PAL (019 §"sockets": "Framing lives above the PAL") — this is that layer.
// ================================================================================================
class FrameStream {
public:
    explicit FrameStream(std::uint32_t max_body = max_frame_body) noexcept
        : max_body_(max_body == 0 ? 1 : max_body) {}

    // Per-transport override (call before start(); a smaller deployment cap rejects oversized
    // frames without buffering them). 0 is clamped to 1.
    void set_max_body(std::uint32_t n) noexcept { max_body_ = n == 0 ? 1 : n; }
    [[nodiscard]] std::uint32_t max_body() const noexcept { return max_body_; }

    // Append `n` bytes just recv()'d, then drain every whole frame now available, calling
    // `on_frame(MessageFrame&&)` for each in arrival order. Returns false on a PROTOCOL ERROR
    // (an oversized length prefix, or a body that fails to decode) — the caller must drop the
    // connection, since the stream framing can no longer be trusted. Returns true on clean progress
    // (including "only a partial frame so far — nothing emitted yet").
    template <class OnFrame>
    [[nodiscard]] bool feed(const std::byte* data, std::size_t n, OnFrame&& on_frame) {
        buf_.insert(buf_.end(), data, data + n);
        std::size_t pos = 0;
        for (;;) {
            if (buf_.size() - pos < 4) break;  // not even a full length prefix yet
            const std::uint32_t body_len = read_u32(&buf_[pos]);
            if (body_len > max_body_) return false;  // protocol error: implausible frame size
            // A MessageFrame body always contains the fixed header fields, so an empty body is a
            // malformed stream. Reject it here rather than taking a span at the vector's end.
            if (body_len == 0) return false;
            if (buf_.size() - pos < 4u + body_len) break;  // partial frame — wait for more bytes
            // `data() +` (not `&buf_[pos + 4]`): valid even when the body ends exactly at the buffer end
            // (a zero-length span), where `operator[]` would trip the libstdc++ bounds assertion.
            std::optional<MessageFrame> f =
                decode_frame_body(std::span<const std::byte>(buf_.data() + pos + 4, body_len));
            if (!f) return false;  // malformed body on a length-consistent stream ⇒ untrustworthy
            pos += 4u + body_len;
            on_frame(std::move(*f));
        }
        if (pos > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
        return true;
    }

    // Bytes currently buffered as an incomplete frame (tests / diagnostics).
    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size(); }
    void clear() noexcept { buf_.clear(); }

private:
    [[nodiscard]] static std::uint32_t read_u32(const std::byte* p) noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i]))
                                         << (8 * i);
        return v;
    }
    std::uint32_t max_body_ = max_frame_body;
    std::vector<std::byte> buf_;
};

}  // namespace quark::net
