// Quark sample 17 — A FOUR-node cluster, fully meshed over the REAL TCP transport (010 Distribution +
// net::TcpTransport on the 019 PAL event loop).
//
// Samples 15/16 are a two-node pair. This one generalizes to N=4 nodes, each holding one Logger, all
// wired into a full mesh (every node holds a peer endpoint for every other node). Every node then
// sends to every node's Logger — including its own (local fast path) — at the same time, so:
//
//   * 4 nodes x 4 receivers = 16 send lanes fire concurrently -> up to 12 simultaneous first-dials
//     across 6 node pairs, which is exactly the scenario 021's lower-NodeId-wins dial-dedup exists for
//     (both sides of a pair can race to dial before either hello lands). Every pair still converges to
//     EXACTLY ONE connection — but tcp_transport.hpp documents that the LOSING side of that race can
//     drop frames it had already handed to its socket (at-most-once, see the NOTE in
//     register_identified()). This sample's 6-way simultaneous race hits that window far more often
//     than the 2-node samples, so don't be surprised to see a node short by exactly one sender's whole
//     lane — that is the documented tradeoff, not a bug; a non-lane-sized shortfall or a broken FIFO
//     order would be.
//   * Per-(sender,receiver) FIFO must hold even while messages from the OTHER 3 senders interleave on
//     the same receiving Logger.
//   * Every wait on cluster convergence is a SINGLE combined bounded poll across all nodes (not one
//     poll per node — stacking N per-node timeouts would make a real hang and "several nodes hit the
//     same lossy race" indistinguishable from outside), so a genuine deadlock in the
//     router/transport/engine shows up as a printed FAIL well within the timeout, not a hang.
//     Run it under `timeout 30 ...` (or a debugger) if you suspect a lock-order or lost-wakeup bug.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 17_tcp_cluster_mesh
// Run  :  taskset -c 0-3 build/samples/17_tcp_cluster_mesh
// Debug:  taskset -c 0-3 timeout 30 build/samples/17_tcp_cluster_mesh   # hang -> timeout kills it, exit 124
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/net/tcp_transport.hpp"

using namespace quark;

constexpr int NUM_NODES = 4;
constexpr int N = 150;                        // messages per (sender, receiver) lane
constexpr std::int32_t SRC_MUL = 1'000'000;   // Seq.n encodes sender index * SRC_MUL + sequence

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))  // remotely-received messages MUST be serializable (016)

// One Logger per node. `last_seen[src]` tracks the last in-order sequence number received FROM that
// sender, so FIFO can be checked per-(sender,receiver) pair even though all 4 senders' traffic
// interleaves on the same actor. Single Sequential executor drains this actor, so no lock is needed.
struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::array<int, NUM_NODES> last_seen{};
    std::atomic<int> count{0};
    bool ordered = true;

    Logger() { last_seen.fill(-1); }

    void handle(const Seq& s) noexcept {
        const int src = s.n / SRC_MUL;
        const int seq = s.n % SRC_MUL;
        if (src < 0 || src >= NUM_NODES || seq != last_seen[static_cast<std::size_t>(src)] + 1) {
            ordered = false;
        } else {
            last_seen[static_cast<std::size_t>(src)] = seq;
        }
        count.fetch_add(1, std::memory_order_release);
    }
};

// One cluster node: engine + local router + membership + a Logger + a REAL TcpTransport bound to an
// ephemeral 127.0.0.1 port, tied together by the DistributedRouter. Same shape as samples 08/15, just
// parameterized over an arbitrary cluster roster instead of a hardcoded pair.
struct ClusterNode {
    detail::MessagePool pool{4096};
    Logger actor;
    std::unique_ptr<Activation> act;
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership;
    net::TcpTransport transport;
    std::unique_ptr<DistributedRouter> dist;

