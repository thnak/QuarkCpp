// Implements ADR-074 D4 / 021 §Enrolment, the WIRE half that `Enrollment` (enrollment.hpp) deliberately leaves to the transport layer (issue #36, D4c):
// the join code a joiner is handed out of band, and the two messages that cross the enrolment channel. Pure and std-only: no sockets, no crypto
// (the SHA-256 of the CA root is carried, never computed here), so it is testable without a transport and cannot be a vector for anything but
// its own parsing.
//
// JOIN CODE (binary, 106 bytes):  "QJC1" | u32 anchor IPv4 (host order, as PAL `addr`) | u16 port | u64 anchor NodeId | u64 cluster id |
//   32 B SHA-256 of the CA root certificate (DER) | 16 B ticket id | 32 B ticket secret.   All integers little endian.
// TEXT FORM (what an operator types or scans): RFC 4648 base32 (upper case, no padding) of  binary || CRC-32 (4 bytes, little endian), written in
// groups of 8 characters separated by '-'. The CRC only catches typing mistakes (a wrong code is refused before any network use); it is NOT a MAC:
// the ticket secret is the only secret, and it never travels (the joiner proves it with an HMAC, enrollment.hpp). Decoding accepts lower case and
// ignores '-' and spaces, and refuses everything else, any wrong length, a bad CRC, a zero port, a zero NodeId and a NodeId whose bit 63 (the client
// role, ADR-074 D3) is set: an anchor is always a member.
//
// REQUEST (joiner -> anchor, inside the server-authenticated channel):  u8 kind=1 | 16 B ticket id | 32 B HMAC tag | u16 CSR length | CSR (DER).
// REPLY   (anchor -> joiner):                                           u8 kind=2 | u8 WireStatus | u16 chain length | chain (concatenated DER);
//   the chain is present only for WireStatus::Issued and absent (length 0) otherwise. A reply never carries a Status finer than WireStatus (the anchor's
//   audit record keeps the detail), so a prober learns nothing about which ticket ids exist.
// Both messages are length-prefixed on the stream by `frame_message` (u32 little endian, bounded), so a reader needs no knowledge of their type.
// Every decoder bounds every length BEFORE it allocates, refuses trailing bytes and unknown kinds, and returns an error rather than a partial value.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "quark/core/enrollment.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"

namespace quark::enrol::wire {

inline constexpr std::size_t kMaxCsrBytes = 4096;           // equals the adapter's bound (mbedtls_enrollment.hpp detail::kMaxCsrBytes)
inline constexpr std::size_t kMaxChainBytes = 16384;        // equals detail::kMaxIssuedChainBytes
inline constexpr std::size_t kMaxMessageBytes = 1 + 16 + 32 + 2 + kMaxCsrBytes + kMaxChainBytes;  // an upper bound for either message
inline constexpr std::size_t kJoinCodeBytes = 106;
inline constexpr std::uint8_t kRequestKind = 1;
inline constexpr std::uint8_t kReplyKind = 2;

using RootHash = std::array<std::byte, 32>;

struct JoinCode {
    std::uint32_t anchor_addr = 0;  // IPv4, host byte order
    std::uint16_t anchor_port = 0;
    NodeId anchor_node{};
    ClusterId cluster{};
    RootHash ca_root_sha256{};
    TicketId ticket_id{};
    TicketSecret ticket_secret{};
    friend bool operator==(const JoinCode&, const JoinCode&) = default;
};

namespace detail {

inline void put_le(std::vector<std::byte>& b, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
[[nodiscard]] inline std::uint64_t get_le(std::span<const std::byte> b, std::size_t at, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b[at + static_cast<std::size_t>(i)])) << (8 * i);
    return v;
}

[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::byte x : data) {
        c ^= std::to_integer<std::uint8_t>(x);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

inline constexpr std::string_view kB32{"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"};

[[nodiscard]] inline std::string base32(std::span<const std::byte> in) {
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::byte x : in) {
        acc = (acc << 8) | std::to_integer<std::uint8_t>(x);
        bits += 8;
        while (bits >= 5) {
            out.push_back(kB32[(acc >> (bits - 5)) & 31u]);
            bits -= 5;
        }
    }
    if (bits > 0) out.push_back(kB32[(acc << (5 - bits)) & 31u]);
    return out;
}

// Decodes RFC 4648 base32 (no padding). Refuses a character outside the alphabet and non-zero trailing bits (so each byte string has ONE text form).
[[nodiscard]] inline bool unbase32(std::string_view in, std::vector<std::byte>& out) {
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v = -1;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= '2' && ch <= '7') v = 26 + (ch - '2');
        if (v < 0) return false;
        acc = (acc << 5) | static_cast<std::uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::byte>((acc >> (bits - 8)) & 0xFF));
            bits -= 8;
        }
    }
    return bits < 5 && (acc & ((1u << bits) - 1u)) == 0;
}

}  // namespace detail

