// Tests 010-Distribution §"Delivery semantics across the network" — the distributed routing data
// path on a two-node in-process cluster (nodes 1 and 2) wired over a LoopbackTransport:
//   * LOCAL fast path: a send to a SELF-owned actor is delivered locally and NEVER touches the
//     transport (0 wire, 0 serialization) — the zero-cost-when-unused / local-fast-path invariant.
//   * REMOTE path: a send to a REMOTE-owned actor serializes via 016, crosses the transport, is
//     decoded on the far node, and arrives at the RIGHT actor with per-(sender,receiver) FIFO intact.
//   * Both 016 wire modes exercised: the negotiated tagless fast path (matched peers) AND the
//     canonical tagged fallback (forced by an injected schema mismatch — a rolling-upgrade peer).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

struct Seq {
    std::int32_t n = 0;
};

// QUARK_SERIALIZE must sit in the SAME namespace as the type so the generated quark_describe is
// found by ADL (a type's associated namespace is its enclosing one) — here, this anonymous namespace.
QUARK_SERIALIZE(Seq, (1, n));

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;  // written only on the single drain lane (single-executor)
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

std::atomic<int> g_warns{0};
void count_warn(std::string_view) { g_warns.fetch_add(1, std::memory_order_relaxed); }

bool wait_count(std::atomic<int>& c, int want) {
    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (c.load(std::memory_order_acquire) < want)
        if (++spins > kStall) return false;
    return true;
}

// One cluster node: engine + local router + a Logger registered at a fixed key + a loopback endpoint.
struct ClusterNode {
    detail::MessagePool pool{4096};
    Logger actor;
    std::unique_ptr<Activation> act;
    Engine<> eng{EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64}};
    LocalRouter local{eng.post_courier(), pool};
    InProcessMembership membership;
    LoopbackTransport transport;
    std::unique_ptr<DistributedRouter> dist;

    ClusterNode(NodeId self, LoopbackFabric& fabric, std::uint64_t logger_key)
        : membership(self, {NodeId{1}, NodeId{2}}), transport(fabric, self) {
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();  // inbound decode+post for (Logger, Seq)
    }
};

}  // namespace

int main() {
    bool ok = true;
    g_wire_fallback_warn = &count_warn;

    // Find one Logger key owned by node 1 (local to the sender) and one owned by node 2 (remote).
    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    MembershipView pv = probe.view();
    std::uint64_t local_key = 0, remote_key = 0;
    bool have_local = false, have_remote = false;
    for (std::uint64_t k = 1; k < 10'000 && !(have_local && have_remote); ++k) {
        const NodeId owner = *place(actor_id_of<Logger>(k), pv);
        if (owner.value == 1 && !have_local) {
            local_key = k;
            have_local = true;
        } else if (owner.value == 2 && !have_remote) {
            remote_key = k;
            have_remote = true;
        }
    }
    check(have_local && have_remote, "found both a node-1-owned and a node-2-owned Logger key", ok);

    LoopbackFabric fabric;
    // ClusterNode 1 hosts the local-owned Logger; node 2 hosts the remote-owned Logger.
    ClusterNode n1(NodeId{1}, fabric, local_key);
    ClusterNode n2(NodeId{2}, fabric, remote_key);
    n1.eng.start();
    n2.eng.start();

    constexpr int N = 500;

    // ---- (1) LOCAL FAST PATH: send to the node-1-owned actor from node 1's router. -------------
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(local_key);
        check(n1.dist->is_local(actor_id_of<Logger>(local_key)), "target is self-owned (local)", ok);
        const std::uint64_t sends_before = fabric.sends();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});
        check(wait_count(n1.actor.count, N), "local: all N delivered", ok);
        check(fabric.sends() == sends_before, "local send NEVER touches the transport (no wire)", ok);
        bool ordered = static_cast<int>(n1.actor.got.size()) == N;
        for (std::size_t i = 0; i < n1.actor.got.size(); ++i)
            if (n1.actor.got[i] != static_cast<int>(i)) ordered = false;
        check(ordered, "local: per-actor FIFO preserved", ok);
    }

    // ---- (2) REMOTE + tagless fast path: send to the node-2-owned actor from node 1's router. --
    {
        DistRef<Logger> ref = n1.dist->get<Logger>(remote_key);
        check(!n1.dist->is_local(actor_id_of<Logger>(remote_key)), "target is remote-owned", ok);
        const std::uint64_t sends_before = fabric.sends();
        for (int i = 0; i < N; ++i) ref.tell(Seq{i});  // serialized (tagless), crosses transport
        check(wait_count(n2.actor.count, N), "remote (tagless): all N delivered on node 2", ok);
        check(fabric.sends() == sends_before + N, "remote send goes through the transport (N frames)",
              ok);
        bool ordered = static_cast<int>(n2.actor.got.size()) >= N;
        for (int i = 0; i < N; ++i)
            if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
        check(ordered, "remote: 016 roundtrip correct + per-(sender,receiver) FIFO intact", ok);
        check(g_warns.load() == 0, "matched peers negotiate the tagless fast path (no fallback warn)",
              ok);
    }

    // ---- (3) REMOTE + canonical tagged fallback: force a schema mismatch (rolling-upgrade peer). --
    {
        n1.dist->set_peer_schema_provider(
            [](NodeId, TypeKey) { return PeerSchema{/*bogus fingerprint*/ 0xDEADBEEF, /*abi*/ 0}; });
        DistRef<Logger> ref = n1.dist->get<Logger>(remote_key);
        const std::uint64_t sends_before = fabric.sends();
        for (int i = 0; i < N; ++i) ref.tell(Seq{N + i});  // now encoded as canonical TAGGED
        check(wait_count(n2.actor.count, 2 * N), "remote (tagged): all N delivered on node 2", ok);
        check(fabric.sends() == sends_before + N, "remote tagged send goes through the transport", ok);
        bool ordered = static_cast<int>(n2.actor.got.size()) >= 2 * N;
        for (int i = 0; i < 2 * N; ++i)
            if (n2.actor.got[static_cast<std::size_t>(i)] != i) ordered = false;
        check(ordered, "tagged fallback roundtrips correctly + FIFO across both batches", ok);
        check(g_warns.load() > 0, "schema mismatch triggered the tagged-fallback warning (016)", ok);
    }

    n1.eng.stop();
    n2.eng.stop();
    g_wire_fallback_warn = nullptr;

    std::printf("distribution_routing_test: %s  (local_key=%llu remote_key=%llu)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(local_key),
                static_cast<unsigned long long>(remote_key));
    return ok ? 0 : 1;
}
