// #81: admission + isolation over the stack AeroCoWorker will actually build — real TCP, pinned wire (no
// HELLO), the real Noise IK factory, an AdmissionAuthority on BOTH SecureTransport and TcpTransport, and two
// cluster instances of one device running under the SAME Ed25519 key.
//   C3/T1  a live session denied by a new generation is closed and its peer delivers nothing; a reconnect is
//          refused while denied and succeeds after a later generation re-admits it (positive control).
//   C5/T4  every delivered frame carries the generation its session was admitted under (never the wire's).
//   C6/T5  LogExchangeOnly passes the Control lane and refuses Data, counted; a later Admit lifts it.
//   ADR-073 T1/T2/T4  two clusters, one device key: denial in one leaves the other untouched; the reverse
//          too; a cluster-A instance dialing cluster B's endpoint gets no session on either side.
// T3 (cross-cluster frame injection) stays with multi_cluster_isolation_test: it is an AAD/session property,
// not a transport one, and the pinned keys additionally bind the ClusterId in the handshake (proven here by T4).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#include "pinned_test_util.hpp"
#include "pinned_tcp_util.hpp"
#include "quark/core/admission.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
std::shared_ptr<const AdmissionSnapshot> snap(std::uint64_t gen, std::initializer_list<NodeId> admit,
                                              std::initializer_list<NodeId> log_only = {}) {
    AdmissionSnapshot::Builder b(gen);
    for (NodeId n : admit) b.admit(n);
    for (NodeId n : log_only) b.log_exchange_only(n);
    return b.build();
}

// One node of one cluster instance over real TCP + the pinned wire, with admission on both layers.
struct PStack {
    Node n;
    AdmissionAuthority auth;  // declared before sec: it must outlive its listeners
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>();
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>();
    net::TcpTransport tcp;
    MockCipher unused{0};
    SecureTransport sec;
    IdentityMaterial identity{id_blob};
    TrustStore trust{trust_blob};
    std::atomic<std::uint64_t> got{0};
    std::mutex mu;
    std::vector<NodeId> closed;
    std::vector<MessageFrame> frames;

    PStack(PinnedIdentity id, ClusterId c)
        : n(std::move(id), PinnedOptions{}, nullptr), tcp(n.id, pal::ipv4_loopback, 0), sec(tcp, unused, n.id) {
        tcp.set_pinned_wire(true);
        sec.enable_handshake(*n.factory, c, identity, trust);
        sec.set_handshake_timeout(300'000'000);  // keep deny -> re-admit retries fast in the test
        sec.on_receive([this](MessageFrame f) {
            std::lock_guard<std::mutex> g(mu);
            frames.push_back(std::move(f));
            got.fetch_add(1);
        });
        sec.set_admission(auth, [this](NodeId p) {
            std::lock_guard<std::mutex> g(mu);
            closed.push_back(p);
            tcp.reset_peer_connection(p);
        });
        tcp.set_admission(auth);
        tcp.set_peer_down_hook([this](NodeId p) { sec.on_peer_disconnected(p); });
    }
    ~PStack() { tcp.stop(); }
    bool start() { return tcp.start(); }
    [[nodiscard]] std::uint16_t port() const { return tcp.listen_port(); }
    void dial(NodeId peer, std::uint16_t p) { tcp.add_peer(loopback_endpoint(peer, p)); }
    void peer_hook(NodeId peer) { sec.set_inbound_session_peer(peer); }
    bool publish(std::uint64_t gen, std::initializer_list<NodeId> admit,
                 std::initializer_list<NodeId> log_only = {}) {
        return auth.publish(snap(gen, admit, log_only));
    }
    [[nodiscard]] std::size_t closed_count() {
        std::lock_guard<std::mutex> g(mu);
        return closed.size();
    }
    [[nodiscard]] std::uint64_t last_generation() {
        std::lock_guard<std::mutex> g(mu);
        return frames.empty() ? 0 : frames.back().admission_generation;
    }
};

MessageFrame mk(NodeId from, NodeId to, FrameKind kind, std::string_view text) {
    MessageFrame f = data(from, to, text);
    f.kind = kind;
    return f;
}

bool deliver(PStack& tx, PStack& rx, std::uint64_t want, std::string_view text = "p", int ms = 4000) {
    const std::uint64_t base = rx.got.load();
    return wait_for(
        [&] {
            if (rx.got.load() >= base + want) return true;
            tx.sec.send(rx.n.id, data(tx.n.id, rx.n.id, text));
            sleep_ms(15);
            return false;
        },
        ms);
}
}  // namespace

