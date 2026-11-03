// ADR-080 S3c: the HELLO-wire client transport against a REAL TcpTransport member on loopback. Proves the
// dialling side of TcpTransport's HELLO exchange: 14-byte HELLO both ways, version/node validation, the
// transport-internal DialHint consumed (never delivered to the application), and two-way frames with
// their real ids. Plaintext here on purpose: this test isolates the wire; the mTLS composition uses the
// same transport under SecureTransport.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "pal/net.hpp"
#include "quark/client/hello_client_transport.hpp"
#include "quark/client/socket_byte_stream.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

namespace {
void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

MessageFrame data(NodeId from, NodeId to, std::string_view text) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Data;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    for (char c : text) f.payload.push_back(static_cast<std::byte>(c));
    return f;
}

std::string text_of(const MessageFrame& f) {
    return std::string(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
}
}  // namespace

int main() {
    bool ok = true;
    const NodeId member{1};
    const NodeId device{kClientNodeBit | 7};

    net::TcpTransport srv(member, pal::ipv4_loopback, 0);
    srv.set_allow_insecure_plaintext(true);
    std::mutex got_mu;
    std::vector<MessageFrame> at_member;
    srv.on_receive([&](MessageFrame f) {
        std::lock_guard<std::mutex> g(got_mu);
        at_member.push_back(std::move(f));
    });
    check(srv.start(), "the member's HELLO-wire listener starts", ok);

    auto stream = client::SocketByteStream::connect(pal::ipv4_loopback, srv.listen_port());
    check(stream.has_value(), "the client's blocking stream connects", ok);
    if (!stream) return 1;

    client::HelloClientTransport tx(**stream, device, member);
    std::mutex got_tx_mu;
    std::vector<MessageFrame> at_device;
    std::atomic<int> dial_hints_at_device{0};
    tx.on_receive([&](MessageFrame f) {
        if (f.kind == FrameKind::DialHint) dial_hints_at_device.fetch_add(1);
        std::lock_guard<std::mutex> g(got_tx_mu);
        at_device.push_back(std::move(f));
    });
    check(tx.start(), "the HELLO exchange completes and validates the answering node", ok);
    check(tx.peer_hello_version() == net::hello_version, "the member advertised the current HELLO version", ok);

    // ---- device -> member -----------------------------------------------------------------------
    tx.send(member, data(device, member, "hello over the HELLO wire"));
    check(wait_until(
              [&] {
                  std::lock_guard<std::mutex> g(got_mu);
                  return !at_member.empty();
              },
              std::chrono::milliseconds(2000)),
          "the member receives the device's frame", ok);
    {
        std::lock_guard<std::mutex> g(got_mu);
        if (!at_member.empty()) {
            check(at_member.front().from == device && at_member.front().to == member,
                  "the HELLO wire carries the real ids (no zeroing, unlike the pinned wire)", ok);
            check(text_of(at_member.front()) == "hello over the HELLO wire", "the payload survives", ok);
        }
    }

    // ---- member -> device, and the DialHint stays transport-internal -----------------------------
    check(wait_until(
              [&] {
                  srv.send(device, data(member, device, "reply over the HELLO wire"));
                  std::lock_guard<std::mutex> g(got_tx_mu);
                  return !at_device.empty();
              },
              std::chrono::milliseconds(2000)),
          "the member's reply reaches the device", ok);
    {
        std::lock_guard<std::mutex> g(got_tx_mu);
        if (!at_device.empty()) {
            check(at_device.front().from == member && at_device.front().to == device,
                  "the device sees the member as the peer", ok);
            check(text_of(at_device.front()) == "reply over the HELLO wire", "the reply payload survives", ok);
        }
    }
    check(dial_hints_at_device.load() == 0,
          "the acceptor's DialHint is consumed by the transport, never delivered", ok);

    tx.stop();
    srv.stop();
    std::printf("client_hello_transport_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
