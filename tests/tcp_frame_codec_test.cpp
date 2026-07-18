// Tests 010-Distribution wire framing (quark/net/wire_codec.hpp) with NO sockets — the pure byte layer.
// Two properties the TCP transport leans on:
//   (1) encode_frame / decode_frame_body round-trip EVERY MessageFrame header field + payload exactly.
//   (2) FrameStream reassembles TCP's boundary-less byte stream back into discrete frames under the
//       three real recv() shapes: a partial frame (wait), several coalesced frames (drain all), and a
//       byte-at-a-time dribble (emit exactly at each boundary). Plus: an oversized length prefix is
//       rejected as a protocol error, not blindly allocated.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/net/wire_codec.hpp"

using namespace quark;
using namespace quark::net;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

MessageFrame make_frame(std::uint64_t seq, std::size_t payload_len) {
    MessageFrame f;
    f.from = NodeId{seq * 7 + 1};
    f.to = NodeId{seq * 13 + 2};
    f.target = ActorId{TypeKey{0xABCD'0000u + seq}, seq * 1000 + 3};
    f.msg_type = TypeKey{0x1234'0000u + seq};
    f.mode = (seq % 2) ? WireMode::Tagless : WireMode::Tagged;
    f.kind = (seq % 3 == 0) ? FrameKind::Control : FrameKind::Data;
    f.deadline_ns = static_cast<std::int64_t>(seq) * -100 + 5;  // exercise a negative-ish value too
    f.trace_id = 0xDEAD'0000'0000'0000ull + seq;
    f.principal.subject = seq * 99 + 1;
    f.principal.rights = seq * 5;
    f.payload.resize(payload_len);
    for (std::size_t i = 0; i < payload_len; ++i)
        f.payload[i] = static_cast<std::byte>((seq + i) & 0xFF);
    return f;
}

bool frames_equal(const MessageFrame& a, const MessageFrame& b) {
    return a.from == b.from && a.to == b.to && a.target == b.target && a.msg_type == b.msg_type &&
           a.mode == b.mode && a.kind == b.kind && a.deadline_ns == b.deadline_ns &&
           a.trace_id == b.trace_id && a.principal == b.principal && a.payload == b.payload;
}
}  // namespace

int main() {
    bool ok = true;

    // ---- (1) field-exact round-trip, including a zero-length payload ----------------------------
    for (std::uint64_t seq = 0; seq < 8; ++seq) {
        const MessageFrame f = make_frame(seq, seq == 0 ? 0 : seq * 37);
        const std::vector<std::byte> bytes = encode_frame(f);
        // The record is [u32 len][body]; decode the body after the 4-byte prefix.
        check(bytes.size() >= 4, "encoded record has a length prefix", ok);
        auto g = decode_frame_body(std::span<const std::byte>(bytes.data() + 4, bytes.size() - 4));
        check(g.has_value(), "decode_frame_body accepts a well-formed body", ok);
        if (g) check(frames_equal(f, *g), "every header field + payload round-trips exactly", ok);
    }

    // ---- (2a) coalesced: three frames back-to-back in one buffer drain in order -----------------
    {
        std::vector<std::byte> wire;
        for (std::uint64_t s = 0; s < 3; ++s) {
            auto b = encode_frame(make_frame(s, 10 + s));
            wire.insert(wire.end(), b.begin(), b.end());
        }
        FrameStream fs;
        std::vector<std::uint64_t> got;
        const bool r = fs.feed(wire.data(), wire.size(),
                               [&](MessageFrame f) { got.push_back(f.trace_id); });
        check(r, "coalesced feed is clean", ok);
        check(got.size() == 3, "all three coalesced frames emitted", ok);
        check(fs.buffered() == 0, "nothing left buffered after a clean drain", ok);
        bool ordered = got.size() == 3;
        for (std::uint64_t s = 0; s < got.size(); ++s)
            if (got[s] != 0xDEAD'0000'0000'0000ull + s) ordered = false;
        check(ordered, "coalesced frames emitted in arrival order", ok);
    }

    // ---- (2b) partial: half a frame emits nothing; the rest completes it ------------------------
    {
        const std::vector<std::byte> wire = encode_frame(make_frame(42, 200));
        const std::size_t split = wire.size() / 2;
        FrameStream fs;
        int emitted = 0;
        check(fs.feed(wire.data(), split, [&](MessageFrame) { ++emitted; }), "partial feed clean", ok);
        check(emitted == 0, "a partial frame emits nothing", ok);
        check(fs.buffered() == split, "the partial bytes are buffered", ok);
        check(fs.feed(wire.data() + split, wire.size() - split, [&](MessageFrame f) {
            ++emitted;
            check(f.trace_id == 0xDEAD'0000'0000'0000ull + 42, "completed frame is the right one", ok);
        }), "completing feed clean", ok);
        check(emitted == 1, "the frame emits once completed", ok);
        check(fs.buffered() == 0, "buffer drained after completion", ok);
    }

    // ---- (2c) dribble: one byte at a time still emits exactly at the boundary --------------------
    {
        std::vector<std::byte> wire;
        for (std::uint64_t s = 0; s < 4; ++s) {
            auto b = encode_frame(make_frame(100 + s, 3 + s));
            wire.insert(wire.end(), b.begin(), b.end());
        }
        FrameStream fs;
        int emitted = 0;
        bool clean = true;
        for (std::byte by : wire) clean = clean && fs.feed(&by, 1, [&](MessageFrame) { ++emitted; });
        check(clean, "byte-at-a-time feed is clean throughout", ok);
        check(emitted == 4, "all four frames emitted across single-byte feeds", ok);
        check(fs.buffered() == 0, "no residue after dribbling exact frames", ok);
    }

    // ---- (2d) protocol error: an oversized length prefix is rejected, not allocated -------------
    {
        std::vector<std::byte> bad(4);
        const std::uint32_t huge = max_frame_body + 1;
        for (int i = 0; i < 4; ++i) bad[i] = static_cast<std::byte>((huge >> (8 * i)) & 0xFF);
        FrameStream fs;
        const bool r = fs.feed(bad.data(), bad.size(), [&](MessageFrame) {});
        check(!r, "an oversized length prefix is a protocol error (feed returns false)", ok);
    }

    std::printf("tcp_frame_codec_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
