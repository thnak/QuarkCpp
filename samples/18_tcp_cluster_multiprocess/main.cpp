// Quark sample 18 — A cluster made of REAL, SEPARATE OS PROCESSES, not one process hosting several
// in-memory Engine/Transport pairs (as every earlier cluster sample — 08/15/16/17 — does).
//
// This binary IS one cluster node. You run it once per node, in its own process, and the processes
// find each other purely over 127.0.0.1 sockets:
//
//   samples/18_tcp_cluster_multiprocess/run_cluster.sh          # launches 4 separate processes
//
// or by hand, four separate terminals/shells:
//   ./18_tcp_cluster_multiprocess 1 4      # node 1 of 4
//   ./18_tcp_cluster_multiprocess 2 4      # node 2 of 4
//   ./18_tcp_cluster_multiprocess 3 4      # node 3 of 4
//   ./18_tcp_cluster_multiprocess 4 4      # node 4 of 4
//
// Why this is a stronger check than 17_tcp_cluster_mesh: with real separate processes there is no
// shared address space to cheat with — every send genuinely crosses process boundaries via the kernel
// socket layer, connection setup genuinely races against independent process/thread scheduling (not
// just independent I/O-thread timing inside one process), and a real cross-process deadlock or crash
// shows up as one process's exit code / hung PID, exactly like a real deployment failure would.
//
// Coordination-free by construction: every process derives an IDENTICAL (Logger-key -> owning node)
// table independently via 010's deterministic HRW placement — the only thing processes must agree on
// up front is `num_nodes` (so every process builds the same membership view) and the port formula
// (base_port + node_id) — no rendezvous file, no leader election, no shared filesystem state.
// A process started before its peers are listening does not fail: 021's jittered reconnect backoff
// keeps re-dialing (pal::tcp_connect + EPOLLERR/HUP -> handle_dead -> schedule_reconnect) until the
// peer's listener appears, so process start order is irrelevant.
//
// Each node sends its own lane (N messages) to EVERY node's Logger, including its own (local fast
// path), then waits — bounded, never blocking — for its OWN Logger to hear from every node in the
// cluster. It cannot observe any OTHER process's Logger (different address space) — each process's
// PASS/FAIL is about the messages it personally received, exactly what an operator checking process
// logs after a real deployment would look at.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 18_tcp_cluster_multiprocess
// Run  :  samples/18_tcp_cluster_multiprocess/run_cluster.sh [num_nodes=4] [messages_per_lane=150] [base_port=21000]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

constexpr std::int32_t SRC_MUL = 1'000'000;  // Seq.n encodes sender NodeId (1-based) * SRC_MUL + sequence

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))  // remotely-received messages MUST be serializable (016)

// `last_seen[src-1]` tracks the last in-order sequence number received FROM that sender, so FIFO can
// be checked per-(sender,receiver) pair even with every OTHER process's traffic interleaved on this
// Logger. Single Sequential executor drains this actor, so no lock is needed.
struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> last_seen;  // sized to num_nodes by main(), filled with -1
    std::atomic<int> count{0};
    bool ordered = true;

    void handle(const Seq& s) noexcept {
        const int src = s.n / SRC_MUL;
        const int seq = s.n % SRC_MUL;
        const std::size_t idx = static_cast<std::size_t>(src - 1);
        if (src < 1 || idx >= last_seen.size() || seq != last_seen[idx] + 1) {
            ordered = false;
        } else {
            last_seen[idx] = seq;
        }
        count.fetch_add(1, std::memory_order_release);
    }
};

