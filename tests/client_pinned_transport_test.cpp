// ADR-080 slice S1: the portable device client's frame transport on the ADR-072 pinned wire, over an
// in-memory blocking ByteStream (no sockets, no PAL reactor). Two-way delivery with the fixed-peer
// identity restored, zero NodeIds on the wire, and a malformed frame closing the transport.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "quark/client/pinned_client_transport.hpp"
#include "quark/core/ids.hpp"

using namespace quark;

namespace {

struct Pipe {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::byte> device_to_member;
    std::deque<std::byte> member_to_device;
    bool closed = false;
    std::vector<std::byte> last_wire;  // last frame bytes written, for the zero-id assertion
};

class End final : public client::ByteStream {
public:
    End(Pipe& p, bool device) : p_(p), device_(device) {}

    std::ptrdiff_t read(std::span<std::byte> out) override {
        std::unique_lock<std::mutex> g(p_.mu);
        std::deque<std::byte>& in = device_ ? p_.member_to_device : p_.device_to_member;
        p_.cv.wait(g, [&] { return p_.closed || !in.empty(); });
        if (in.empty()) return 0;
        const std::size_t n = std::min(out.size(), in.size());
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = in.front();
            in.pop_front();
        }
        return static_cast<std::ptrdiff_t>(n);
    }

    std::ptrdiff_t write(std::span<const std::byte> in) override {
        std::lock_guard<std::mutex> g(p_.mu);
        if (p_.closed) return -1;
        std::deque<std::byte>& out = device_ ? p_.device_to_member : p_.member_to_device;
        out.insert(out.end(), in.begin(), in.end());
        p_.last_wire.assign(in.begin(), in.end());
        p_.cv.notify_all();
        return static_cast<std::ptrdiff_t>(in.size());
    }

    void close() noexcept override {
        {
            std::lock_guard<std::mutex> g(p_.mu);
            p_.closed = true;
        }
        p_.cv.notify_all();
    }

private:
    Pipe& p_;
    bool device_;
};

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

std::uint32_t read_u32_le(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

}  // namespace

int main() {
    bool ok = true;
    const NodeId device{kClientNodeBit | 7};  // client role: the device is always the initiator
    const NodeId member{1};

    Pipe pipe;
    End device_end(pipe, true), member_end(pipe, false);
    client::PinnedClientTransport to_member(device_end, device, member);
    client::PinnedClientTransport to_device(member_end, member, device);

    std::mutex got_mu;
    std::vector<MessageFrame> got_member, got_device;
    // `to_member` is the DEVICE end: it receives the member's frames. `to_device` is the member end.
    to_member.on_receive([&](MessageFrame f) {
        std::lock_guard<std::mutex> g(got_mu);
        got_device.push_back(std::move(f));
    });
    to_device.on_receive([&](MessageFrame f) {
        std::lock_guard<std::mutex> g(got_mu);
        got_member.push_back(std::move(f));
    });
    to_member.start();
    to_device.start();

    // ---- device -> member -----------------------------------------------------------------------
    MessageFrame hello{};
    hello.from = device;
    hello.to = member;
    hello.kind = FrameKind::Data;
    hello.target.type = TypeKey{99};
    hello.msg_type = TypeKey{42};
    hello.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
    to_member.send(member, hello);

    check(wait_until(
              [&] {
                  std::lock_guard<std::mutex> g(got_mu);
                  return !got_member.empty();
              },
              std::chrono::milliseconds(2000)),
          "device -> member frame delivered", ok);
    {
        std::lock_guard<std::mutex> g(got_mu);
        if (!got_member.empty()) {
            const MessageFrame& f = got_member.front();
            check(f.from == device && f.to == member, "the receiver restores the fixed peer identity", ok);
            check(f.kind == FrameKind::Data && f.msg_type == TypeKey{42} && f.payload == hello.payload,
                  "kind/type/payload are preserved", ok);
        }
    }

    // ---- ADR-072: the pinned wire carries no NodeIds --------------------------------------------
    {
        std::lock_guard<std::mutex> g(pipe.mu);
        check(pipe.last_wire.size() >= 4, "a frame reached the wire", ok);
        if (pipe.last_wire.size() >= 4) {
            const std::uint32_t body = read_u32_le(pipe.last_wire.data());
            const bool fits = pipe.last_wire.size() == 4 + static_cast<std::size_t>(body);
            std::optional<MessageFrame> decoded;
            if (fits) decoded = net::decode_frame_body(std::span<const std::byte>(pipe.last_wire.data() + 4, body));
            check(decoded && decoded->from.value == 0 && decoded->to.value == 0,
                  "ADR-072: a direct frame carries from == to == 0 on the wire", ok);
        }
    }

    // ---- member -> device -----------------------------------------------------------------------
    MessageFrame reply{};
    reply.from = member;
    reply.to = device;
    reply.kind = FrameKind::Data;
    reply.payload = {std::byte{9}};
    to_device.send(device, reply);
    check(wait_until(
              [&] {
                  std::lock_guard<std::mutex> g(got_mu);
                  return !got_device.empty();
              },
              std::chrono::milliseconds(2000)),
          "member -> device frame delivered", ok);
    {
        std::lock_guard<std::mutex> g(got_mu);
        if (!got_device.empty()) {
            const MessageFrame& f = got_device.front();
            check(f.from == member && f.to == device && f.payload == reply.payload,
                  "the reverse direction restores ids and preserves the payload", ok);
        }
    }

    // ---- a malformed stream closes the transport (no silent desync) ----------------------------
    {
        std::lock_guard<std::mutex> g(pipe.mu);
        // An oversized length prefix (0xFFFFFFFF > max_frame_body) is a protocol error.
        for (int i = 0; i < 4; ++i) pipe.device_to_member.push_back(std::byte{0xFF});
        pipe.cv.notify_all();
    }
    check(wait_until([&] { return to_device.closed(); }, std::chrono::milliseconds(2000)),
          "an oversized length prefix closes the transport", ok);

    to_member.stop();
    to_device.stop();
    std::printf("client_pinned_transport_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
