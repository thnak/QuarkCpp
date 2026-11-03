// Tests #84: the frame-body cap (`max_frame_body`) covers the fixed 78-byte MessageFrame header, so a
// consumer cannot use the cap as its payload limit. Proves:
//   * `frame_body_header_size` is exactly what encode_frame_body writes before the payload;
//   * `max_payload_for_body_cap` / `max_sealed_payload_for_body_cap` track that layout, including
//     SecureTransport's [seq][ciphertext][tag] envelope;
//   * FrameStream accepts a body exactly at the cap and rejects one byte more;
//   * TcpTransport reports the limits and a frame carrying exactly `max_payload_bytes()` crosses a real
//     loopback socket intact (the reported failure: a valid payload was refused by the cap).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "quark/net/wire_codec.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::net;
using namespace quark::test;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

MessageFrame frame_with_payload(std::uint64_t trace, std::size_t payload_len) {
    MessageFrame f;
    f.target = ActorId{TypeKey{0x8484u}, trace};
    f.msg_type = TypeKey{0x99u};
    f.mode = WireMode::Tagged;
    f.kind = FrameKind::Data;
    f.trace_id = trace;
    f.payload.resize(payload_len);
    for (std::size_t i = 0; i < payload_len; ++i)
        f.payload[i] = static_cast<std::byte>((trace + i) & 0xFF);
    return f;
}

void byte_layer() {
    bool ok = true;
    constexpr std::uint32_t cap = 4096;
    constexpr std::uint32_t tag = 16;

    check(frame_body_header_size == 78, "the fixed frame header is 78 bytes", ok);
    check(encode_frame(frame_with_payload(1, 0)).size() - 4 == frame_body_header_size,
          "encoded body of an empty-payload frame is exactly the header", ok);
    check(max_payload_for_body_cap(cap) == cap - frame_body_header_size,
          "direct payload limit is the cap minus the header", ok);
    check(max_payload_for_body_cap(frame_body_header_size) == 0,
          "a cap equal to the header fits only an empty payload", ok);
    check(max_payload_for_body_cap(frame_body_header_size - 1) == 0,
          "a cap smaller than the header has no payload room", ok);
    check(max_sealed_payload_for_body_cap(cap, tag) == cap - frame_body_header_size - 8 - tag,
          "sealed payload limit subtracts the sequence prefix and the AEAD tag", ok);

    // Exact fit: at the cap FrameStream accepts; one byte more is a protocol error.
    const std::uint32_t direct_cap = max_payload_for_body_cap(cap);
    const std::vector<std::byte> exact = encode_frame(frame_with_payload(2, direct_cap));
    check(exact.size() - 4 == cap, "an exact-fit frame fills the body cap", ok);
    {
        FrameStream fs{cap};
        bool emitted = false;
        const bool clean = fs.feed(exact.data(), exact.size(), [&](MessageFrame f) {
            emitted = true;
            check(f.payload.size() == direct_cap, "exact-fit payload survives decode", ok);
        });
        check(clean && emitted, "a body exactly at the cap is accepted", ok);
    }
    {
        const std::vector<std::byte> over = encode_frame(frame_with_payload(3, direct_cap + 1));
        FrameStream fs{cap};
        check(!fs.feed(over.data(), over.size(), [&](MessageFrame) {}),
              "one byte over the cap is a protocol error", ok);
    }

    // Sealed: the body is header + 8-byte seq + ciphertext + tag.
    const std::uint32_t plain_cap = max_sealed_payload_for_body_cap(cap, tag);
    MessageFrame sealed = frame_with_payload(4, plain_cap);
    sealed.payload.insert(sealed.payload.begin(), sealed_seq_prefix_size, std::byte{0});
    sealed.payload.resize(sealed.payload.size() + tag);
    const std::vector<std::byte> sealed_wire = encode_frame(sealed);
    check(sealed_wire.size() - 4 == cap, "a sealed exact-fit frame fills the body cap", ok);
    {
        FrameStream fs{cap};
        bool emitted = false;
        const bool clean = fs.feed(sealed_wire.data(), sealed_wire.size(),
                                   [&](MessageFrame) { emitted = true; });
        check(clean && emitted, "a sealed body exactly at the cap is accepted", ok);
    }
    {
        MessageFrame sealed_over = frame_with_payload(5, plain_cap + 1);
        sealed_over.payload.insert(sealed_over.payload.begin(), sealed_seq_prefix_size, std::byte{0});
        sealed_over.payload.resize(sealed_over.payload.size() + tag);
        const std::vector<std::byte> wire = encode_frame(sealed_over);
        FrameStream fs{cap};
        check(!fs.feed(wire.data(), wire.size(), [&](MessageFrame) {}),
              "a sealed frame one byte over the cap is a protocol error", ok);
    }

    std::printf("  byte layer: %s\n", ok ? "OK" : "FAIL");
    if (!ok) std::exit(1);
}

void socket_layer() {
    bool ok = true;
    constexpr std::uint32_t cap = 2048;
    const NodeId A{1}, B{2};

    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    ta.set_max_frame_body(cap);
    tb.set_max_frame_body(cap);

    check(ta.max_frame_body() == cap, "the transport reports the configured body cap", ok);
    check(ta.max_payload_bytes() == cap - frame_body_header_size,
          "the transport reports the direct payload limit", ok);
    check(ta.max_sealed_payload_bytes(16) == cap - frame_body_header_size - 8 - 16,
          "the transport reports the sealed payload limit", ok);

    Recorder ra, rb;
    ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    check(ta.start() && tb.start(), "both listeners start", ok);
    if (!ok) {
        std::printf("  socket layer: FAIL (startup)\n");
        std::exit(1);
    }
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));

    const std::size_t payload_len = ta.max_payload_bytes();
    MessageFrame f = frame_with_payload(7, payload_len);
    f.from = A;
    f.to = B;
    ta.send(B, std::move(f));
    check(wait_until([&] { return rb.count() >= 1; }),
          "a frame carrying exactly max_payload_bytes() crosses the socket", ok);
    {
        std::lock_guard<std::mutex> g(rb.m);
        check(!rb.payloads.empty() && rb.payloads[0].size() == payload_len,
              "the exact-fit payload arrives intact", ok);
    }

    ta.stop();
    tb.stop();
    std::printf("  socket layer: %s\n", ok ? "OK" : "FAIL");
    if (!ok) std::exit(1);
}

}  // namespace

int main() {
    byte_layer();
    socket_layer();
    std::printf("tcp_frame_payload_cap_test: OK\n");
    return 0;
}