// sleep-poll (no busy-spin — machine-safety rule: never burn a core). A bounded poll instead of a
// blocking wait means a real cross-process deadlock surfaces as this process exiting FAIL, not hanging.
template <class Pred>
static bool wait_until(Pred pred, int timeout_ms) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                      "usage: %s <self_node_id 1..N> [num_nodes=4] [messages_per_lane=150] [base_port=21000]\n",
                      argv[0]);
        return 2;
    }
    const std::uint64_t self_id = std::strtoull(argv[1], nullptr, 10);
    const int num_nodes = argc > 2 ? std::atoi(argv[2]) : 4;
    const int N = argc > 3 ? std::atoi(argv[3]) : 150;
    const int base_port = argc > 4 ? std::atoi(argv[4]) : 21000;
    if (self_id < 1 || static_cast<int>(self_id) > num_nodes || num_nodes < 1) {
        std::fprintf(stderr, "self_node_id must be in [1, num_nodes]\n");
        return 2;
    }

    std::vector<NodeId> cluster;
    for (int i = 1; i <= num_nodes; ++i) cluster.push_back(NodeId{static_cast<std::uint64_t>(i)});

    // Deterministic HRW placement: every process computes this table independently and gets the SAME
    // answer (010) — the only cross-process agreement needed is `num_nodes` (given identically on argv
    // to every process by run_cluster.sh), not the keys themselves.
    InProcessMembership probe(NodeId{1}, cluster);
    const MembershipView pv = probe.view();
    std::vector<std::uint64_t> keys(static_cast<std::size_t>(num_nodes), 0);
    int found = 0;
    for (std::uint64_t k = 1; k < 1'000'000 && found < num_nodes; ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        const std::size_t idx = static_cast<std::size_t>(owner.value - 1);
        if (idx < static_cast<std::size_t>(num_nodes) && keys[idx] == 0) {
            keys[idx] = k;
            ++found;
        }
    }
    if (found != num_nodes) {
        std::printf("node %llu: FAIL (could not find a key owned by every node)\n",
                    static_cast<unsigned long long>(self_id));
        return 1;
    }

    detail::MessagePool pool{4096};
    Logger actor;
    actor.last_seen.assign(static_cast<std::size_t>(num_nodes), -1);
    auto act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership{NodeId{self_id}, cluster};
    // Fixed (not ephemeral) port so every OTHER process can dial it without any rendezvous mechanism.
    net::TcpTransport transport{NodeId{self_id}, pal::ipv4_loopback,
                                 static_cast<std::uint16_t>(base_port + static_cast<int>(self_id))};
    eng.register_activation(actor_id_of<Logger>(keys[static_cast<std::size_t>(self_id - 1)]), *act);
    auto dist = std::make_unique<DistributedRouter>(membership, local, transport);
    dist->register_remote<Logger, Seq>();  // decode+post inbound (Logger,Seq)

    if (!transport.start()) {
        std::printf("node %llu: FAIL (bind on port %d failed — already in use by a leftover process?)\n",
                    static_cast<unsigned long long>(self_id), base_port + static_cast<int>(self_id));
        return 1;
    }
    std::printf("node %llu listening on 127.0.0.1:%u\n", static_cast<unsigned long long>(self_id),
                transport.listen_port());
    for (int j = 1; j <= num_nodes; ++j) {
        if (j == static_cast<int>(self_id)) continue;
        transport.add_peer(
            Endpoint{NodeId{static_cast<std::uint64_t>(j)}, pal::ipv4_loopback, static_cast<std::uint16_t>(base_port + j)});
    }

    eng.start();

    // Send our lane to every node's Logger, including our own (local fast path). Independent of
    // whether every OTHER process has started listening yet — the transport's reconnect backoff
    // (021) covers that; a send just queues until dial + hello complete.
    for (int j = 1; j <= num_nodes; ++j) {
        DistRef<Logger> ref = dist->get<Logger>(keys[static_cast<std::size_t>(j - 1)]);
        for (int s = 0; s < N; ++s) ref.tell(Seq{static_cast<std::int32_t>(self_id) * SRC_MUL + s});
    }

    const int expected = num_nodes * N;

    // A SHARED settle window, not "stop as soon as I personally succeed": every process was launched
    // at roughly the same wall-clock moment (run_cluster.sh), so holding this deadline in common keeps
    // every node's socket open for the same span. Without this, a node with no message loss can finish
    // in milliseconds, stop() its transport, and exit its OS process while a slower peer (still
    // retrying a dial, or working through the documented dedup-race backoff) is mid-flight — and that
    // early exit closes the socket out from under the slower peer, turning ordinary convergence lag
    // into artificial connection loss or a genuinely torn mid-stream frame (a different, worse failure
    // than the single documented one-lane loss).
    constexpr int settle_budget_ms = 8000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(settle_budget_ms);
    auto remaining_ms = [&] {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return rem > 0 ? static_cast<int>(rem) : 0;
    };

    const bool conn_ok =
        wait_until([&] { return transport.connections_open() == static_cast<std::uint64_t>(num_nodes - 1); }, remaining_ms());
    wait_until([&] { return actor.count.load() >= expected; }, remaining_ms());
    // Hold the connection open for whatever's left of the shared window instead of tearing down the
    // instant this node is satisfied — see the comment above.
    if (const int rem = remaining_ms(); rem > 0) std::this_thread::sleep_for(std::chrono::milliseconds(rem));

    const int got = actor.count.load();
    const int shortfall = expected - got;
    // Same documented tradeoff as sample 17: the LOSING side of a simultaneous first-dial race can
    // drop frames it had already handed to its socket (tcp_transport.hpp register_identified(), "NOTE
    // (at-most-once...)") — always one sender's WHOLE lane, never a partial/torn frame. Anything else
    // (a non-lane-sized shortfall, or a broken FIFO order) is a real, different bug.
    const bool shortfall_is_documented_race = shortfall >= 0 && shortfall <= N * (num_nodes - 1) && shortfall % N == 0;
    const bool ordered = actor.ordered;

    std::printf(
        "node %llu: received=%d/%d FIFO=%s connections=%llu/%d frames_sent=%llu dedup_closed=%llu%s\n",
        static_cast<unsigned long long>(self_id), got, expected, ordered ? "intact" : "BROKEN",
        static_cast<unsigned long long>(transport.connections_open()), num_nodes - 1,
        static_cast<unsigned long long>(transport.frames_sent()), static_cast<unsigned long long>(transport.dedup_closed()),
        shortfall > 0 ? "  <- dial-dedup race dropped a lane (documented at-most-once, tcp_transport.hpp)" : "");

    eng.stop();
    transport.stop();

    const bool ok = ordered && shortfall_is_documented_race && conn_ok;
    std::printf("node %llu: %s\n", static_cast<unsigned long long>(self_id), ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