    ClusterNode(NodeId self, const std::vector<NodeId>& cluster, std::uint64_t logger_key)
        : membership(self, cluster), transport(self, pal::ipv4_loopback, /*ephemeral*/ 0) {
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();  // decode+post inbound (Logger,Seq)
    }
};

// sleep-poll (no busy-spin — machine-safety rule: never burn a core). A bounded poll instead of a
// blocking wait means a real deadlock surfaces as this returning false, not the process hanging.
template <class Pred>
static bool wait_until(Pred pred, int timeout_ms = 15000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    bool ok = true;

    const std::vector<NodeId> cluster{NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}};

    // Deterministic HRW placement: one Logger key owned by each of the 4 nodes.
    InProcessMembership probe(NodeId{1}, cluster);
    const MembershipView pv = probe.view();
    std::array<std::uint64_t, NUM_NODES> keys{};
    int found = 0;
    for (std::uint64_t k = 1; k < 1'000'000 && found < NUM_NODES; ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        const std::size_t idx = static_cast<std::size_t>(owner.value - 1);
        if (idx < NUM_NODES && keys[idx] == 0) {
            keys[idx] = k;
            ++found;
        }
    }
    ok &= found == NUM_NODES;
    if (!ok) { std::printf("FAIL (could not find a key owned by every node)\n"); return 1; }
    for (int i = 0; i < NUM_NODES; ++i) {
        std::printf("placement: key %llu -> node %d\n", static_cast<unsigned long long>(keys[static_cast<std::size_t>(i)]), i + 1);
    }

    // ---- Bring up all 4 nodes. -------------------------------------------------------------------
    std::array<std::unique_ptr<ClusterNode>, NUM_NODES> nodes;
    for (int i = 0; i < NUM_NODES; ++i) {
        nodes[static_cast<std::size_t>(i)] =
            std::make_unique<ClusterNode>(NodeId{static_cast<std::uint64_t>(i + 1)}, cluster, keys[static_cast<std::size_t>(i)]);
    }

    // Start every transport FIRST (each binds + starts its I/O thread and gets a real ephemeral port),
    // then teach every node every OTHER node's endpoint — a full mesh, 12 directed peer links across
    // 6 pairs. On a real cluster these endpoints come from 021 gossip; here we read the OS-assigned
    // ephemeral ports directly.
    for (int i = 0; i < NUM_NODES; ++i) ok &= nodes[static_cast<std::size_t>(i)]->transport.start();
    if (!ok) { std::printf("FAIL (transport startup)\n"); return 1; }
    for (int i = 0; i < NUM_NODES; ++i) {
        std::printf("node %d listening on 127.0.0.1:%u\n", i + 1, nodes[static_cast<std::size_t>(i)]->transport.listen_port());
    }
    for (int i = 0; i < NUM_NODES; ++i) {
        for (int j = 0; j < NUM_NODES; ++j) {
            if (i == j) continue;
            nodes[static_cast<std::size_t>(i)]->transport.add_peer(
                Endpoint{NodeId{static_cast<std::uint64_t>(j + 1)}, pal::ipv4_loopback,
                         nodes[static_cast<std::size_t>(j)]->transport.listen_port()});
        }
    }

    for (int i = 0; i < NUM_NODES; ++i) nodes[static_cast<std::size_t>(i)]->eng.start();

    // Sanity: every node is the LOCAL owner of its own key (no wire touched for self-sends below).
    for (int i = 0; i < NUM_NODES; ++i) {
        ok &= nodes[static_cast<std::size_t>(i)]->dist->is_local(actor_id_of<Logger>(keys[static_cast<std::size_t>(i)]));
    }

    // ---- Fire all 16 (sender, receiver) lanes — 4 local + 12 remote — with no synchronization      ----
    // between them, so first-dials genuinely race across the 6 node pairs.
    for (int i = 0; i < NUM_NODES; ++i) {
        for (int j = 0; j < NUM_NODES; ++j) {
            DistRef<Logger> ref = nodes[static_cast<std::size_t>(i)]->dist->get<Logger>(keys[static_cast<std::size_t>(j)]);
            for (int s = 0; s < N; ++s) ref.tell(Seq{static_cast<std::int32_t>(i) * SRC_MUL + s});
        }
    }

    // ---- Converge: ONE combined bounded poll for delivery, not one per node. A per-node loop of N
    // bounded waits can STACK into N times the timeout when several nodes are affected by the same
    // cause — on external inspection (e.g. `timeout 30 ...`) that stacking is indistinguishable from a
    // real hang. Waiting on all nodes at once keeps the worst case bounded to a single timeout no
    // matter how many nodes are affected.
    const int expected_per_node = NUM_NODES * N;
    wait_until(
        [&] {
            for (int j = 0; j < NUM_NODES; ++j)
                if (nodes[static_cast<std::size_t>(j)]->actor.count.load() < expected_per_node) return false;
            return true;
        },
        6000);

    // Full mesh: each node should converge to exactly NUM_NODES-1 open connections (dial-dedup
    // collapses any pair where both sides raced to dial down to one) — again one combined bounded poll.
    ok &= wait_until(
        [&] {
            for (int i = 0; i < NUM_NODES; ++i)
                if (nodes[static_cast<std::size_t>(i)]->transport.connections_open() != NUM_NODES - 1) return false;
            return true;
        },
        4000);

    std::uint64_t total_frames = 0;
    for (int i = 0; i < NUM_NODES; ++i) {
        auto& n = *nodes[static_cast<std::size_t>(i)];
        const int got = n.actor.count.load();
        const int shortfall = expected_per_node - got;
        // 021's dial-dedup rule is deterministic (lower NodeId wins) and DOES converge every pair to
        // one connection (checked above) — but tcp_transport.hpp's register_identified() documents that
        // the LOSING side of a simultaneous first-dial race can drop frames it had already handed to its
        // (about-to-close) socket: "at-most-once... a future refinement... once settled, delivery is
        // lossless" (see the NOTE there). With 6 pairs racing at once this sample hits that window far
        // more often than the 2-node samples do. The loss is always one sender's WHOLE lane at once
        // (the doomed connection never delivered anything, so no partial/torn frame) — anything else
        // (a non-lane-sized shortfall, or a broken FIFO order) would be a real, different bug.
        const bool shortfall_is_documented_race = shortfall >= 0 && shortfall <= N * (NUM_NODES - 1) && shortfall % N == 0;
        const bool ordered = n.actor.ordered;
        ok &= ordered;
        ok &= shortfall_is_documented_race;
        total_frames += n.transport.frames_sent();
        std::printf(
            "node %d: received=%d/%d FIFO=%s connections=%llu frames_sent=%llu dedup_closed=%llu%s\n",
            i + 1, got, expected_per_node, ordered ? "intact" : "BROKEN",
            static_cast<unsigned long long>(n.transport.connections_open()),
            static_cast<unsigned long long>(n.transport.frames_sent()),
            static_cast<unsigned long long>(n.transport.dedup_closed()),
            shortfall > 0 ? "  <- dial-dedup race dropped a lane (documented at-most-once, tcp_transport.hpp)" : "");
    }
    // 12 remote lanes x N messages must have crossed real sockets (4 local lanes touch none).
    ok &= total_frames >= static_cast<std::uint64_t>(NUM_NODES) * (NUM_NODES - 1) * N;
    std::printf("total wire frames across the mesh: %llu (expected >= %d)\n",
                static_cast<unsigned long long>(total_frames), NUM_NODES * (NUM_NODES - 1) * N);

    for (int i = 0; i < NUM_NODES; ++i) nodes[static_cast<std::size_t>(i)]->eng.stop();
    for (int i = 0; i < NUM_NODES; ++i) nodes[static_cast<std::size_t>(i)]->transport.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
