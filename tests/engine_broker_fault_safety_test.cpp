// Tests ADR-028 Phase 7 — Engine::handle_wake exception safety. Before this phase, both
// ActivationBroker::handle() and Engine::handle_wake() were marked `noexcept`, but handle_wake calls
// `m->construct()` (a plain `new A()`, compiled by make_construct_fn<A>()) — a lazily-activated
// actor's constructor throwing (or a bare allocation failure) unwound into a `noexcept` function and
// called std::terminate(), crashing the WHOLE PROCESS on its first touch, not just failing that one
// activation. handle_wake now wraps construct/wire/recover/Activation-build in a try/catch that
// dead-letters the original message instead — proven here via a constructor that always throws.
// Also sanity-checks the new 009 broker-convoy observability counters (broker_wakes_enqueued/handled,
// broker_stall_ns) got wired at all (not a precise-timing assertion).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <utility>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {
    int n = 1;
};

struct CountedMsg {
    static inline std::atomic<int> destructed{0};
    ~CountedMsg() { destructed.fetch_add(1, std::memory_order_release); }
};

// Throws unconditionally from its default constructor — the fault this phase must survive.
struct ThrowsOnConstruct : Actor<ThrowsOnConstruct, Sequential> {
    using protocol = Protocol<CountedMsg>;
    static inline std::atomic<int> constructions{0};
    ThrowsOnConstruct() {
        constructions.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("boom");
    }
    void handle(const CountedMsg&) noexcept {}
};

// A perfectly ordinary lazily-activated actor sharing the SAME (single-shard) broker — proves the
// broker itself survives a sibling id's constructor fault, not just "the process didn't crash".
struct Survivor : Actor<Survivor, Sequential> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<int> handled{0};
    void handle(const Ping& p) noexcept { handled.fetch_add(p.n, std::memory_order_release); }
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
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

}  // namespace

int main() {
    bool ok = true;

    // shards(1): both types hash to the SAME shard's broker, so scenario 2 below deterministically
    // proves the broker survives, rather than just happening to land on an unaffected shard.
    auto built = ConfigBuilder{}.workers(2).shards(1).default_drain_budget(64).idle_tick_ms(1).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_broker_fault_safety_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(256);
    check(eng.declare_lazy<ThrowsOnConstruct>(nullptr, pool.sink()).has_value(),
          "declare_lazy<ThrowsOnConstruct>", ok);
    check(eng.declare_lazy<Survivor>(nullptr, pool.sink()).has_value(), "declare_lazy<Survivor>", ok);

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    // ---- 1) a throwing constructor on first touch must NOT crash the process; the original message
    //         dead-letters through the wired reclaim sink instead. -----------------------------------
    {
        CountedMsg m;  // named local: its destructor fires at this block's closing brace, isolated
                       // from the tell() argument-temporary's own lifetime (mirrors the established
                       // pattern in engine_lazy_activation_test.cpp).
        router.tell<ThrowsOnConstruct>(actor_id_of<ThrowsOnConstruct>(1), std::move(m));
        check(wait_until([&] { return ThrowsOnConstruct::constructions.load() >= 1; },
                         std::chrono::seconds(2)),
              "the broker attempts construction (and the ctor throws)", ok);
        check(wait_until([&] { return CountedMsg::destructed.load(std::memory_order_acquire) >= 1; },
                         std::chrono::seconds(2)),
              "the original message dead-letters via the wired reclaim sink (no crash)", ok);
        check(eng.resolve(actor_id_of<ThrowsOnConstruct>(1)) == nullptr,
              "the faulted id never becomes live (no half-constructed activation)", ok);
    }

    // ---- 2) the SAME shard's broker keeps working: a sibling id activates normally afterward. -------
    {
        const ActorId id = actor_id_of<Survivor>(1);
        router.tell<Survivor>(id, Ping{5});
        check(wait_until([&] { return Survivor::handled.load(std::memory_order_acquire) >= 5; },
                         std::chrono::seconds(2)),
              "a sibling id on the same shard's broker still activates and handles normally", ok);
    }

    // ---- 3) basic sanity: the new 009 broker counters were actually wired. --------------------------
    {
        const MetricsSnapshot snap = eng.metrics_snapshot();
        check(snap.broker_wakes_enqueued >= snap.broker_wakes_handled,
              "broker_wakes_enqueued >= broker_wakes_handled (never handled more than enqueued)", ok);
        check(snap.broker_wakes_handled >= 2, "broker_wakes_handled counted both Wakes above", ok);
        check(snap.broker_stall_ns.count >= 2, "broker_stall_ns recorded a sample for both Wakes", ok);
    }

    eng.stop();
    std::printf("engine_broker_fault_safety_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
