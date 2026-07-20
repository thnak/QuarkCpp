// Quark sample 21 — isolates the SECOND half of the question sample 20 raised: sample 20 proved
// SwimMembership + net::TcpTransport connections are rock-solid stable in real time, including across
// a live join, when nothing tears anything down mid-test. So why did samples 18/19 (before their
// "shared settle window" fix) see connections_open() bounce around at teardown time, all the way down
// to 0, even when every message had been delivered correctly?
//
// This sample answers it directly by DELIBERATELY REPRODUCING the anti-pattern samples 18/19 originally
// had, stripped of every other confound (no actor traffic, no dynamic join — pure membership/transport,
// multiple real OS processes): every process exits AS SOON AS IT PERSONALLY is satisfied (its own
// roster converged), with NO shared settle window. If connections_open() readings come back low/varied
// across otherwise-identical processes — purely a function of which sibling happened to exit first —
// that is the mechanism, confirmed in isolation: a fast-exiting process's transport.stop() closes its
// sockets, and whichever slower sibling hadn't read connections_open() yet sees fewer than N-1, even
// though the mesh was perfectly healthy moments earlier. It is a TEST-RIG artifact of unsynchronized
// teardown, not an engine bug — exactly what samples 18/19's shared-settle-window fix (main.cpp there)
// exists to prevent.
//
// Usage: run once per node via run_repro.sh, which launches `final_n` real separate processes with NO
// synchronization at exit — the anti-pattern, on purpose.
//   samples/21_teardown_skew_repro/run_repro.sh [final_n=4] [base_port=24000]
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 21_teardown_skew_repro
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>

#include "quark/core/cluster.hpp"
#include "quark/core/membership.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

constexpr ClusterId kClusterId{0xC0FFEE};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <self_id> <final_n> [base_port=24000]\n", argv[0]);
        return 2;
    }
    const std::uint64_t self_id = std::strtoull(argv[1], nullptr, 10);
    const int final_n = std::atoi(argv[2]);
    const int base_port = argc > 3 ? std::atoi(argv[3]) : 24000;

    net::TcpTransport transport{NodeId{self_id}, pal::ipv4_loopback,
                                 static_cast<std::uint16_t>(base_port + static_cast<int>(self_id))};
    SwimMembership::Config cfg;
    cfg.cluster_id = kClusterId;
    SwimMembership swim{NodeId{self_id}, transport, cfg};

    if (!transport.start()) {
        std::printf("node %llu: FAIL (bind failed)\n", static_cast<unsigned long long>(self_id));
        return 1;
    }
    if (self_id != 1) {
        transport.add_peer(Endpoint{NodeId{1}, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + 1)});
        transport.post([&swim] { swim.request_join(NodeId{1}); });
    }

    std::atomic<bool> running{true};
    std::unordered_set<std::uint64_t> known_peers{self_id};
    std::thread driver([&] {
        while (running.load(std::memory_order_relaxed)) {
            transport.post([&swim] { swim.tick(); });
            const MembershipView view = swim.view();
            for (NodeId n : view.nodes()) {
                if (known_peers.insert(n.value).second) {
                    transport.add_peer(
                        Endpoint{n, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + static_cast<int>(n.value))});
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // THE ANTI-PATTERN, ON PURPOSE: wait only for THIS node's own roster to converge, then tear down
    // and exit IMMEDIATELY — no shared settle window, no coordination with siblings whatsoever.
    bool converged = false;
    for (int i = 0; i < 15000 && !converged; ++i) {
        converged = swim.view().size() == static_cast<std::size_t>(final_n);
        if (!converged) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t conns_at_exit = transport.connections_open();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();

    running.store(false, std::memory_order_relaxed);
    driver.join();
    transport.stop();

    std::printf("node %llu: converged=%s connections_at_exit=%llu/%d wall_clock_ms=%lld\n",
                static_cast<unsigned long long>(self_id), converged ? "yes" : "NO",
                static_cast<unsigned long long>(conns_at_exit), final_n - 1, static_cast<long long>(now_ms));
    // Exits 0 whenever the roster converged, REGARDLESS of conns_at_exit — the point of this sample is
    // to SHOW the reading fluctuate, not to assert it must equal final_n-1 (that would defeat the point;
    // see run_repro.sh, which does the actual "is this the teardown-skew artifact" analysis afterward).
    return converged ? 0 : 1;
}
