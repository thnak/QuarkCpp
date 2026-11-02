// Tests ADR-074 D4 / issue #36 (D4c), the wire half in enrol_wire.hpp: the join code (binary and typed text form) and the two enrolment messages.
// Pure and std-only. Each property has a positive control (the well-formed value round-trips) and the refusals that give the property its meaning.
//
//   W1 a join code round-trips through binary and through its text form; the text form is stable and group-separated
//   W2 a typing mistake, a truncation, an extension or a wrong alphabet is refused (CRC and length), and lower case / separators are accepted
//   W3 a join code naming port 0, NodeId 0 or a client-bit anchor id is refused even when its CRC is valid
//   W4 requests and replies round-trip; every length bound is checked before allocation; trailing bytes, unknown kinds and a chain on a refusal are refused
//   W5 the stream reader yields whole messages across arbitrary chunking, and fails permanently on an absurd length prefix
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/enrol_wire.hpp"

using namespace quark;
using namespace quark::enrol;
using namespace quark::enrol::wire;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::printf("FAIL: %s\n", what);
        ok = false;
    }
}

JoinCode sample() {
    JoinCode c;
    c.anchor_addr = 0xC0A80107u;  // 192.168.1.7
    c.anchor_port = 41871;
    c.anchor_node = NodeId{0x1122334455667788ull & ~(1ull << 63)};
    c.cluster = ClusterId{0x0102030405060708ull};
    for (std::size_t i = 0; i < c.ca_root_sha256.size(); ++i) c.ca_root_sha256[i] = static_cast<std::byte>(0xA0 + i);
    for (std::size_t i = 0; i < c.ticket_id.size(); ++i) c.ticket_id[i] = static_cast<std::byte>(i + 1);
    for (std::size_t i = 0; i < c.ticket_secret.size(); ++i) c.ticket_secret[i] = static_cast<std::byte>(0x40 + 3 * i);
    return c;
}

std::vector<std::byte> bytes(std::size_t n, std::uint8_t v) { return std::vector<std::byte>(n, static_cast<std::byte>(v)); }

// A text code for arbitrary binary content with a VALID checksum (to prove a refusal is about the content, not the CRC).
std::string text_of(std::vector<std::byte> b) {
    b.resize(kJoinCodeBytes);
    wire::detail::put_le(b, wire::detail::crc32(b), 4);
    return wire::detail::base32(b);
}
}  // namespace