[[nodiscard]] inline std::vector<std::byte> encode_join_code_binary(const JoinCode& c) {
    std::vector<std::byte> b;
    b.reserve(kJoinCodeBytes);
    for (char ch : std::string_view{"QJC1"}) b.push_back(static_cast<std::byte>(ch));
    detail::put_le(b, c.anchor_addr, 4);
    detail::put_le(b, c.anchor_port, 2);
    detail::put_le(b, c.anchor_node.value, 8);
    detail::put_le(b, c.cluster.value, 8);
    b.insert(b.end(), c.ca_root_sha256.begin(), c.ca_root_sha256.end());
    b.insert(b.end(), c.ticket_id.begin(), c.ticket_id.end());
    b.insert(b.end(), c.ticket_secret.begin(), c.ticket_secret.end());
    return b;
}

[[nodiscard]] inline result<JoinCode> decode_join_code_binary(std::span<const std::byte> b) {
    if (b.size() != kJoinCodeBytes) return fail(errc::validation, "join code: wrong length");
    if (b[0] != std::byte{'Q'} || b[1] != std::byte{'J'} || b[2] != std::byte{'C'} || b[3] != std::byte{'1'})
        return fail(errc::validation, "join code: bad magic or version");
    JoinCode c;
    c.anchor_addr = static_cast<std::uint32_t>(detail::get_le(b, 4, 4));
    c.anchor_port = static_cast<std::uint16_t>(detail::get_le(b, 8, 2));
    c.anchor_node = NodeId{detail::get_le(b, 10, 8)};
    c.cluster = ClusterId{detail::get_le(b, 18, 8)};
    std::copy_n(b.begin() + 26, 32, c.ca_root_sha256.begin());
    std::copy_n(b.begin() + 58, 16, c.ticket_id.begin());
    std::copy_n(b.begin() + 74, 32, c.ticket_secret.begin());
    if (c.anchor_port == 0) return fail(errc::validation, "join code: port 0");
    if (c.anchor_node.value == 0) return fail(errc::validation, "join code: anchor NodeId 0");
    if ((c.anchor_node.value >> 63) != 0) return fail(errc::validation, "join code: the anchor must be a member, not a client id");
    return c;
}

[[nodiscard]] inline std::string encode_join_code(const JoinCode& c) {
    std::vector<std::byte> b = encode_join_code_binary(c);
    detail::put_le(b, detail::crc32(b), 4);
    const std::string flat = detail::base32(b);
    std::string out;
    out.reserve(flat.size() + flat.size() / 8);
    for (std::size_t i = 0; i < flat.size(); ++i) {
        if (i != 0 && i % 8 == 0) out.push_back('-');
        out.push_back(flat[i]);
    }
    return out;
}

[[nodiscard]] inline result<JoinCode> decode_join_code(std::string_view text) {
    if (text.size() > 4 * kJoinCodeBytes) return fail(errc::validation, "join code: text too long");
    std::string flat;
    flat.reserve(text.size());
    for (char ch : text) {
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        flat.push_back(ch);
    }
    std::vector<std::byte> b;
    if (!detail::unbase32(flat, b)) return fail(errc::validation, "join code: not base32");
    if (b.size() != kJoinCodeBytes + 4) return fail(errc::validation, "join code: wrong length");
    const auto body = std::span<const std::byte>(b).first(kJoinCodeBytes);
    if (detail::get_le(b, kJoinCodeBytes, 4) != detail::crc32(body)) return fail(errc::validation, "join code: checksum mismatch (typing error)");
    return decode_join_code_binary(body);
}

[[nodiscard]] inline result<std::vector<std::byte>> encode_request(const EnrolRequest& r) {
    if (r.csr_der.empty() || r.csr_der.size() > kMaxCsrBytes) return fail(errc::validation, "enrol request: CSR size out of bounds");
    std::vector<std::byte> b;
    b.reserve(1 + 16 + 32 + 2 + r.csr_der.size());
    b.push_back(static_cast<std::byte>(kRequestKind));
    b.insert(b.end(), r.ticket_id.begin(), r.ticket_id.end());
    b.insert(b.end(), r.tag.begin(), r.tag.end());
    detail::put_le(b, r.csr_der.size(), 2);
    b.insert(b.end(), r.csr_der.begin(), r.csr_der.end());
    return b;
}

