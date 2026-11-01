// Tests issue #44 / ADR-073 (AeroCoWorker ADR-044 Q5): ONE process in SEVERAL independent clusters, with separate trust,
// authority, membership and session state and nothing leaking between them. Implements 020 §1a / 021 §1 as amended by ADR-073.
//
// A "cluster instance" is one TcpTransport + SecureTransport + AdmissionAuthority + handshake factory set (no process-global
// security state exists: the only statics are the message pool, thread-local reentrancy flags and the RNG, all resource sharing).
// The same two devices (NodeIds 1, 2) are in clusters A and B.
//   T1 admit in A, deny in B: X delivers in A; in B the handshake never completes (no session, nothing delivered).
//      CONTROL: B admits X -> it connects and delivers.
//   T2 evicting X in A (a snapshot that denies it) closes A's session and leaves B's untouched; and the reverse.
//      CONTROL: the A deny really took effect (A delivers 0 after it).
//   T3 a sealed frame captured in A and injected into B's established session is rejected (tamper), even though both sessions
//      are keyed IDENTICALLY here (the mock handshake derives keys from the NodeIds only, so only the cluster-bound AAD can stop it).
//      CONTROL: with the pre-fix AAD (omit_cluster_in_aad) the same injection is ACCEPTED.
//   T4 a peer of cluster A that dials cluster B's endpoint (wrong cluster id) gets no session on either side.
//   T5 cost per extra cluster, measured: threads and resident memory (printed; bounded loosely).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "admission_test_util.hpp"
#include "tcp_test_util.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

using namespace quark;
using namespace quark::test;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
template <class F>
bool wait_for(F f, int ms = 4000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f()) return true;
        sleep_ms(5);
    }
    return f();
}

// One node of one cluster instance over real TCP.
struct Member {
    AdmissionAuthority auth;  // declared first: an authority must outlive its listeners
    net::TcpTransport tcp;
    MockCipher unused{0};
    SecureTransport sec;
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>();
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>();
    IdentityMaterial identity{id_blob};
    TrustStore trust{trust_blob};
    MockHandshakeFactory factory;
    std::atomic<std::uint64_t> got{0};

    Member(NodeId self, ClusterId cluster, std::shared_ptr<const AdmissionSnapshot> initial)
        : tcp(self, pal::ipv4_loopback, 0), sec(tcp, unused, self) {
        sec.enable_handshake(factory, cluster, identity, trust);
        sec.on_receive([this](MessageFrame) { got.fetch_add(1); });
        sec.set_admission(auth);
        tcp.set_peer_down_hook([this](NodeId p) { sec.on_peer_disconnected(p); });
        (void)auth.publish(std::move(initial));
    }
    bool start() { return tcp.start(); }
    ~Member() { tcp.stop(); }
};

bool deliver(Member& tx, Member& rx, NodeId from, NodeId to, std::uint64_t want, int ms = 3000) {
    const std::uint64_t base = rx.got.load();
    return wait_for(
        [&] {
            if (rx.got.load() >= base + want) return true;
            tx.sec.send(to, make_frame(from, to));
            sleep_ms(15);
            return false;
        },
        ms);
}

std::size_t rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize;
    return 0;
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long a = 0, r = 0;
    if (std::fscanf(f, "%lu %lu", &a, &r) != 2) r = 0;
    std::fclose(f);
    return static_cast<std::size_t>(r) * 4096;
#endif
}

// In-memory pair of one cluster (for T3): n1 < n2, queued fabric, own authorities.
struct MemCluster {
    const NodeId n1{1}, n2{2};
    AdmissionAuthority a1, a2;
    QueuedFabric fabric;
    QueuedTransport i1{fabric, n1}, i2{fabric, n2};
    MockCipher unused{0};
    SecureTransport s1, s2;
    std::shared_ptr<const TlsIdentity> id_blob = std::make_shared<const TlsIdentity>();
    std::shared_ptr<const TrustedRoots> trust_blob = std::make_shared<const TrustedRoots>();
    IdentityMaterial identity1{id_blob}, identity2{id_blob};
    TrustStore trust1{trust_blob}, trust2{trust_blob};
    MockHandshakeFactory f1, f2;
    std::vector<MessageFrame> got2;

