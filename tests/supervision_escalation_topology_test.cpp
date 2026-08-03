// Tests 007 §Escalation (ADR-009 residual risk #6) — the `Supervision<Node|PerType>` escalation
// routing wired into `Engine::spawn<A>()`: an `Escalate`-decisioned fault (or a budget-exhausted
// Restart) TELLS the registered supervisor's lane with an `Escalated{source, cause}` message,
// exactly as 007's "escalate() tells the supervisor's lane — a message hop, never a synchronous
// cross-lane touch" invariant requires — driven through a real `Engine`, not a bare `Activation`.
//   * Supervision<Node> (the default) routes to the engine-wide default (`set_node_supervisor`).
//   * Supervision<PerType> routes to a supervisor registered specifically for that actor TYPE,
//     independent of the Node default.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/supervision.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct Boom {};

// Default topology (Supervision<Node>, implicit) — routes to the engine-wide default supervisor.
struct NodeFaulty : Actor<NodeFaulty, Sequential, OnFailure<Escalate>> {
    using protocol = Protocol<Boom>;
    void handle(const Boom&) { throw std::runtime_error("boom"); }
};

// Explicit Supervision<PerType> — routes to a supervisor registered for THIS type specifically.
struct PerTypeFaulty : Actor<PerTypeFaulty, Sequential, OnFailure<Escalate>, Supervision<PerType>> {
    using protocol = Protocol<Boom>;
    void handle(const Boom&) { throw std::runtime_error("boom"); }
};

// The Node-default supervisor.
struct NodeWatcher : Actor<NodeWatcher, Sequential> {
    using protocol = Protocol<Escalated>;
    static inline std::atomic<int> received{0};
    static inline std::atomic<std::uint64_t> last_source_key{0};
    void handle(const Escalated& m) {
        received.fetch_add(1, std::memory_order_relaxed);
        last_source_key.store(m.source.key, std::memory_order_relaxed);
    }
};

// A DISTINCT PerType supervisor — proves PerType routes independently of the Node default.
struct TypeWatcher : Actor<TypeWatcher, Sequential> {
    using protocol = Protocol<Escalated>;
    static inline std::atomic<int> received{0};
    void handle(const Escalated&) { received.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

int main() {
    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(256);

    auto node_watcher_id = eng.spawn<NodeWatcher>(1);
    check(node_watcher_id.has_value(), "spawn<NodeWatcher>");
    eng.set_node_supervisor<NodeWatcher>(*node_watcher_id);  // BEFORE spawning any escalating actor

    auto type_watcher_id = eng.spawn<TypeWatcher>(1);
    check(type_watcher_id.has_value(), "spawn<TypeWatcher>");
    eng.set_type_supervisor<PerTypeFaulty, TypeWatcher>(*type_watcher_id);

    auto node_faulty_id = eng.spawn<NodeFaulty>(1);
    check(node_faulty_id.has_value(), "spawn<NodeFaulty> (Supervision<Node>, the default)");
    auto per_type_faulty_id = eng.spawn<PerTypeFaulty>(2);
    check(per_type_faulty_id.has_value(), "spawn<PerTypeFaulty> (Supervision<PerType>)");

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    router.tell<NodeFaulty>(*node_faulty_id, Boom{});
    router.tell<PerTypeFaulty>(*per_type_faulty_id, Boom{});

    check(wait_until([&] { return NodeWatcher::received.load(std::memory_order_acquire) >= 1; },
                     std::chrono::seconds(2)),
          "Supervision<Node>: the node-default supervisor received the Escalated tell");
    check(wait_until([&] { return TypeWatcher::received.load(std::memory_order_acquire) >= 1; },
                     std::chrono::seconds(2)),
          "Supervision<PerType>: the per-type supervisor received the Escalated tell");
    check(NodeWatcher::last_source_key.load(std::memory_order_acquire) == node_faulty_id->key,
          "Escalated::source names the faulting actor's own ActorId");
    // The PerType escalation must NOT also have reached the Node-default supervisor a second time.
    check(NodeWatcher::received.load(std::memory_order_acquire) == 1,
          "Supervision<PerType> routes independently of the Node default (no double delivery)");

    eng.stop();

    std::printf("supervision_escalation_topology_test: %s (failures=%d)\n",
                g_failures ? "FAIL" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