// `source` is NOT on the wire: the receiving transport fills it from the connection (enrollment.hpp, EnrolRequest).
[[nodiscard]] inline result<EnrolRequest> decode_request(std::span<const std::byte> b) {
    constexpr std::size_t kFixed = 1 + 16 + 32 + 2;
    if (b.size() < kFixed) return fail(errc::validation, "enrol request: truncated");
    if (std::to_integer<std::uint8_t>(b[0]) != kRequestKind) return fail(errc::validation, "enrol request: unknown kind");
    const std::size_t n = static_cast<std::size_t>(detail::get_le(b, 49, 2));
    if (n == 0 || n > kMaxCsrBytes) return fail(errc::validation, "enrol request: CSR size out of bounds");
    if (b.size() != kFixed + n) return fail(errc::validation, "enrol request: length mismatch or trailing bytes");
    EnrolRequest r;
    std::copy_n(b.begin() + 1, 16, r.ticket_id.begin());
    std::copy_n(b.begin() + 17, 32, r.tag.begin());
    r.csr_der.assign(b.begin() + static_cast<std::ptrdiff_t>(kFixed), b.end());
    return r;
}

struct WireReply {
    WireStatus status = WireStatus::Refused;
    std::vector<std::byte> cert_chain_der;  // WireStatus::Issued only
};

[[nodiscard]] inline result<std::vector<std::byte>> encode_reply(const WireReply& r) {
    if (r.status == WireStatus::Issued) {
        if (r.cert_chain_der.empty() || r.cert_chain_der.size() > kMaxChainBytes) return fail(errc::validation, "enrol reply: chain size out of bounds");
    } else if (!r.cert_chain_der.empty()) {
        return fail(errc::validation, "enrol reply: only an Issued reply carries a chain");
    }
    std::vector<std::byte> b;
    b.push_back(static_cast<std::byte>(kReplyKind));
    b.push_back(static_cast<std::byte>(r.status));
    detail::put_le(b, r.cert_chain_der.size(), 2);
    b.insert(b.end(), r.cert_chain_der.begin(), r.cert_chain_der.end());
    return b;
}

[[nodiscard]] inline result<WireReply> decode_reply(std::span<const std::byte> b) {
    if (b.size() < 4) return fail(errc::validation, "enrol reply: truncated");
    if (std::to_integer<std::uint8_t>(b[0]) != kReplyKind) return fail(errc::validation, "enrol reply: unknown kind");
    const auto s = std::to_integer<std::uint8_t>(b[1]);
    if (s > static_cast<std::uint8_t>(WireStatus::IssuanceFailed)) return fail(errc::validation, "enrol reply: unknown status");
    const std::size_t n = static_cast<std::size_t>(detail::get_le(b, 2, 2));
    if (n > kMaxChainBytes) return fail(errc::validation, "enrol reply: chain size out of bounds");
    if (b.size() != 4 + n) return fail(errc::validation, "enrol reply: length mismatch or trailing bytes");
    WireReply r;
    r.status = static_cast<WireStatus>(s);
    if (r.status == WireStatus::Issued && n == 0) return fail(errc::validation, "enrol reply: Issued without a chain");
    if (r.status != WireStatus::Issued && n != 0) return fail(errc::validation, "enrol reply: a refusal carries no chain");
    r.cert_chain_der.assign(b.begin() + 4, b.end());
    return r;
}

// Stream framing: u32 little-endian length, then the message. `kMaxMessageBytes` bounds it so a reader allocates nothing for an absurd prefix.
[[nodiscard]] inline std::vector<std::byte> frame_message(std::span<const std::byte> msg) {
    std::vector<std::byte> b;
    b.reserve(4 + msg.size());
    detail::put_le(b, msg.size(), 4);
    b.insert(b.end(), msg.begin(), msg.end());
    return b;
}

// Incremental reader for the stream framing. Feed bytes as they arrive; `next` yields one whole message at a time. Bounded: a length above
// kMaxMessageBytes (or a buffer that would exceed one framed message) makes the reader fail permanently, so a hostile peer cannot grow it.
class MessageReader {
public:
    [[nodiscard]] bool feed(std::span<const std::byte> in) {
        if (failed_) return false;
        if (buf_.size() + in.size() > 4 + kMaxMessageBytes) {
            failed_ = true;
            return false;
        }
        buf_.insert(buf_.end(), in.begin(), in.end());
        if (buf_.size() >= 4 && detail::get_le(buf_, 0, 4) > kMaxMessageBytes) {
            failed_ = true;
            return false;
        }
        return true;
    }
    // True and fills `msg` when a whole message is buffered; the bytes are consumed.
    [[nodiscard]] bool next(std::vector<std::byte>& msg) {
        if (failed_ || buf_.size() < 4) return false;
        const std::size_t n = static_cast<std::size_t>(detail::get_le(buf_, 0, 4));
        if (buf_.size() < 4 + n) return false;
        msg.assign(buf_.begin() + 4, buf_.begin() + static_cast<std::ptrdiff_t>(4 + n));
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(4 + n));
        return true;
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::vector<std::byte> buf_;
    bool failed_ = false;
};

}  // namespace quark::enrol::wire
