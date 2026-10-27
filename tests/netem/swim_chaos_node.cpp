// One SWIM member (021 §3) over the real TcpTransport (010/019), run ONE PROCESS PER NODE inside its
// own Linux network namespace by tests/netem/run_swim_chaos.sh, so partitions and link impairment are
// real kernel-level packet drops between real processes — not the in-process `set_link_reachable` /
// `set_online` test seams the cluster_swim_* unit tests use. Not a CTest (needs sudo + netns); built
// on Linux only.
//
// Node i listens on 10.77.0.i:<port>. Every peer's endpoint is registered up front (static discovery —
// keeps 021's not-yet-built Discovery seam out of what is being measured). Wiring follows sample 19:
// swim.tick() and every swim read run on the transport's I/O thread via transport.post(), because
// SwimMembership's state is single-threaded by contract.
//
// Output, one line every 250 ms (parsed by the runner):
//   T <ms> node=<id> epoch=<e> inc=<self incarnation> conns=<open> st=<one char per node 1..n>
// where st is A(live) S(suspect) D(dead or unknown) L(left) and '*' for self.
//
// Usage: swim_chaos_node <self_id> <n> <seed_id_or_0> [port=7400] [duration_ms=60000] [rejoin=0] [wire_version]
//   wire_version: SWIM wire version override (ADR-053 mixed-version runs; default = current)
//   rejoin=1: while this node's roster is short of n, re-send Join to the seed every 2 s.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "quark/core/cluster.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

constexpr ClusterId kClusterId{0xC4A05};

static std::uint64_t node_ip(std::uint64_t id) { return pal::ipv4(10, 77, 0, static_cast<std::uint8_t>(id)); }

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <self_id> <n> <seed_id_or_0> [port] [duration_ms] [rejoin]\n", argv[0]);
        return 2;
    }
    const std::uint64_t self_id = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t n = std::strtoull(argv[2], nullptr, 10);
    const std::uint64_t seed_id = std::strtoull(argv[3], nullptr, 10);
    const auto port = static_cast<std::uint16_t>(argc > 4 ? std::atoi(argv[4]) : 7400);
    const int duration_ms = argc > 5 ? std::atoi(argv[5]) : 60000;
    const bool rejoin = argc > 6 && std::atoi(argv[6]) != 0;
    if (self_id < 1 || self_id > n || n > 250) {
        std::fprintf(stderr, "self_id must be in [1, n], n <= 250\n");
        return 2;
    }

    net::TcpTransport transport{NodeId{self_id}, node_ip(self_id), port};
    SwimMembership::Config cfg;
    cfg.cluster_id = kClusterId;
    cfg.seed = self_id;
    if (argc > 7) cfg.wire_version = static_cast<std::uint8_t>(std::atoi(argv[7]));  // ADR-053 mixed N / N-1 run
    SwimMembership swim{NodeId{self_id}, transport, cfg};

    if (!transport.start()) {
        std::printf("node %llu: FAIL (bind 10.77.0.%llu:%u)\n", static_cast<unsigned long long>(self_id),
                    static_cast<unsigned long long>(self_id), port);
        return 1;
    }
    for (std::uint64_t i = 1; i <= n; ++i)
        if (i != self_id) transport.add_peer(Endpoint{NodeId{i}, node_ip(i), port});
    if (seed_id != 0) transport.post([&swim, seed_id] { swim.request_join(NodeId{seed_id}); });

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    // Sampled on the I/O thread (status_of/incarnation reads are not synchronized for other threads).
    auto sample = [&, self_id, n](long long t) {
        std::string st(n, 'D');
        for (std::uint64_t i = 1; i <= n; ++i) {
            if (i == self_id) {
                st[i - 1] = '*';
                continue;
            }
            switch (swim.status_of(NodeId{i})) {
                case MemberStatus::Alive: st[i - 1] = 'A'; break;
                case MemberStatus::Suspect: st[i - 1] = 'S'; break;
                case MemberStatus::Dead: st[i - 1] = 'D'; break;
                case MemberStatus::Left: st[i - 1] = 'L'; break;
            }
        }
        std::printf("T %lld node=%llu epoch=%llu inc=%llu conns=%llu st=%s\n", t,
                    static_cast<unsigned long long>(self_id), static_cast<unsigned long long>(swim.epoch()),
                    static_cast<unsigned long long>(swim.self_incarnation()),
                    static_cast<unsigned long long>(transport.connections_open()), st.c_str());
        std::fflush(stdout);
    };

    long long next_sample = 0, next_rejoin = 2000;
    while (elapsed_ms() < duration_ms) {
        transport.post([&swim] { swim.tick(); });
        const long long t = elapsed_ms();
        if (t >= next_sample) {
            transport.post([&sample, t] { sample(t); });
            next_sample = t + 250;
        }
        if (rejoin && seed_id != 0 && t >= next_rejoin) {
            if (swim.view().size() < n) transport.post([&swim, seed_id] { swim.request_join(NodeId{seed_id}); });
            next_rejoin = t + 2000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Drain: one last sample, then let the I/O thread run it before stopping.
    std::atomic<bool> done{false};
    transport.post([&] {
        sample(elapsed_ms());
        done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 1000 && !done.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    transport.stop();
    return 0;
}
