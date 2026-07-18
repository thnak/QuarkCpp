// Quark sample 16 — Bidirectional cluster interaction over the real TCP transport.
//
// Sample 15 sends one way (node 1 -> node 2). This one closes the loop: an actor on the FAR node
// replies back across the cluster, so traffic flows in BOTH directions. It shows two things at once —
// how an actor addresses a remote peer from inside its own handler, and that a Quark connection is
// full-duplex: the reply rides the SAME socket node 1 opened, so the cluster stays at one connection
// per peer without a second dial.
//
//   node 1                                    node 2
//   ------                                    ------
//   driver --Ping(n)--> [ dial 1->2, TCP ] --> Ponger.handle(Ping)
//                                                   |
//   Collector.handle(Pong) <---- same socket ------+  home->get<Collector>(key).tell(Pong(n))
//
// The Ponger lives on node 2 but holds node 2's DistributedRouter, so from inside its handler it can
// address the Collector on node 1 by key exactly like any other actor — the router figures out it is
// remote and puts it on the wire. Neither actor knows a socket exists. Every Pong that comes back is
// proof a full request/reply crossed the cluster through the placement + serialization stack.
//
// Because node 1 dialed first and the NodeId hello identified that link, node 2 REUSES the inbound
// connection to reply — it does not dial back. So you normally see dedup_closed=0 and exactly one
// connection each. (The 021 lower-NodeId-wins dedup only fires in the rare case both sides dial before
// either hello lands; the dedicated dial-dedup test forces that race — this sample just observes it.)
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 16_tcp_request_reply
// Run  :  taskset -c 0-3 build/samples/16_tcp_request_reply
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

struct Ping {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Ping, (1, n))

struct Pong {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Pong, (1, n))

// Lives on node 1. Collects the replies that came back across the cluster.
struct Collector : Actor<Collector, Sequential> {
    using protocol = Protocol<Pong>;
    std::vector<int> got;
    std::atomic<int> count{0};
    void handle(const Pong& p) noexcept {
        got.push_back(p.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

// Lives on node 2. On each Ping, sends a Pong straight back to the Collector on node 1 — addressed by
// key through node 2's own DistributedRouter, which sees it is remote and routes it over TCP.
struct Ponger : Actor<Ponger, Sequential> {
    using protocol = Protocol<Ping>;
    DistributedRouter* home = nullptr;  // wired after construction; the path back to node 1
    std::uint64_t collector_key = 0;
    std::atomic<int> count{0};
    void handle(const Ping& p) noexcept {
        count.fetch_add(1, std::memory_order_release);
        if (home) home->get<Collector>(collector_key).tell(Pong{p.n});  // reply crosses the cluster
    }
};

template <class Pred>
static bool wait_until(Pred pred, int timeout_ms = 8000) {
    for (int i = 0; i < timeout_ms; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    bool ok = true;

    // Deterministic HRW placement: a Collector key owned by node 1, a Ponger key owned by node 2.
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    const MembershipView pv = probe.view();
    std::uint64_t collector_key = 0, ponger_key = 0;
    for (std::uint64_t k = 1; k < 100'000 && !(collector_key && ponger_key); ++k) {
        if (!collector_key && place(actor_id_of<Collector>(k), pv)->value == 1) collector_key = k;
        if (!ponger_key && place(actor_id_of<Ponger>(k), pv)->value == 2) ponger_key = k;
    }
    std::printf("placement: Collector key %llu -> node 1, Ponger key %llu -> node 2\n",
                static_cast<unsigned long long>(collector_key), static_cast<unsigned long long>(ponger_key));

    const std::vector<NodeId> cluster{NodeId{1}, NodeId{2}};

    // ---- Node 1: engine + Collector + transport + router. --------------------------------------
    detail::MessagePool pool1{4096};
    Collector collector;
    auto act1 = std::make_unique<Activation>(&collector, Collector::dispatch_table(), pool1.sink());
    Engine<> eng1{EngineConfig{1, 1, 64, 64}};
    LocalRouter local1{eng1.post_courier(), pool1};
    InProcessMembership mem1{NodeId{1}, cluster};
    net::TcpTransport tx1{NodeId{1}, pal::ipv4_loopback, 0};
    eng1.register_activation(actor_id_of<Collector>(collector_key), *act1);
    auto dist1 = std::make_unique<DistributedRouter>(mem1, local1, tx1);
    dist1->register_remote<Collector, Pong>();  // node 1 decodes inbound Pong

    // ---- Node 2: engine + Ponger + transport + router. -----------------------------------------
    detail::MessagePool pool2{4096};
    Ponger ponger;
    auto act2 = std::make_unique<Activation>(&ponger, Ponger::dispatch_table(), pool2.sink());
    Engine<> eng2{EngineConfig{1, 1, 64, 64}};
    LocalRouter local2{eng2.post_courier(), pool2};
    InProcessMembership mem2{NodeId{2}, cluster};
    net::TcpTransport tx2{NodeId{2}, pal::ipv4_loopback, 0};
    eng2.register_activation(actor_id_of<Ponger>(ponger_key), *act2);
    auto dist2 = std::make_unique<DistributedRouter>(mem2, local2, tx2);
    dist2->register_remote<Ponger, Ping>();  // node 2 decodes inbound Ping
    ponger.home = dist2.get();               // Ponger replies through node 2's router...
    ponger.collector_key = collector_key;    // ...to the Collector on node 1

    // Start both transports, exchange endpoints, start both engines.
    ok &= tx1.start() && tx2.start();
    if (!ok) { std::printf("FAIL (transport startup)\n"); return 1; }
    std::printf("node 1 on 127.0.0.1:%u,  node 2 on 127.0.0.1:%u\n", tx1.listen_port(),
                tx2.listen_port());
    tx1.add_peer(Endpoint{NodeId{2}, pal::ipv4_loopback, tx2.listen_port()});
    tx2.add_peer(Endpoint{NodeId{1}, pal::ipv4_loopback, tx1.listen_port()});
    eng1.start();
    eng2.start();

    // ---- Fire N pings from node 1; each round-trips to node 2 and back. ------------------------
    constexpr int N = 300;
    DistRef<Ponger> ping_ref = dist1->get<Ponger>(ponger_key);
    for (int i = 0; i < N; ++i) ping_ref.tell(Ping{i});

    ok &= wait_until([&] { return collector.count.load() >= N; });
    ok &= ponger.count.load() >= N;

    // Traffic went both ways over ONE full-duplex connection per peer (node 2 replied on the socket
    // node 1 opened — no dial-back), so each side holds exactly one connection.
    ok &= wait_until([&] { return tx1.connections_open() == 1 && tx2.connections_open() == 1; });

    std::printf("sent %d pings: node 2 saw %d, node 1 got %d pongs back over TCP\n", N,
                ponger.count.load(), collector.count.load());
    std::printf("connections: node1=%llu node2=%llu, dedup_closed n1=%llu n2=%llu (>0 iff a dial raced)\n",
                static_cast<unsigned long long>(tx1.connections_open()), static_cast<unsigned long long>(tx2.connections_open()),
                static_cast<unsigned long long>(tx1.dedup_closed()), static_cast<unsigned long long>(tx2.dedup_closed()));

    eng1.stop();
    eng2.stop();
    tx1.stop();
    tx2.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
