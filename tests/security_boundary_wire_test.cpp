// Tests 020-Security §3 boundary authorization WIRED INTO THE REAL CROSS-NODE RECEIVE PATH
// (DistributedRouter::deliver, distribution.hpp) — not the standalone simulation in
// security_boundary_authz_test.cpp. Two-node loopback cluster (the same harness shape as
// distribution_routing_test.cpp):
//   * DENY: a frame carrying a principal that lacks the required right is refused BEFORE decode —
//     the target actor's state never advances, and the denial lands in the receiving node's
//     SecurityDeadLetter.
//   * ALLOW: a frame carrying a principal that dominates the required right is delivered normally,
//     proving the check is a real gate, not a permanent close.
//   * PROPAGATION: the send side stamps `MessageFrame::principal` from the SENDING actor's ambient
//     `MessageContext` (distribution.hpp's `send_remote`) — exercised here via `AmbientContextScope`,
//     the same RAII the engine uses internally around a handler dispatch (message_context.hpp).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/authorizer.hpp"
#include "quark/core/distribution.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/message_context.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

struct Seq {
    std::int32_t n = 0;
};
QUARK_SERIALIZE(Seq, (1, n));

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;
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

bool wait_count(std::atomic<int>& c, int want) {
    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (c.load(std::memory_order_acquire) < want)
        if (++spins > kStall) return false;
    return true;
}

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
        // ADR-044: this cluster genuinely round-trips a non-anonymous Principal (case 2 below), so
        // node 2's Activation must reclaim through the dual sink, not the plain pool's — an
        // EnvelopePool-sourced descriptor reclaimed via pool.sink() alone is the exact S1r corruption
        // bug the winning design's cross-examination found and fixed.
        act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), local.reclaim_sink());
        eng.register_activation(actor_id_of<Logger>(logger_key), *act);
        dist = std::make_unique<DistributedRouter>(membership, local, transport);
        dist->template register_remote<Logger, Seq>();
    }
};

constexpr std::uint64_t kSendRight = 1ULL << 0;

}  // namespace

int main() {
    bool ok = true;

    InProcessMembership probe(NodeId{1}, {NodeId{1}, NodeId{2}});
    MembershipView pv = probe.view();
    std::uint64_t remote_key = 0;
    bool have_remote = false;
    for (std::uint64_t k = 1; k < 10'000 && !have_remote; ++k) {
        if (place(actor_id_of<Logger>(k), pv)->value == 2) {
            remote_key = k;
            have_remote = true;
        }
    }
    check(have_remote, "found a node-2-owned Logger key", ok);

    LoopbackFabric fabric;
    ClusterNode n1(NodeId{1}, fabric, remote_key);
    ClusterNode n2(NodeId{2}, fabric, remote_key);

    // Node 2 (the receiver) requires kSendRight to deliver Seq to a Logger — Closed default means
    // any OTHER (actor,msg) pair is refused too, but this cluster only ever sends this one pair.
    AclAuthorizer authz(AclAuthorizer::Default::Closed);
    authz.require(type_key_of<Logger>(), type_key_of<Seq>(), kSendRight);
    SecurityDeadLetter sec_dl;
    n2.dist->set_authorizer(authz, &sec_dl);

    n1.eng.start();
    n2.eng.start();

    DistRef<Logger> ref = n1.dist->get<Logger>(remote_key);
    check(!n1.dist->is_local(actor_id_of<Logger>(remote_key)), "target is remote-owned", ok);

    // ---- (1) DENY: no ambient principal ⇒ anonymous (0 rights) ⇒ Closed ACL refuses it. ----------
    // LoopbackFabric::send() dispatches to deliver() INLINE on the calling thread (no cross-thread
    // hop until/unless the message is actually posted to the mailbox), so by the time tell() returns,
    // a denial has already happened synchronously — no polling race to account for on either check.
    {
        ref.tell(Seq{1});
        check(sec_dl.total() == 1, "denial recorded at the boundary", ok);
        check(n2.actor.count.load(std::memory_order_acquire) == 0,
              "denied frame never reaches the actor's handler", ok);
    }

    // ---- (2) ALLOW: an ambient principal dominating kSendRight ⇒ stamped onto the frame ⇒ admitted. --
    {
        MessageContext ctx{};
        ctx.principal = Principal{/*subject*/ 7, /*rights*/ kSendRight};
        {
            detail::AmbientContextScope amb(ctx);  // same RAII the engine publishes around a real handler
            ref.tell(Seq{2});
        }
        check(wait_count(n2.actor.count, 1), "allowed frame is delivered", ok);
        check(n2.actor.got.size() == 1 && n2.actor.got[0] == 2, "delivered payload is correct", ok);
        check(sec_dl.total() == 1, "allow path adds no new denial", ok);
    }

    n1.eng.stop();
    n2.eng.stop();

    std::printf("security_boundary_wire_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
