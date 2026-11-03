// Tests #78: a consumer's start-up self-check reads back the settings it installed instead of trusting
// that a setter took effect. Every object below is configured with non-default values and each field is
// compared to the value that was set, so a getter that returns its default (or ignores the setter) fails.
// The same check runs on a default-configured object as the control: the documented defaults read back.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>

#include "quark/core/aead.hpp"
#include "quark/core/admission.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/pinned_key.hpp"
#include "quark/core/revocation_registry.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// A transport that moves nothing: these checks only read configuration.
class NullTransport final : public Transport {
public:
    void send(NodeId, MessageFrame) override {}
    void on_receive(std::function<void(MessageFrame)>) override {}
};

// The seam's test double: create() is never called here, but is_test_double() must report it.
class MockFactory final : public HandshakeEngineFactory {
public:
    [[nodiscard]] std::unique_ptr<HandshakeEngine> create(
        bool, NodeId, NodeId, ClusterId, IdentityMaterial::Snapshot, TrustStore::Snapshot,
        std::shared_ptr<const std::unordered_set<Fingerprint>>) override {
        return nullptr;
    }
    [[nodiscard]] bool is_test_double() const noexcept override { return true; }
};

IdentityMaterial make_identity() {
    return IdentityMaterial(std::make_shared<const TlsIdentity>(TlsIdentity{}));
}
TrustStore make_trust() {
    return TrustStore(std::make_shared<const TrustedRoots>(TrustedRoots{}));
}

void tcp_posture() {
    bool ok = true;
    AdmissionAuthority auth;  // declared before the transport that listens to it
    net::TcpTransport tcp(NodeId{7}, pal::ipv4_loopback, 0);

    const net::TcpTransport::Posture d = tcp.posture();
    check(d.max_frame_body == net::max_frame_body && d.max_pending_inbound == 1024 &&
              d.max_pending_inbound_per_ip == 64 && d.max_inbound_per_ip == 64 &&
              d.pending_hello_timeout_ns == 5'000'000'000 && d.max_outbound_data_bytes == 64ull << 20 &&
              d.max_outbound_control_bytes == 256ull << 20 && !d.admission_bound && !d.relay_configured &&
              !d.allow_insecure_plaintext && !d.pinned_wire && !d.test_faults.legacy_dial_election &&
              !d.test_faults.skip_origin_check && !d.test_faults.suppress_dial_hint,
          "CONTROL: a default TcpTransport reads back its documented defaults", ok);

    tcp.set_max_frame_body(8192);
    tcp.set_max_pending_inbound(11);
    tcp.set_max_pending_inbound_per_ip(12);
    tcp.set_max_inbound_per_ip(13);
    tcp.set_pending_hello_timeout_ns(14);
    tcp.set_max_outbound_data_bytes(15);
    tcp.set_max_outbound_control_bytes(16);
    tcp.set_allow_insecure_plaintext(true);
    tcp.set_pinned_wire(true);
    net::TcpTestFaults faults;
    faults.legacy_dial_election = true;
    faults.skip_origin_check = true;
    faults.suppress_dial_hint = true;
    tcp.set_test_faults(faults);
    tcp.set_admission(auth);
    tcp.set_relay_via(NodeId{8}, {NodeId{9}});

    const net::TcpTransport::Posture p = tcp.posture();
    check(p.max_frame_body == 8192, "#78: TcpTransport max_frame_body reads back", ok);
    check(p.max_pending_inbound == 11, "#78: max_pending_inbound reads back", ok);
    check(p.max_pending_inbound_per_ip == 12, "#78: max_pending_inbound_per_ip reads back", ok);
    check(p.max_inbound_per_ip == 13, "#78: max_inbound_per_ip reads back", ok);
    check(p.pending_hello_timeout_ns == 14, "#78: pending_hello_timeout_ns reads back", ok);
    check(p.max_outbound_data_bytes == 15, "#78: max_outbound_data_bytes reads back", ok);
    check(p.max_outbound_control_bytes == 16, "#78: max_outbound_control_bytes reads back", ok);
    check(p.admission_bound, "#78: an attached admission authority reads back", ok);
    check(p.relay_configured, "#78: an installed relay hook reads back", ok);
    check(p.allow_insecure_plaintext, "#78: allow_insecure_plaintext reads back", ok);
    check(p.pinned_wire, "#78: pinned_wire reads back", ok);
    check(p.test_faults.legacy_dial_election && p.test_faults.skip_origin_check &&
              p.test_faults.suppress_dial_hint,
          "#78: TcpTestFaults reads back", ok);

    if (!ok) std::exit(1);
}

