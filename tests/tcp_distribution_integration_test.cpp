// Full-stack proof: 010 DistributedRouter routing a cross-node `tell` over the REAL TcpTransport (not
// the loopback double). Same two-node cluster as distribution_routing_test, but the frames cross actual
// 127.0.0.1 sockets: node 1 serializes (016), the TCP transport carries the frame, node 2 decodes and
// re-posts it as an ordinary local delivery to the target actor — with per-(sender,receiver) FIFO and
// the single-executor invariant intact end to end. Also re-confirms the local fast path never touches
// the transport (0 frames on the wire for a self-owned send).
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
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;

namespace {

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n));

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;  // single drain lane (single-executor) — no lock needed
    std::atomic<int> count{0};
    void handle(const Seq& s) noexcept {
        got.push_back(s.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// One cluster node: engine + local router + a Logger + a REAL TcpTransport bound to an ephemeral port.
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
          transport(self, pal::ipv4_loopback, 0) {
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        // DistributedRouter attaches its inbound sink here (transport not started yet — stored, used
        // once the I/O thread runs). register the (Logger,Seq) inbound decode+post thunk.
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();
    }
};

}  // namespace

int main() {
    bool ok = true;

    // Deterministic HRW placement: find a Logger key owned by node 1 (local) and one by node 2 (remote).
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    MembershipView pv = probe.view();
    std::uint64_t local_key = 0, remote_key = 0;
    bool have_local = false, have_remote = false;
    for (std::uint64_t k = 1; k < 10'000 && !(have_local && have_remote); ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        if (owner.value == 1 && !have_local) { local_key = k; have_local = true; }
        else if (owner.value == 2 && !have_remote) { remote_key = k; have_remote = true; }
    }
    check(have_local && have_remote, "found a node-1-owned and a node-2-owned Logger key", ok);

    ClusterNode n1(NodeId{1}, local_key);
    ClusterNode n2(NodeId{2}, remote_key);
    check(n1.transport.start() && n2.transport.start(), "both node transports start", ok);
    if (!ok) {
        std::printf("tcp_distribution_integration_test: FAIL (startup)\n");
        return 1;
    }
    n1.transport.add_peer(quark::test::loopback_endpoint(NodeId{2}, n2.transport.listen_port()));
    n2.transport.add_peer(quark::test::loopback_endpoint(NodeId{1}, n1.transport.listen_port()));
    n1.eng.start();
    n2.eng.start();

    constexpr int N = 300;

    // ---- LOCAL FAST PATH: a self-owned send is delivered locally and NEVER hits the transport. ---
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(local_key);
        check(n1.dist->is_local(actor_id_of<Logger>(local_key)), "local target is self-owned", ok);
        const std::uint64_t wire_before = n1.transport.frames_sent();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        check(quark::test::wait_until([&] { return n1.actor.count.load() >= N; }),
              "local: all N delivered", ok);
        check(n1.transport.frames_sent() == wire_before,
              "local send emits 0 frames on the wire (zero-cost local path)", ok);
    }

    // ---- REMOTE over TCP: node 1 tells a node-2-owned actor; frame crosses the socket to node 2. ---
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(remote_key);
        check(!n1.dist->is_local(actor_id_of<Logger>(remote_key)), "remote target is node-2-owned", ok);
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        check(quark::test::wait_until([&] { return n2.actor.count.load() >= N; }, 8000),
              "remote: all N frames delivered to node 2 over TCP", ok);
        check(n1.transport.frames_sent() >= static_cast<std::uint64_t>(N),
              "remote send actually crossed the transport", ok);
        // The engine drains node 2's Logger on its single executor; check FIFO on that lane.
        bool ordered = static_cast<int>(n2.actor.got.size()) >= N;
        for (int i = 0; i < N && i < static_cast<int>(n2.actor.got.size()); ++i)
            if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
        check(ordered, "remote: 016 roundtrip correct + per-(sender,receiver) FIFO over TCP", ok);
    }

    n1.eng.stop();
    n2.eng.stop();
    n1.transport.stop();
    n2.transport.stop();

    std::printf("tcp_distribution_integration_test: %s  (local_key=%llu remote_key=%llu, n2 recv=%d)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(local_key),
                static_cast<unsigned long long>(remote_key), n2.actor.count.load());
    return ok ? 0 : 1;
}