int main() {
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    };
    if (!noise::init()) return 1;

    // ONE device key, TWO instances (cluster A and cluster B) — the exact shape the HAL needs.
    auto seed_identity = PinnedIdentity::generate();
    std::array<std::byte, PinnedIdentity::kSecretKeyBytes> device_sk{};
    std::copy(seed_identity.secret_key().begin(), seed_identity.secret_key().end(), device_sk.begin());
    auto device_a = PinnedIdentity::from_secret_key(device_sk);
    auto device_b = PinnedIdentity::from_secret_key(device_sk);
    auto device_4 = PinnedIdentity::from_secret_key(device_sk);
    check(device_a && device_b && device_4, "the same Ed25519 key backs three instances");
    if (!ok) return 1;

    const ClusterId A{0xAAAA}, B{0xBBBB};
    PStack xa(std::move(*device_a), A), ya(PinnedIdentity::generate(), A);
    PStack xb(std::move(*device_b), B), yb(PinnedIdentity::generate(), B);
    PStack x4(std::move(*device_4), A), y5(PinnedIdentity::generate(), B);
    pair_up(xa.n, ya.n, A);
    pair_up(xb.n, yb.n, B);
    // For T4: x4 (cluster A) trusts y5 in A; y5 (cluster B) trusts x4 in B. Nothing else differs.
    (void)x4.n.book->admit(A, y5.n.pub, y5.n.role);
    (void)y5.n.book->admit(B, x4.n.pub, x4.n.role);

    check(xa.n.id.value == xb.n.id.value && xa.n.id.value == x4.n.id.value,
          "both instances of the device share one NodeId");
    xa.peer_hook(ya.n.id);
    ya.peer_hook(xa.n.id);
    xb.peer_hook(yb.n.id);
    yb.peer_hook(xb.n.id);
    x4.peer_hook(y5.n.id);

    check(xa.start() && ya.start() && xb.start() && yb.start() && x4.start() && y5.start(),
          "all pinned listeners start");
    xa.dial(ya.n.id, ya.port());
    xb.dial(yb.n.id, yb.port());
    x4.dial(y5.n.id, y5.port());

    // ---- T1: admitted in A, denied in B ------------------------------------------------
    check(xa.publish(1, {ya.n.id}) && ya.publish(1, {xa.n.id}), "A: both admit each other (gen 1)");
    check(xb.publish(1, {yb.n.id}) && yb.publish(1, {}), "B: X is denied (gen 1)");
    xa.sec.open(ya.n.id);
    check(deliver(xa, ya, 1, "A1"), "T1: admitted X opens and delivers in A");
    check(ya.sec.session_count() == 1 && xa.sec.session_count() == 1, "T1: A has one session per end");

    xb.sec.open(yb.n.id);
    sleep_ms(400);
    check(xb.sec.session_count() == 0 && yb.sec.session_count() == 0 && yb.got.load() == 0,
          "T1: denied in B — no session on either end, nothing delivered");

    // ---- C5/T4: the delivered frame carries the receiver's admission generation ---------
    check(ya.publish(2, {xa.n.id}), "C5: the receiver re-admits X under generation 2");
    check(deliver(xa, ya, 1, "A2", 2000), "C5: traffic still flows under generation 2");
    check(ya.last_generation() == 2, "C5: the delivered frame carries the generation it was admitted under");

    // ---- T1 positive control: B admits X and the session forms ---------------------------
    check(yb.publish(2, {xb.n.id}), "T1 control: B admits X (gen 2)");
    xb.sec.open(yb.n.id);
    check(deliver(xb, yb, 1, "B1"), "T1 control: X connects and delivers in B once admitted");
    check(xb.sec.session_count() == 1 && yb.sec.session_count() == 1, "T1 control: B now has a session");

    // ---- C6/T5: log-exchange-only on the Control lane (on the live session) ------------
    check(ya.publish(3, {}, {xa.n.id}), "C6: A restricts X to log exchange (gen 3)");
    const std::uint64_t c6_base = ya.got.load();
    const std::uint64_t c6_refused = ya.sec.admission_stats().lane_refused;
    xa.sec.send(ya.n.id, mk(xa.n.id, ya.n.id, FrameKind::Data, "data"));
    xa.sec.send(ya.n.id, mk(xa.n.id, ya.n.id, FrameKind::Control, "ctl"));
    check(wait_for([&] { return ya.got.load() >= c6_base + 1; }, 3000), "C6: the Control frame passes");
    // The Control lane is a separate transport lane, so it can overtake the Data frame: wait for the gate.
    check(wait_for([&] { return ya.sec.admission_stats().lane_refused > c6_refused; }, 3000),
          "C6: the Data frame is refused and counted");
    sleep_ms(200);
    check(ya.got.load() == c6_base + 1, "C6: only the Control frame was delivered");
    check(ya.sec.session_count() == 1, "C6: log-only does not close the session");
    check(ya.publish(4, {xa.n.id}), "C6: A admits X again (gen 4)");
    check(deliver(xa, ya, 1, "A5", 4000), "C6: the Data lane flows again after the later Admit");

    // ---- T2: evict in A leaves B alone (last: no reconnection is needed afterwards) ----
    check(ya.publish(5, {}), "T2: A evicts X (gen 5)");
    check(deliver(xb, yb, 1, "B2", 3000), "T2: B is unaffected by A's eviction");
    check(xa.sec.session_count() == 0 && ya.sec.session_count() == 0, "T2: A's session is closed");
    check(ya.closed_count() >= 1, "T2: A closed the inner connection of the denied peer");
    check(!deliver(xa, ya, 1, "A3", 800), "T2 control: after the eviction A delivers nothing from X");

    // ---- ADR-073 T4: a cluster-A instance dials a cluster-B endpoint ------------------
    // Both sides admit the peer, so only the ClusterId differs.
    check(x4.publish(1, {y5.n.id}) && y5.publish(1, {x4.n.id}), "T4: both sides admit the peer");
    const std::uint64_t before = y5.got.load();
    x4.sec.open(y5.n.id);
    sleep_ms(600);
    check(x4.sec.session_count() == 0, "T4: the cluster-A instance forms no session");
    check(y5.sec.session_count() == 0, "T4: the cluster-B endpoint forms none either");
    check(y5.got.load() == before, "T4: nothing is delivered across clusters");

    std::printf("pinned_admission_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
