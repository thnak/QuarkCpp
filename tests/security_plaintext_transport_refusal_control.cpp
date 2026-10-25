// F1 NEGATIVE CONTROL — built WITHOUT QUARK_ALLOW_INSECURE (the test suite's dev builds define it).
//
// A bare plaintext TcpTransport must refuse start(); a SecureTransport-wrapped inner must be allowed
// to start (the wrapper marks it authenticated), and the plaintext transport must still fail the
// Strict multi-node posture check even when the explicit dev setter is used. This is the structural
// gate that catches an application which forgets validate_startup_security.
#include <cstdio>

#include "quark/core/aead.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/security.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // 1. Production default: bare plaintext refuses to start.
    {
        net::TcpTransport tx{NodeId{1}, pal::ipv4_loopback, 0};
        check(!tx.start(), "bare plaintext TcpTransport refuses start() by default", ok);
        check(tx.plaintext_start_refused(), "the refusal is reported (plaintext_start_refused)", ok);
        check(tx.is_plaintext(), "the transport still identifies itself as plaintext", ok);
        check(!validate_startup_security(SecurityMode::Strict, 3, tx).has_value(),
              "Strict + multi-node + plaintext is still rejected", ok);
    }

    // 2. Explicit dev opt-in starts (and still fails the Strict posture check).
    {
        net::TcpTransport tx{NodeId{1}, pal::ipv4_loopback, 0};
        tx.set_allow_insecure_plaintext(true);
        check(tx.start(), "explicit set_allow_insecure_plaintext(true) starts", ok);
        check(!validate_startup_security(SecurityMode::Strict, 3, tx).has_value(),
              "the dev opt-in does not make the transport Strict-safe", ok);
        tx.stop();
    }

    // 3. An authenticating wrapper (SecureTransport) is allowed to start its plaintext inner.
    {
        net::TcpTransport inner{NodeId{1}, pal::ipv4_loopback, 0};
        MockCipher cipher(0xF1C0DE);
        SecureTransport secure(inner, cipher, NodeId{1});
        check(!secure.is_plaintext(), "SecureTransport reports non-plaintext", ok);
        check(inner.start(), "SecureTransport marks the inner so it may start", ok);
        check(validate_startup_security(SecurityMode::Strict, 3, secure).has_value(),
              "Strict + multi-node + SecureTransport is allowed", ok);
        inner.stop();
    }

    std::printf("security_plaintext_transport_refusal_control: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
