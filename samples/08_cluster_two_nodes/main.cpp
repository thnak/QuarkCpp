// Quark sample 08 — A two-node cluster (010 Distribution).
//
// Two engine instances form a cluster over a transport. Each actor identity has a deterministic OWNER
// node (computed by rendezvous/HRW placement over the membership), so any node can address any actor
// by key and the message is routed to wherever it lives:
//
//   * LOCAL FAST PATH — sending to a SELF-owned actor is delivered in-process and NEVER touches the
//     transport (0 wire bytes, 0 serialization). Distribution is zero-cost when the target is local.
//   * REMOTE PATH — sending to an actor owned by the OTHER node serializes the message (016), crosses
//     the transport, is decoded on the far node, and arrives at the right actor with FIFO preserved.
//
// Here both nodes run in ONE process wired over a LoopbackFabric (an in-memory stand-in for a real
// network) so the whole two-node data path is deterministic and needs no sockets.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 08_cluster_two_nodes
// Run  :  taskset -c 0-3 build/samples/08_cluster_two_nodes
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))  // must sit in the same namespace as Seq (ADL) — here, global

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;
    std::atomic<int> count{0};
    void handle(const Seq& s) noexcept {
        got.push_back(s.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

// One cluster node: its own engine + local router + membership view + transport endpoint + the
// distributed router that ties them together. Registers a Logger at `logger_key`.
struct ClusterNode {
    detail::MessagePool pool{4096};
    Logger actor;
    std::unique_ptr<Activation> act;
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership;
    LoopbackTransport transport;
    std::unique_ptr<DistributedRouter> dist;

    ClusterNode(NodeId self, LoopbackFabric& fabric, std::uint64_t logger_key)
        : membership(self, {NodeId{1}, NodeId{2}}), transport(fabric, self) {
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();  // teach it to decode+post inbound (Logger,Seq)
    }
};

static bool wait_count(std::atomic<int>& c, int want) {
    for (std::uint64_t spins = 0; c.load(std::memory_order_acquire) < want; ++spins)
        if (spins > 5'000'000'000ULL) return false;
    return true;
}

int main() {
    bool ok = true;

    // Placement is deterministic, so we can pick one key owned by node 1 and one owned by node 2.
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    const MembershipView pv = probe.view();
    std::uint64_t key_on_1 = 0, key_on_2 = 0;
    for (std::uint64_t k = 1; k < 10'000 && !(key_on_1 && key_on_2); ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        if (owner.value == 1 && !key_on_1) key_on_1 = k;
        else if (owner.value == 2 && !key_on_2) key_on_2 = k;
    }
    std::printf("placement: key %llu -> node 1, key %llu -> node 2\n",
                (unsigned long long)key_on_1, (unsigned long long)key_on_2);

    LoopbackFabric fabric;
    ClusterNode n1(NodeId{1}, fabric, key_on_1);  // node 1 hosts the node-1-owned Logger
    ClusterNode n2(NodeId{2}, fabric, key_on_2);  // node 2 hosts the node-2-owned Logger
    n1.eng.start();
    n2.eng.start();

    constexpr int N = 500;

    // ---- LOCAL: node 1 sends to the node-1-owned Logger. In-process, no wire. ------------------
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(key_on_1);
        const std::uint64_t wire_before = fabric.sends();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        ok &= wait_count(n1.actor.count, N);
        const bool no_wire = fabric.sends() == wire_before;
        ok &= no_wire && n1.dist->is_local(actor_id_of<Logger>(key_on_1));
        std::printf("local  send x%d -> node 1: delivered=%d, wire frames used=%llu  (expected 0 — local fast path)\n",
                    N, n1.actor.count.load(), (unsigned long long)(fabric.sends() - wire_before));
    }

    // ---- REMOTE: node 1 sends to the node-2-owned Logger. Serialized, crosses the transport. ----
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(key_on_2);
        const std::uint64_t wire_before = fabric.sends();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        ok &= wait_count(n2.actor.count, N);
        const std::uint64_t used = fabric.sends() - wire_before;
        ok &= used == static_cast<std::uint64_t>(N) && !n1.dist->is_local(actor_id_of<Logger>(key_on_2));
        // FIFO preserved across the wire.
        bool ordered = static_cast<int>(n2.actor.got.size()) >= N;
        for (int i = 0; i < N && ordered; ++i)
            if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
        ok &= ordered;
        std::printf("remote send x%d -> node 2: delivered=%d, wire frames used=%llu  (expected %d), FIFO=%s\n",
                    N, n2.actor.count.load(), (unsigned long long)used, N, ordered ? "intact" : "BROKEN");
    }

    n1.eng.stop();
    n2.eng.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
