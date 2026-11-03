// ADR-080 slice S2: the portable device client composed with SecureTransport, end to end against a REAL
// TcpTransport + SecureTransport member on loopback. The device is a client-role NodeId (ADR-074 D3) and
// always initiates; the member's pinned listener accepts the anonymous Authenticate opener (ADR-072).
// Proves: a client-role handshake completes, data flows both ways, an un-admitted client gets no session
// (control for the admission gate), and the client never sends a HELLO or a NodeId before the handshake.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "pinned_tcp_util.hpp"
#include "quark/client/pinned_client_transport.hpp"
#include "quark/core/secure_transport.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {

void sleep_for(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A blocking ByteStream over a non-blocking PAL socket. The application supplies its own (Android/iOS);
// this is the test's minimal desktop stand-in, and it deliberately uses no PAL reactor: retry + sleep.
class PalByteStream final : public client::ByteStream {
public:
    explicit PalByteStream(std::uint16_t port) {
        if (auto fd = pal::tcp_connect(pal::ipv4_loopback, port)) fd_.store(*fd);
        else closed_.store(true);
        sleep_for(20);  // loopback: the connect completes; a real app would block in connect()
    }
    ~PalByteStream() override { close(); }

    std::ptrdiff_t read(std::span<std::byte> out) override {
        for (;;) {
            const pal::fd_t fd = fd_.load(std::memory_order_acquire);
            if (closed_.load(std::memory_order_acquire) || fd == pal::invalid_fd) return 0;
            auto r = pal::recv_some(fd, out.data(), out.size());
            if (r) return static_cast<std::ptrdiff_t>(*r);
            if (r.error() != pal::would_block()) return -1;
            sleep_for(1);
        }
    }

    std::ptrdiff_t write(std::span<const std::byte> in) override {
        std::size_t off = 0;
        while (off < in.size()) {
            const pal::fd_t fd = fd_.load(std::memory_order_acquire);
            if (closed_.load(std::memory_order_acquire) || fd == pal::invalid_fd) return -1;
            auto w = pal::send_some(fd, in.data() + off, in.size() - off);
            if (w) off += *w;
            else if (w.error() == pal::would_block()) sleep_for(1);
            else return -1;
        }
        return static_cast<std::ptrdiff_t>(in.size());
    }

    void close() noexcept override {
        closed_.store(true, std::memory_order_release);
        const pal::fd_t fd = fd_.exchange(pal::invalid_fd);
        if (fd != pal::invalid_fd) pal::close_fd(fd);
    }

private:
    std::atomic<pal::fd_t> fd_{pal::invalid_fd};
    std::atomic<bool> closed_{false};
};

}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    const ClusterId cluster = kPinnedTcpCluster;

    Node member;                                              // member role
    Node device{PinnedOptions{}, nullptr, PinnedRole::Client};  // client role: bit 63 set
    pair_up(device, member, cluster);
    check(is_client_node(device.id) && !is_client_node(member.id),
          "the device id carries the client bit and the member does not", ok);

    Stack srv(member, /*pinned_wire=*/true);
    check(srv.start(), "the member's pinned listener starts", ok);

    // ---- admitted client-role device -------------------------------------------------------------
    PalByteStream stream(srv.tcp.listen_port());
    client::PinnedClientTransport dtrans(stream, device.id, member.id);
    MockCipher unused(0);
    SecureTransport dsec(dtrans, unused, device.id);
    auto id_blob = std::make_shared<const TlsIdentity>();
    auto trust_blob = std::make_shared<const TrustedRoots>();
    IdentityMaterial identity(id_blob);
    TrustStore trust(trust_blob);
    dsec.enable_handshake(*device.factory, cluster, identity, trust);

    std::mutex got_mu;
    std::vector<MessageFrame> at_device;
    dsec.on_receive([&](MessageFrame f) {
        std::lock_guard<std::mutex> g(got_mu);
        at_device.push_back(std::move(f));
    });

    // Send the first frame BEFORE starting the reader: it opens the handshake (the first send triggers
    // Msg1), and the socket is connected by the time write() returns. Then start the receive loop.
    dsec.send(member.id, data(device.id, member.id, "hello from the device"));
    dtrans.start();

    bool delivered = wait_for([&] {
        if (srv.got.load() >= 1) return true;
        dsec.send(member.id, data(device.id, member.id, "hello from the device"));
        sleep_for(15);
        return false;
    });
    check(delivered, "the client-role handshake completes and the device's frame reaches the member", ok);
    check(dsec.session_count() == 1 && srv.sec.session_count() == 1,
          "both ends installed exactly one session", ok);

    // Member -> device on the same inbound connection (the member has no endpoint for the device).
    const bool replied = wait_for([&] {
        {
            std::lock_guard<std::mutex> g(got_mu);
            if (!at_device.empty()) return true;
        }
        srv.sec.send(device.id, data(member.id, device.id, "reply from home"));
        sleep_for(15);
        return false;
    });
    check(replied, "the member's reply reaches the device", ok);
    {
        std::lock_guard<std::mutex> g(got_mu);
        if (!at_device.empty()) {
            const MessageFrame& f = at_device.front();
            const std::string text(reinterpret_cast<const char*>(f.payload.data()), f.payload.size());
            check(f.from == member.id && f.to == device.id, "the device sees the member as the peer", ok);
            check(text == "reply from home", "the reply payload survives the sealed envelope", ok);
        }
    }

    // ---- CONTROL: an un-admitted client-role device is refused ------------------------------------
    {
        Node rogue{PinnedOptions{}, nullptr, PinnedRole::Client};
        (void)rogue.book->admit(cluster, member.pub);  // the rogue trusts the member; the member does not admit it
        PalByteStream rstream(srv.tcp.listen_port());
        client::PinnedClientTransport rtrans(rstream, rogue.id, member.id);
        SecureTransport rsec(rtrans, unused, rogue.id);
        IdentityMaterial rid(id_blob);
        TrustStore rtrust(trust_blob);
        rsec.enable_handshake(*rogue.factory, cluster, rid, rtrust);
        std::atomic<int> rgot{0};
        rsec.on_receive([&](MessageFrame) { rgot.fetch_add(1); });

        rsec.send(member.id, data(rogue.id, member.id, "let me in"));
        rtrans.start();
        sleep_for(500);
        check(rsec.session_count() == 0, "CONTROL: an un-admitted client-role device gets no session", ok);
        check(srv.sec.session_count() == 1, "CONTROL: the member installs no session for it", ok);
        check(rgot.load() == 0, "CONTROL: it receives no frame", ok);
        rtrans.stop();
    }

    dtrans.stop();
    std::printf("pinned_client_library_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