void secure_posture() {
    bool ok = true;
    AdmissionAuthority auth;
    NullTransport inner;
    MockCipher cipher{1};
    IdentityMaterial identity = make_identity();
    TrustStore trust = make_trust();
    MockFactory factory;
    SecureTransport secure(inner, cipher, NodeId{7});

    const SecureTransport::Posture d = secure.posture();
    check(!d.handshake_enabled && d.trust_mode == TrustMode::CaChain && !d.handshake_factory_is_test_double &&
              !d.admission_bound && !d.close_hook_set && !d.reset_hook_set && !d.audit_sink_set &&
              d.max_pending_handshakes == SecureTransport::kMaxPendingHandshakes && d.max_pending_per_ip == 64 &&
              d.reserved_handshake_slots == 16 && d.handshake_timeout_ns == 5'000'000'000 &&
              d.min_renegotiate_interval_ns == 250'000'000 &&
              d.max_frames_per_key == SecureTransport::kDefaultMaxFramesPerKey &&
              d.renegotiate_interval_ns == SecureTransport::kDefaultRenegotiateIntervalNs &&
              !d.admission_test_faults.skip_live_sweep && !d.test_faults.erase_session_on_answered_failure,
          "CONTROL: a default SecureTransport reads back its documented defaults", ok);

    secure.set_max_pending_handshakes(21);
    secure.set_max_pending_per_ip(22);
    secure.set_reserved_handshake_slots(23);
    secure.set_handshake_timeout(24);
    secure.set_min_renegotiate_interval(25);
    secure.set_rotation_policy(26, 27);
    AdmissionTestFaults af;
    af.skip_handshake_recheck = true;
    af.skip_live_sweep = true;
    af.skip_inbound_gate = true;
    secure.set_admission_test_faults(af);
    SecureTransportTestFaults sf;
    sf.erase_session_on_answered_failure = true;
    sf.erase_session_on_inbound_failure = true;
    sf.answer_opener_as_client = true;
    sf.no_renegotiate_bound = true;
    sf.omit_cluster_in_aad = true;
    secure.set_test_faults(sf);
    secure.set_reset_hook([](NodeId) {});
    secure.set_audit_sink(default_audit_sink());
    secure.set_admission(auth, [](NodeId) {});
    check(secure.enable_handshake(factory, ClusterId{99}, identity, trust),
          "#78 setup: a default-accepting factory enables the handshake", ok);

    const SecureTransport::Posture p = secure.posture();
    check(p.handshake_enabled, "#78: handshake enabled reads back", ok);
    check(p.trust_mode == TrustMode::CaChain, "#78: the factory's trust mode reads back", ok);
    check(p.handshake_factory_is_test_double, "#78: a test-double factory reads back as one", ok);
    check(p.cluster_id == ClusterId{99}, "#78: cluster id reads back", ok);
    check(p.max_pending_handshakes == 21, "#78: max_pending_handshakes reads back", ok);
    check(p.max_pending_per_ip == 22, "#78: max_pending_per_ip reads back", ok);
    check(p.reserved_handshake_slots == 23, "#78: reserved_handshake_slots reads back", ok);
    check(p.handshake_timeout_ns == 24, "#78: handshake_timeout_ns reads back", ok);
    check(p.min_renegotiate_interval_ns == 25, "#78: min_renegotiate_interval_ns reads back", ok);
    check(p.max_frames_per_key == 26 && p.renegotiate_interval_ns == 27, "#78: rotation policy reads back", ok);
    check(p.admission_bound, "#78: an attached admission authority reads back", ok);
    check(p.close_hook_set, "#78: an admission close hook reads back", ok);
    check(p.reset_hook_set, "#78: reset hook presence reads back", ok);
    check(p.audit_sink_set, "#78: audit sink presence reads back", ok);
    check(p.admission_test_faults.skip_handshake_recheck && p.admission_test_faults.skip_live_sweep &&
              p.admission_test_faults.skip_inbound_gate,
          "#78: AdmissionTestFaults reads back", ok);
    check(p.test_faults.erase_session_on_answered_failure && p.test_faults.erase_session_on_inbound_failure &&
              p.test_faults.answer_opener_as_client && p.test_faults.no_renegotiate_bound &&
              p.test_faults.omit_cluster_in_aad,
          "#78: SecureTransportTestFaults reads back", ok);

    if (!ok) std::exit(1);
}

void other_postures() {
    bool ok = true;

    MockCipher mock{1};
    check(mock.is_test_double(), "#78: MockCipher reports itself as a test double", ok);

    MockFactory factory;
    check(factory.is_test_double() && factory.trust_mode() == TrustMode::CaChain,
          "#78: a mock factory reports test-double and its trust mode", ok);

    RevocationRegistry registry;
    check(!registry.authority_only(), "CONTROL: a default registry is not authority-only", ok);
    registry.set_authority_only(true);
    check(registry.authority_only(), "#78: RevocationRegistry authority-only reads back", ok);

    NullTransport inner;
    SwimMembership::Config cfg;
    cfg.authority_only_revocation = true;
    cfg.control_rate_per_sec = 31;
    cfg.control_burst = 32;
    SwimMembership swim(NodeId{1}, inner, cfg);
    const SwimMembership::Posture sp = swim.posture();
    check(sp.authority_only_revocation && sp.control_rate_per_sec == 31 && sp.control_burst == 32,
          "#78: SwimMembership Config reads back", ok);

    if (!ok) std::exit(1);
}

}  // namespace

int main() {
    tcp_posture();
    secure_posture();
    other_postures();
    std::printf("security_posture_test: OK\n");
    return 0;
}
