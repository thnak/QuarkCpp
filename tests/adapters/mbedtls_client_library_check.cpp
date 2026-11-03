// ADR-080 S3c: the HELLO-wire client transport composed with the mTLS engine, end to end. A client-role
// device (CN prefix `quark-client:`) dials a REAL TcpTransport + SecureTransport member over the HELLO
// wire, performs the mbedTLS mutual handshake carried in Authenticate frames, and exchanges frames both
// ways. Control: a device certificate from a DIFFERENT root is refused at the handshake, with no session
// and no frame on either side.
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <mbedtls/pk.h>

#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"
#include "quark/client/hello_client_transport.hpp"
#include "quark/client/socket_byte_stream.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"
#include "test_pki.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
constexpr std::uint64_t kCluster = 42;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

MessageFrame data(NodeId from, NodeId to, const char* text) {
    MessageFrame f{};
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Data;
    f.target = ActorId{TypeKey{0xAAAA}, 1};
    f.msg_type = TypeKey{0xBBBB};
    for (const char* p = text; *p != '\0'; ++p) f.payload.push_back(static_cast<std::byte>(*p));
    return f;
}

// One member: TcpTransport (HELLO wire) under a SecureTransport driving the mbedTLS engine.
struct Member {
    NodeId self;
    net::TcpTransport tcp;
    MockCipher unused{0};
    SecureTransport sec;
    std::shared_ptr<const TlsIdentity> id_blob;
    std::shared_ptr<const TrustedRoots> trust_blob;
    IdentityMaterial identity;
    TrustStore trust;
    MbedtlsHandshakeEngineFactory factory;
    std::atomic<std::uint64_t> got{0};

    Member(NodeId node, TlsIdentity id, std::vector<std::byte> root)
        : self(node),
          tcp(node, pal::ipv4_loopback, 0),
          sec(tcp, unused, node),
          id_blob(std::make_shared<const TlsIdentity>(std::move(id))),
          trust_blob(std::make_shared<const TrustedRoots>(TrustedRoots{std::move(root)})),
          identity(id_blob),
          trust(trust_blob) {
        sec.enable_handshake(factory, ClusterId{kCluster}, identity, trust);
        sec.on_receive([this](MessageFrame) { got.fetch_add(1); });
        tcp.set_peer_down_hook([this](NodeId p) { sec.on_peer_disconnected(p); });
        sec.set_reset_hook([this](NodeId p) { tcp.reset_peer_connection(p); });
    }
    ~Member() { tcp.stop(); }
};

std::unique_ptr<client::SocketByteStream> open_stream(std::uint16_t port) {
    auto s = client::SocketByteStream::connect(pal::ipv4_loopback, port);
    if (!s) {
        std::fprintf(stderr, "SocketByteStream::connect failed\n");
        std::abort();
    }
    return std::move(*s);
}

struct Device {
    std::unique_ptr<client::SocketByteStream> stream;
    client::HelloClientTransport tx;
    MockCipher unused{0};
    SecureTransport sec;
    std::shared_ptr<const TlsIdentity> id_blob;
    std::shared_ptr<const TrustedRoots> trust_blob;
    IdentityMaterial identity;
    TrustStore trust;
    MbedtlsHandshakeEngineFactory factory;
    std::atomic<std::uint64_t> got{0};

    Device(std::uint16_t port, NodeId self, NodeId peer, test_pki::GeneratedCert leaf,
           std::vector<std::byte> root)
        : stream(open_stream(port)),
          tx(*stream, self, peer),
          sec(tx, unused, self),
          id_blob(std::make_shared<const TlsIdentity>(TlsIdentity{std::move(leaf.cert_der), std::move(leaf.key_der), {}})),
          trust_blob(std::make_shared<const TrustedRoots>(TrustedRoots{std::move(root)})),
          identity(id_blob),
          trust(trust_blob) {
        sec.enable_handshake(factory, ClusterId{kCluster}, identity, trust);
        sec.on_receive([this](MessageFrame) { got.fetch_add(1); });
    }
};
}  // namespace

int main() {
    test_pki::Rng rng;
    mbedtls_pk_context ca_key;
    mbedtls_pk_init(&ca_key);
    const test_pki::GeneratedCert ca = test_pki::make_ca(rng, ca_key, "quark-test-ca");

    const NodeId member{1};
    const NodeId device{kClientNodeBit | 7};
    check(is_client_node(device), "the device identity is client-role");

    const test_pki::GeneratedCert mleaf = test_pki::make_leaf(rng, ca, ca_key, kCluster, member.value);
    const test_pki::GeneratedCert dleaf =
        test_pki::make_leaf_cn(rng, ca, ca_key, format_quark_cn(ClusterId{kCluster}, device), device.value);

    Member srv(member, TlsIdentity{mleaf.cert_der, mleaf.key_der, {}}, ca.cert_der);
    check(srv.tcp.start(), "the member's HELLO-wire listener starts");

    // ---- admitted client-role device over mTLS + HELLO ------------------------------------------
    {
        Device dev(srv.tcp.listen_port(), device, member, dleaf, ca.cert_der);
        check(dev.tx.start(), "the device completes the HELLO exchange");
        const bool delivered = wait_until(
            [&] {
                if (srv.got.load() >= 1) return true;
                dev.sec.send(member, data(device, member, "tls hello"));
                sleep_ms(15);
                return false;
            },
            8000);
        check(delivered, "the mTLS handshake completes and the device's frame reaches the member");
        check(dev.sec.session_count() == 1 && srv.sec.session_count() == 1,
              "both ends installed exactly one session");

        const bool replied = wait_until(
            [&] {
                if (dev.got.load() >= 1) return true;
                srv.sec.send(device, data(member, device, "tls reply"));
                sleep_ms(15);
                return false;
            },
            8000);
        check(replied, "the member's reply reaches the device");
    }

    // ---- CONTROL: a device certificate from a different root is refused -------------------------
    {
        mbedtls_pk_context other_key;
        mbedtls_pk_init(&other_key);
        const test_pki::GeneratedCert other_ca = test_pki::make_ca(rng, other_key, "other-ca");
        const NodeId rogue{kClientNodeBit | 9};
        const test_pki::GeneratedCert bad =
            test_pki::make_leaf_cn(rng, other_ca, other_key, format_quark_cn(ClusterId{kCluster}, rogue), rogue.value);
        const std::size_t sessions_before = srv.sec.session_count();
        const std::uint64_t got_before = srv.got.load();
        Device bad_dev(srv.tcp.listen_port(), rogue, member, bad, ca.cert_der);  // trusts the MEMBER's root
        check(bad_dev.tx.start(), "CONTROL: the rogue completes the HELLO exchange");
        for (int i = 0; i < 10; ++i) {
            bad_dev.sec.send(member, data(rogue, member, "wrong root"));
            sleep_ms(30);
        }
        sleep_ms(200);
        // The member's verdict is authoritative: TLS 1.3 lets the CLIENT reach Done before the server's
        // alert arrives, so only the member side is asserted.
        check(srv.sec.session_count() == sessions_before, "CONTROL: the member installs no session for it");
        check(srv.got.load() == got_before, "CONTROL: no frame from the wrong-root device reaches the member");
        mbedtls_pk_free(&other_key);
    }

    mbedtls_pk_free(&ca_key);
    std::printf("mbedtls_client_library_check: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
