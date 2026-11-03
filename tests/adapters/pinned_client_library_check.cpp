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
#include "quark/client/socket_byte_stream.hpp"
#include "quark/core/secure_transport.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {

void sleep_for(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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
    auto stream = client::SocketByteStream::connect(pal::ipv4_loopback, srv.tcp.listen_port());
    check(stream.has_value(), "the shipped blocking SocketByteStream connects", ok);
    if (!stream) {
        std::printf("pinned_client_library_check: FAIL (connect)\n");
        return 1;
    }
    client::PinnedClientTransport dtrans(**stream, device.id, member.id);
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

    // ---- reconnect: the APPLICATION owns the stream/transport/SecureTransport lifecycle -----------
    // ADR-080: reconnect/resume is not hidden in the library. The app drops the dead objects and builds
    // a new stream + transport + SecureTransport; the fresh handshake derives new keys, and replaying
    // its own event log from its last sequence number stays the app's job.
    dtrans.stop();
    {
        auto stream2 = client::SocketByteStream::connect(pal::ipv4_loopback, srv.tcp.listen_port());
        check(stream2.has_value(), "RECONNECT: a second blocking stream connects", ok);
        if (!stream2) return 1;
        client::PinnedClientTransport dtrans2(**stream2, device.id, member.id);
        SecureTransport dsec2(dtrans2, unused, device.id);
        IdentityMaterial id2(id_blob);
        TrustStore trust2(trust_blob);
        dsec2.enable_handshake(*device.factory, cluster, id2, trust2);
        std::mutex mu2;
        std::vector<MessageFrame> at_device2;
        dsec2.on_receive([&](MessageFrame f) {
            std::lock_guard<std::mutex> g(mu2);
            at_device2.push_back(std::move(f));
        });

        const std::uint64_t before = srv.got.load();
        dsec2.send(member.id, data(device.id, member.id, "after reconnect"));
        dtrans2.start();
        const bool reconnected = wait_for([&] {
            if (srv.got.load() >= before + 1) return true;
            dsec2.send(member.id, data(device.id, member.id, "after reconnect"));
            sleep_for(15);
            return false;
        });
        check(reconnected, "RECONNECT: a fresh stream + transport + handshake delivers again", ok);
        check(dsec2.session_count() == 1, "RECONNECT: the new client has a session", ok);
        check(wait_for([&] {
                  srv.sec.send(device.id, data(member.id, device.id, "reply after reconnect"));
                  sleep_for(15);
                  std::lock_guard<std::mutex> g(mu2);
                  return !at_device2.empty();
              }),
              "RECONNECT: the member's reply reaches the new client instance", ok);
        dtrans2.stop();
    }

    // ---- CONTROL: an un-admitted client-role device is refused ------------------------------------
    {
        Node rogue{PinnedOptions{}, nullptr, PinnedRole::Client};
        (void)rogue.book->admit(cluster, member.pub);  // the rogue trusts the member; the member does not admit it
        auto rstream = client::SocketByteStream::connect(pal::ipv4_loopback, srv.tcp.listen_port());
        check(rstream.has_value(), "the rogue's blocking stream connects", ok);
        if (!rstream) return 1;
        client::PinnedClientTransport rtrans(**rstream, rogue.id, member.id);
        SecureTransport rsec(rtrans, unused, rogue.id);
        IdentityMaterial rid(id_blob);
        TrustStore rtrust(trust_blob);
        rsec.enable_handshake(*rogue.factory, cluster, rid, rtrust);
        std::atomic<int> rgot{0};
        rsec.on_receive([&](MessageFrame) { rgot.fetch_add(1); });
        const std::size_t sessions_before = srv.sec.session_count();  // the device may have two after its reconnect

        rsec.send(member.id, data(rogue.id, member.id, "let me in"));
        rtrans.start();
        sleep_for(500);
        check(rsec.session_count() == 0, "CONTROL: an un-admitted client-role device gets no session", ok);
        check(srv.sec.session_count() == sessions_before, "CONTROL: the member installs no session for it", ok);
        check(rgot.load() == 0, "CONTROL: it receives no frame", ok);
        rtrans.stop();
    }

    dtrans.stop();
    std::printf("pinned_client_library_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