    explicit MemCluster(std::uint64_t cluster, bool omit_cluster_in_aad)
        : s1(i1, unused, n1), s2(i2, unused, n2) {
        SecureTransportTestFaults f;
        f.omit_cluster_in_aad = omit_cluster_in_aad;
        s1.set_test_faults(f);
        s2.set_test_faults(f);
        s1.enable_handshake(f1, ClusterId{cluster}, identity1, trust1);
        s2.enable_handshake(f2, ClusterId{cluster}, identity2, trust2);
        s1.on_receive([](MessageFrame) {});
        s2.on_receive([this](MessageFrame f) { got2.push_back(std::move(f)); });
        (void)a1.publish(snap(1, {n2}));
        (void)a2.publish(snap(1, {n1}));
        s1.set_admission(a1);
        s2.set_admission(a2);
    }
    void establish() {
        s1.send(n2, make_frame(n1, n2));
        fabric.pump_all();
    }
};

}  // namespace

int main() {
    const NodeId n1{1}, n2{2};
    const ClusterId A{0xAAAA}, B{0xBBBB};
    const std::size_t rss0 = rss_bytes();

    // ---- T1: admit in A, deny in B (same two devices, one process) ----
    {
        Member pa(n2, A, snap(1, {n1})), pb(n2, B, snap(1, {}, {}, {n1}));  // the process hosts n2 in both clusters
        Member xa(n1, A, snap(1, {n2})), xb(n1, B, snap(1, {n2}));         // peer n1 in both
        const bool up = pa.start() && pb.start() && xa.start() && xb.start();
        check(up, "T1: all four stacks start");
        xa.tcp.add_peer(loopback_endpoint(n2, pa.tcp.listen_port()));
        xb.tcp.add_peer(loopback_endpoint(n2, pb.tcp.listen_port()));
        check(deliver(xa, pa, n1, n2, 3), "T1: n1 is admitted in A and delivers");
        for (int i = 0; i < 40; ++i) {
            xb.sec.send(n2, make_frame(n1, n2));
            sleep_ms(15);
        }
        check(pb.got.load() == 0 && pb.sec.session_count() == 0 && xb.sec.session_count() == 0,
              "T1: n1 is denied in B (B's authority): no session on either side, nothing delivered");
        check(pa.sec.session_count() == 1, "T1: A's session is unaffected by B's denial");
        // CONTROL: B admits n1.
        (void)pb.auth.publish(snap(2, {n1}));
        Member xb2(n1, B, snap(1, {n2}));
        (void)xb2.start();
        xb2.tcp.add_peer(loopback_endpoint(n2, pb.tcp.listen_port()));
        check(deliver(xb2, pb, n1, n2, 3), "T1 CONTROL: once B admits it, n1 connects and delivers in B");

        // ---- T2: evict in A leaves B alone, and the reverse ----
        check(deliver(xa, pa, n1, n2, 1), "T2: A live before the eviction");
        (void)pa.auth.publish(snap(3, {}, {}, {n1}));  // A's authority evicts n1
        sleep_ms(300);
        const std::uint64_t a_before = pa.got.load();
        for (int i = 0; i < 20; ++i) {
            xa.sec.send(n2, make_frame(n1, n2));
            sleep_ms(10);
        }
        check(pa.got.load() == a_before, "T2 CONTROL: after the eviction A delivers nothing from n1");
        check(deliver(xb2, pb, n1, n2, 3), "T2: B (n1 admitted there) is unaffected by the eviction in A");
        (void)pb.auth.publish(snap(4, {}, {}, {n1}));  // now evict in B
        sleep_ms(300);
        (void)pa.auth.publish(snap(5, {n1}));  // and re-admit in A
        Member xa2(n1, A, snap(1, {n2}));
        (void)xa2.start();
        xa2.tcp.add_peer(loopback_endpoint(n2, pa.tcp.listen_port()));
        check(deliver(xa2, pa, n1, n2, 3), "T2 reverse: re-admitting in A works while n1 is evicted in B");
        const std::uint64_t b_before = pb.got.load();
        for (int i = 0; i < 20; ++i) {
            xb2.sec.send(n2, make_frame(n1, n2));
            sleep_ms(10);
        }
        check(pb.got.load() == b_before, "T2 reverse: the eviction in B is not undone by A");

        // ---- T4: a cluster-A peer dials B's endpoint ----
        Member pb4(n2, B, snap(1, {n1}));
        Member xa4(n1, A, snap(1, {n2}));
        (void)pb4.start();
        (void)xa4.start();
        xa4.tcp.add_peer(loopback_endpoint(n2, pb4.tcp.listen_port()));
        for (int i = 0; i < 40; ++i) {
            xa4.sec.send(n2, make_frame(n1, n2));
            sleep_ms(15);
        }
        check(pb4.sec.session_count() == 0 && xa4.sec.session_count() == 0 && pb4.got.load() == 0,
              "T4: a cluster-A peer reaching cluster B's endpoint gets no session on either side");
    }

    // ---- T3: a captured frame injected into the other cluster's session ----
    for (const bool omit : {false, true}) {
        MemCluster ca(0xAAAA, omit), cb(0xBBBB, omit);
        ca.establish();
        cb.establish();
        // Capture what n1 seals in A: swap n2's fabric receiver for a recorder, then send one frame.
        std::vector<MessageFrame> seen;
        ca.fabric.attach(ca.n2, [&](MessageFrame fr) { seen.push_back(std::move(fr)); });
        ca.s1.send(ca.n2, make_frame(ca.n1, ca.n2));
        ca.fabric.pump_all();
        const MessageFrame* sealed = nullptr;
        for (const auto& fr : seen)
            if (fr.kind == FrameKind::Data) sealed = &fr;
        check(sealed != nullptr, "T3: captured a sealed Data frame from cluster A");
        if (!sealed) continue;
        const std::size_t before = cb.got2.size();
        cb.fabric.send(cb.n2, *sealed);
        cb.fabric.pump_all();
        const bool accepted = cb.got2.size() > before;
        std::printf("T3 omit_cluster_in_aad=%d: injected frame %s (tamper_rejected=%llu)\n", static_cast<int>(omit),
                    accepted ? "ACCEPTED" : "rejected", static_cast<unsigned long long>(cb.s2.tamper_rejected()));
        if (omit) check(accepted, "T3 CONTROL: with the pre-fix AAD the cross-cluster injection is accepted");
        else check(!accepted && cb.s2.tamper_rejected() >= 1, "T3: the frame captured in A fails authentication in B");
    }

    // ---- T5: cost per extra cluster ----
    {
        constexpr int kClusters = 8;
        std::vector<std::unique_ptr<Member>> ms;
        const std::size_t before = rss_bytes();
        for (int i = 0; i < kClusters; ++i) {
            ms.push_back(std::make_unique<Member>(n2, ClusterId{static_cast<std::uint64_t>(0x1000 + i)}, snap(1, {n1})));
            (void)ms.back()->start();
        }
        sleep_ms(100);
        const std::size_t after = rss_bytes();
        const long per = kClusters ? static_cast<long>((after - before) / kClusters / 1024) : 0;
        std::printf("T5 cost per extra cluster instance: 1 IO thread, 1 listening socket, ~%ld KiB resident (%d instances; process baseline %zu KiB)\n",
                    per, kClusters, rss0 / 1024);
        check(per < 16 * 1024, "T5: an extra cluster instance costs well under 16 MiB resident");
    }

    std::printf(g_ok ? "PASS\n" : "FAIL\n");
    return g_ok ? 0 : 1;
}