int main() {
    bool ok = true;
    const JoinCode jc = sample();

    // W1
    {
        auto bin = decode_join_code_binary(encode_join_code_binary(jc));
        check(bin && *bin == jc, "W1 binary round trip", ok);
        const std::string t = encode_join_code(jc);
        auto back = decode_join_code(t);
        check(back && *back == jc, "W1 text round trip", ok);
        check(t == encode_join_code(jc), "W1 the text form is deterministic", ok);
        check(t.find('-') == 8 && t.size() == 176 + 21, "W1 groups of 8 separated by dash (176 chars + 21 separators)", ok);
        check(encode_join_code_binary(jc).size() == kJoinCodeBytes, "W1 binary length is 106", ok);
    }

    // W2
    {
        const std::string t = encode_join_code(jc);
        std::string lower;
        for (char c : t) lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        check(decode_join_code(lower).has_value(), "W2 lower case accepted", ok);
        std::string spaced = t;
        spaced.insert(3, "  ");
        check(decode_join_code(spaced).has_value(), "W2 stray spaces accepted", ok);

        int rejected = 0, mutated = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '-') continue;
            std::string m = t;
            m[i] = (m[i] == 'A') ? 'B' : 'A';
            ++mutated;
            if (!decode_join_code(m)) ++rejected;
        }
        check(rejected == mutated, "W2 every single-character change is refused (CRC)", ok);
        check(!decode_join_code(t.substr(0, t.size() - 2)), "W2 truncation refused", ok);
        check(!decode_join_code(t + "AA"), "W2 extension refused", ok);
        check(!decode_join_code(""), "W2 empty refused", ok);
        std::string bad = t;
        bad[0] = '1';
        check(!decode_join_code(bad), "W2 a character outside the alphabet is refused", ok);
        check(!decode_join_code(std::string(2000, 'A')), "W2 absurd length refused before decoding", ok);
        // A second text form for the same bytes must not exist: the codec refuses non-zero trailing bits. (The 110-byte join code fills its last
        // character exactly, so this is checked on the base32 helper itself, with its positive control.)
        std::vector<std::byte> tmp;
        check(wire::detail::unbase32("AA", tmp) && tmp.size() == 1, "W2 CONTROL: two characters with zero trailing bits decode to one byte", ok);
        tmp.clear();
        check(!wire::detail::unbase32("AB", tmp), "W2 non-zero trailing bits are refused (one text form per byte string)", ok);
    }

    // W3 (valid CRC, bad content)
    {
        JoinCode c = jc;
        c.anchor_port = 0;
        check(!decode_join_code(text_of(encode_join_code_binary(c))), "W3 port 0 refused", ok);
        c = jc;
        c.anchor_node = NodeId{0};
        check(!decode_join_code(text_of(encode_join_code_binary(c))), "W3 NodeId 0 refused", ok);
        c = jc;
        c.anchor_node = NodeId{jc.anchor_node.value | (1ull << 63)};
        check(!decode_join_code(text_of(encode_join_code_binary(c))), "W3 a client-bit anchor id refused", ok);
        auto b = encode_join_code_binary(jc);
        b[3] = static_cast<std::byte>('2');
        check(!decode_join_code(text_of(b)), "W3 an unknown version refused", ok);
        check(decode_join_code(text_of(encode_join_code_binary(jc))).has_value(), "W3 CONTROL: the same helper with valid content decodes", ok);
    }

    // W4
    {
        EnrolRequest r;
        for (std::size_t i = 0; i < r.ticket_id.size(); ++i) r.ticket_id[i] = static_cast<std::byte>(i);
        for (std::size_t i = 0; i < r.tag.size(); ++i) r.tag[i] = static_cast<std::byte>(0x80 + i);
        r.csr_der = bytes(300, 0x30);
        auto enc = encode_request(r);
        check(enc.has_value(), "W4 request encodes", ok);
        auto dec = decode_request(*enc);
        check(dec && dec->ticket_id == r.ticket_id && dec->tag == r.tag && dec->csr_der == r.csr_der, "W4 request round trip", ok);
        check(dec && dec->source.empty(), "W4 the request carries no source (the transport fills it)", ok);

        auto bad = *enc;
        bad.push_back(std::byte{0});
        check(!decode_request(bad), "W4 request trailing byte refused", ok);
        bad = *enc;
        bad.pop_back();
        check(!decode_request(bad), "W4 request truncated body refused", ok);
        bad = *enc;
        bad[0] = std::byte{9};
        check(!decode_request(bad), "W4 request unknown kind refused", ok);
        bad = *enc;
        bad[49] = std::byte{0};
        bad[50] = std::byte{0};
        check(!decode_request(bad), "W4 request CSR length 0 refused", ok);
        bad = *enc;
        bad[49] = std::byte{0xFF};
        bad[50] = std::byte{0xFF};
        check(!decode_request(bad), "W4 request CSR length above the bound refused without allocating it", ok);
        r.csr_der = bytes(kMaxCsrBytes + 1, 1);
        check(!encode_request(r), "W4 encoder refuses an oversize CSR", ok);
        r.csr_der.clear();
        check(!encode_request(r), "W4 encoder refuses an empty CSR", ok);

        WireReply ok_reply{WireStatus::Issued, bytes(900, 0x31)};
        auto re = encode_reply(ok_reply);
        auto rd = decode_reply(*re);
        check(rd && rd->status == WireStatus::Issued && rd->cert_chain_der == ok_reply.cert_chain_der, "W4 Issued reply round trip", ok);
        for (WireStatus s : {WireStatus::Refused, WireStatus::RetryLater, WireStatus::BadCsr, WireStatus::IssuanceFailed}) {
            auto e2 = encode_reply(WireReply{s, {}});
            auto d2 = e2 ? decode_reply(*e2) : fail(errc::internal);
            check(d2 && d2->status == s && d2->cert_chain_der.empty(), "W4 refusal reply round trip", ok);
        }
        check(!encode_reply(WireReply{WireStatus::Refused, bytes(4, 1)}), "W4 encoder refuses a chain on a refusal", ok);
        check(!encode_reply(WireReply{WireStatus::Issued, {}}), "W4 encoder refuses Issued without a chain", ok);
        check(!encode_reply(WireReply{WireStatus::Issued, bytes(kMaxChainBytes + 1, 1)}), "W4 encoder refuses an oversize chain", ok);
        auto rbad = *re;
        rbad[1] = std::byte{77};
        check(!decode_reply(rbad), "W4 reply unknown status refused", ok);
        rbad = *re;
        rbad.push_back(std::byte{0});
        check(!decode_reply(rbad), "W4 reply trailing byte refused", ok);
        rbad = *encode_reply(WireReply{WireStatus::Refused, {}});
        rbad.push_back(std::byte{1});
        rbad[2] = std::byte{1};
        check(!decode_reply(rbad), "W4 a decoded refusal that carries a chain is refused", ok);
        std::vector<std::byte> issued_empty = {static_cast<std::byte>(kReplyKind), std::byte{0}, std::byte{0}, std::byte{0}};
        check(!decode_reply(issued_empty), "W4 a decoded Issued without a chain is refused", ok);
    }

    // W5
    {
        const auto m1 = *encode_reply(WireReply{WireStatus::Refused, {}});
        const auto m2 = *encode_reply(WireReply{WireStatus::Issued, bytes(700, 9)});
        std::vector<std::byte> stream = frame_message(m1);
        const auto f2 = frame_message(m2);
        stream.insert(stream.end(), f2.begin(), f2.end());
        for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{64}, stream.size()}) {
            MessageReader rd;
            std::vector<std::vector<std::byte>> got;
            std::vector<std::byte> msg;
            for (std::size_t at = 0; at < stream.size(); at += chunk) {
                const std::size_t n = std::min(chunk, stream.size() - at);
                if (!rd.feed(std::span<const std::byte>(stream).subspan(at, n))) break;
                while (rd.next(msg)) got.push_back(msg);
            }
            check(got.size() == 2 && got[0] == m1 && got[1] == m2, "W5 whole messages across arbitrary chunking", ok);
        }
        MessageReader big;
        std::vector<std::byte> prefix;
        wire::detail::put_le(prefix, kMaxMessageBytes + 1, 4);
        check(!big.feed(prefix) && big.failed(), "W5 an absurd length prefix fails the reader", ok);
        std::vector<std::byte> more = bytes(4, 0);
        check(!big.feed(more), "W5 and it stays failed", ok);
        // A valid prefix (the largest allowed message) followed by more bytes than one framed message can hold: the buffer must stop growing.
        MessageReader flood;
        std::vector<std::byte> pre;
        wire::detail::put_le(pre, kMaxMessageBytes, 4);
        check(flood.feed(pre), "W5 a prefix declaring the largest allowed message is accepted", ok);
        bool grew = true;
        for (std::size_t sent = 0; sent < 2 * kMaxMessageBytes && grew; sent += 1000) grew = flood.feed(bytes(1000, 0xEE));
        check(!grew && flood.failed(), "W5 an endless stream after a valid prefix cannot grow the buffer past one framed message", ok);
    }

    std::printf(ok ? "enrol_wire_test: all checks passed\n" : "enrol_wire_test: FAILED\n");
    return ok ? 0 : 1;
}
