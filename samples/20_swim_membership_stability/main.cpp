// Quark sample 20 — an ISOLATED instance: does real-time SwimMembership + net::TcpTransport hold a
// stable N-1-connections-per-node mesh indefinitely, or does it churn — including across a live join?
//
// WHY THIS EXISTS: sample 19 (dynamic join) reported `connections=X` at teardown time bouncing between
// 0 and final_n-1 across otherwise-identical runs, even when every message was delivered and FIFO was
// intact. That was left unexplained — plausibly SWIM ping/gossip churn under real scheduling jitter, or
// plausibly just multi-process teardown-timing skew (a fast-exiting sibling process's socket closing
// out from under this one right before it read connections_open()). Sample 19 cannot tell those apart:
// it has THREE confounds at once — real dynamic join, actor message traffic, and 4 separate OS processes
// tearing down at slightly different wall-clock moments.
//
// This sample removes all three. ONE process, up to FOUR SwimMembership+TcpTransport nodes (no
// DistributedRouter, no actors, no messages — pure membership/transport, nothing else). BASE_NODES
// start together at t=0; the remaining node joins live at LATE_JOIN_MS, exactly like sample 19's
// scenario, but with no traffic and no separate-process teardown to confound the reading. Running for a
// fixed, generous duration, SAMPLING (not just reading once at the end) roster size / epoch /
// connections_open every 250ms and printing the full timeline answers two questions at once: does the
// join transition itself cause any instability, and does the post-join steady state ever churn on its
// own. If it is NOT stable even here, that is unambiguous: a real churn bug in the SWIM/transport
// interaction under real (not virtual) time, not a sample-rig artifact.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 20_swim_membership_stability
// Run  :  taskset -c 0-3 build/samples/20_swim_membership_stability
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/membership.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

constexpr int BASE_NODES = 3;   // start together at t=0
constexpr int FINAL_NODES = 4;  // node 4 joins live at LATE_JOIN_MS
constexpr ClusterId kClusterId{0xC0FFEE};
constexpr int RUN_MS = 10000;
constexpr int SAMPLE_INTERVAL_MS = 250;
constexpr int LATE_JOIN_MS = 3000;
constexpr int STABLE_TAIL_MS = 3000;  // the trailing window that must be flat at N-1 to call it PASS

struct Node {
    net::TcpTransport transport;
    SwimMembership swim;
    std::thread driver;
    std::atomic<bool> running{true};
    std::unordered_set<std::uint64_t> known_peers;

    Node(NodeId self, int base_port)
        : transport(self, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + static_cast<int>(self.value))),
          swim(self, transport, [] {
              SwimMembership::Config c;
              c.cluster_id = kClusterId;
              return c;
          }()) {
        known_peers.insert(self.value);
    }

    void start_driver(int base_port) {
        driver = std::thread([this, base_port] {
            while (running.load(std::memory_order_relaxed)) {
                transport.post([this] { swim.tick(); });
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
    }

    void stop() {
        running.store(false, std::memory_order_relaxed);
        driver.join();
        transport.stop();
    }
};

static std::unique_ptr<Node> spawn(int id, int base_port) {
    auto n = std::make_unique<Node>(NodeId{static_cast<std::uint64_t>(id)}, base_port);
    if (!n->transport.start()) {
        std::printf("FAIL (transport bind failed for node %d)\n", id);
        return nullptr;
    }
    if (id != 1) {
        // Node 1 is the seed everyone else joins through.
        n->transport.add_peer(Endpoint{NodeId{1}, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + 1)});
        n->transport.post([node = n.get()] { node->swim.request_join(NodeId{1}); });
    }
    n->start_driver(base_port);
    return n;
}

int main() {
    constexpr int base_port = 23000;

    std::vector<std::unique_ptr<Node>> nodes(FINAL_NODES);
    for (int i = 1; i <= BASE_NODES; ++i) {
        nodes[static_cast<std::size_t>(i - 1)] = spawn(i, base_port);
        if (!nodes[static_cast<std::size_t>(i - 1)]) return 1;
    }

    std::printf("t_ms  roster(1,2,3,4)  conns(1,2,3,4)  epoch(1,2,3,4)\n");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::array<std::uint64_t, FINAL_NODES>> conn_history;
    bool late_joined = false;
    for (int elapsed = 0; elapsed <= RUN_MS; elapsed += SAMPLE_INTERVAL_MS) {
        if (!late_joined && elapsed >= LATE_JOIN_MS) {
            std::printf("      -- node %d joining live now (cluster already up %dms) --\n", FINAL_NODES, elapsed);
            nodes[FINAL_NODES - 1] = spawn(FINAL_NODES, base_port);
            if (!nodes[FINAL_NODES - 1]) return 1;
            late_joined = true;
        }

        std::array<std::uint64_t, FINAL_NODES> conns{};
        char roster_buf[64];
        char conns_buf[64];
        char epoch_buf[64];
        int rp = 0, cp = 0, ep = 0;
        for (int i = 0; i < FINAL_NODES; ++i) {
            Node* n = nodes[static_cast<std::size_t>(i)].get();
            if (n) {
                conns[static_cast<std::size_t>(i)] = n->transport.connections_open();
                rp += std::snprintf(roster_buf + rp, sizeof(roster_buf) - static_cast<std::size_t>(rp), "%2zu,", n->swim.view().size());
                cp += std::snprintf(conns_buf + cp, sizeof(conns_buf) - static_cast<std::size_t>(cp), "%2llu,",
                                     static_cast<unsigned long long>(conns[static_cast<std::size_t>(i)]));
                ep += std::snprintf(epoch_buf + ep, sizeof(epoch_buf) - static_cast<std::size_t>(ep), "%3llu,",
                                     static_cast<unsigned long long>(n->swim.epoch()));
            } else {
                rp += std::snprintf(roster_buf + rp, sizeof(roster_buf) - static_cast<std::size_t>(rp), " -,");
                cp += std::snprintf(conns_buf + cp, sizeof(conns_buf) - static_cast<std::size_t>(cp), " -,");
                ep += std::snprintf(epoch_buf + ep, sizeof(epoch_buf) - static_cast<std::size_t>(ep), "  -,");
                conns[static_cast<std::size_t>(i)] = 0;
            }
        }
        conn_history.push_back(conns);
        std::printf("%5d  %-13s  %-13s  %-13s\n", elapsed, roster_buf, conns_buf, epoch_buf);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(elapsed));
    }

    // PASS iff every node's connections_open() sat at exactly FINAL_NODES-1 for the ENTIRE trailing
    // STABLE_TAIL_MS window (by then node 4 has long since joined) — not just at the final instant, so a
    // flap-and-recover in the tail still fails.
    const int tail_samples = STABLE_TAIL_MS / SAMPLE_INTERVAL_MS;
    bool stable = static_cast<int>(conn_history.size()) > tail_samples;
    for (int i = static_cast<int>(conn_history.size()) - tail_samples; stable && i < static_cast<int>(conn_history.size()); ++i) {
        for (int node = 0; node < FINAL_NODES; ++node) {
            if (conn_history[static_cast<std::size_t>(i)][static_cast<std::size_t>(node)] != FINAL_NODES - 1) stable = false;
        }
    }

    for (auto& n : nodes)
        if (n) n->stop();

    std::printf("%s: connections %s at %d for the trailing %dms (across a live join at %dms)\n", stable ? "OK" : "FAIL",
                stable ? "held stable" : "did NOT hold stable", FINAL_NODES - 1, STABLE_TAIL_MS, LATE_JOIN_MS);
    return stable ? 0 : 1;
}
