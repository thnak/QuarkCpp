// Quark sample 15 — A two-node cluster over the REAL TCP transport (010 Distribution + the default
// net::TcpTransport on the 019 PAL event loop).
//
// This is sample 08's twin, but the frames cross ACTUAL 127.0.0.1 sockets instead of an in-memory
// loopback fabric. Everything above the transport — HRW placement, the DistributedRouter, the local
// fast path, 016 serialization — is byte-for-byte the same code; only the Transport implementation
// changes. That is the whole point of the transport seam: the engine does not know or care whether the
// wire is a function call or a socket.
//
//   * LOCAL FAST PATH   — a send to a SELF-owned actor is delivered in-process, 0 bytes on the wire.
//   * REMOTE over TCP   — a send to the OTHER node's actor is serialized (016), lazily DIALED on the
//                         first send (021: a TCP connection is opened on demand, with a NodeId hello
//                         handshake), carried over the socket, decoded on the far node, and posted to
//                         the target actor with per-(sender,receiver) FIFO intact.
//   * ONE CONNECTION    — after the exchange each node holds exactly one connection to its peer.
//
// Both nodes run in ONE process here (two engines, two transports, two ephemeral ports) so the sample
// is self-contained — but the two transports only ever talk through the kernel's TCP stack, exactly as
// two separate machines would.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 15_tcp_cluster
// Run  :  taskset -c 0-3 build/samples/15_tcp_cluster
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

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n))  // remotely-received messages MUST be serializable (016)

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;             // single drain lane (single executor) — no lock needed
    std::atomic<int> count{0};
    void handle(const Seq& s) noexcept {
        got.push_back(s.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

// One cluster node: engine + local router + membership + a Logger + a REAL TcpTransport bound to an
// ephemeral 127.0.0.1 port, tied together by the DistributedRouter. Same struct as sample 08 with
// LoopbackTransport swapped for net::TcpTransport(self, ip, port).
struct ClusterNode {
    detail::MessagePool pool{4096};
    Logger actor;
    std::unique_ptr<Activation> act;
    Engine<> eng{EngineConfig{1, 1, 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership;
    net::TcpTransport transport;
    std::unique_ptr<DistributedRouter> dist;

    ClusterNode(NodeId self, std::uint64_t logger_key)
        : membership(self, {NodeId{1}, NodeId{2}}),
          transport(self, pal::ipv4_loopback, /*ephemeral*/ 0) {
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();  // decode+post inbound (Logger,Seq)
    }
};

// sleep-poll (no busy-spin — machine-safety rule: never burn a core).
template <class Pred>
static bool wait_until(Pred pred, int timeout_ms = 5000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    bool ok = true;

    // Deterministic HRW placement: pick one Logger key owned by node 1, one owned by node 2.
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    const MembershipView pv = probe.view();
    std::uint64_t key_on_1 = 0, key_on_2 = 0;
    for (std::uint64_t k = 1; k < 10'000 && !(key_on_1 && key_on_2); ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        if (owner.value == 1 && !key_on_1) key_on_1 = k;
        else if (owner.value == 2 && !key_on_2) key_on_2 = k;
    }
    std::printf("placement: key %llu -> node 1, key %llu -> node 2\n",
                static_cast<unsigned long long>(key_on_1), static_cast<unsigned long long>(key_on_2));

    ClusterNode n1(NodeId{1}, key_on_1);
    ClusterNode n2(NodeId{2}, key_on_2);

    // Bring the transports up FIRST (each binds + starts its I/O thread and gets a real port), then
    // teach each node the other's endpoint. On a real cluster these endpoints come from 021 gossip;
    // here we read the OS-assigned ephemeral ports directly.
    ok &= n1.transport.start() && n2.transport.start();
    if (!ok) { std::printf("FAIL (transport startup)\n"); return 1; }
    std::printf("node 1 listening on 127.0.0.1:%u,  node 2 on 127.0.0.1:%u\n",
                n1.transport.listen_port(), n2.transport.listen_port());
    n1.transport.add_peer(Endpoint{NodeId{2}, pal::ipv4_loopback, n2.transport.listen_port()});
    n2.transport.add_peer(Endpoint{NodeId{1}, pal::ipv4_loopback, n1.transport.listen_port()});

    n1.eng.start();
    n2.eng.start();

    constexpr int N = 500;

    // ---- LOCAL: node 1 -> node-1-owned Logger. In-process, no socket touched. ------------------
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(key_on_1);
        const std::uint64_t wire_before = n1.transport.frames_sent();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        ok &= wait_until([&] { return n1.actor.count.load() >= N; });
        const std::uint64_t used = n1.transport.frames_sent() - wire_before;
        ok &= used == 0 && n1.dist->is_local(actor_id_of<Logger>(key_on_1));
        std::printf("local  send x%d -> node 1: delivered=%d, wire frames=%llu  (expected 0 — local fast path)\n",
                    N, n1.actor.count.load(), static_cast<unsigned long long>(used));
    }

    // ---- REMOTE over TCP: node 1 -> node-2-owned Logger. First send lazily dials the socket. -----
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(key_on_2);
        const std::uint64_t wire_before = n1.transport.frames_sent();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        ok &= wait_until([&] { return n2.actor.count.load() >= N; }, 8000);
        const std::uint64_t used = n1.transport.frames_sent() - wire_before;
        ok &= used >= static_cast<std::uint64_t>(N) && !n1.dist->is_local(actor_id_of<Logger>(key_on_2));
        // FIFO preserved across the socket.
        bool ordered = static_cast<int>(n2.actor.got.size()) >= N;
        for (int i = 0; i < N && ordered; ++i)
            if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
        ok &= ordered;
        // The lazy dial converged to exactly one connection to the peer.
        ok &= wait_until([&] { return n1.transport.connections_open() == 1; });
        std::printf("remote send x%d -> node 2: delivered=%d over TCP, wire frames=%llu, FIFO=%s, "
                    "connections=%llu\n",
                    N, n2.actor.count.load(), static_cast<unsigned long long>(used), ordered ? "intact" : "BROKEN",
                    static_cast<unsigned long long>(n1.transport.connections_open()));
    }

    n1.eng.stop();
    n2.eng.stop();
    n1.transport.stop();
    n2.transport.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
