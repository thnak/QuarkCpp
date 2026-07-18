// Tests 008-Metadata-and-Startup INTEGRATION — the 007 reconstruct seam wired end-to-end through the
// real Engine (worker lanes + mailbox). The headline invariant:
//
//   An actor whose handler FAULTS and is Restart-supervised comes back with GENUINELY FRESH STATE
//   (a counter reset via the 008-compiled reconstruct factory), NOT merely re-activated intact.
//
// The proof is a CONTRAST between two identical actors on the same engine:
//   * SPAWNED via Engine::spawn<A>() — the 008 keystone wires set_reconstruct(make_reconstruct_sink<A>())
//     → after a fault the counter is 0 (fresh state).
//   * REGISTERED manually via register_activation() with NO reconstruct sink (007's assert-intact
//     default) → after the same fault the counter KEEPS its (incremented) value.
// The delta (0 vs kept) is exactly the reconstruct factory doing its job.
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/supervision.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Bump {
    bool boom = false;
};
struct GetCount {};

// Default supervision = Restart with an unbounded budget (007), so it re-activates across faults.
struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Bump, Ask<GetCount, int>>;
    int count = 0;
    void handle(const Bump& b) {
        ++count;                                          // mutate FIRST …
        if (b.boom) throw std::runtime_error("boom");     // … then fault at the boundary
    }
    void handle(const Ask<GetCount, int>& m) { m.respond(count); }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

int ask_count(ActorRef<Counter>& ref) {
    result<int> r = block_on(ref.ask<int>(GetCount{}));
    return r.value_or(-1);
}

}  // namespace

int main() {
    bool ok = true;

    detail::MessagePool pool(1024);
    Engine<> eng(EngineConfig{2, 2, 64, 64});

    // A) The 008 keystone: spawn wires supervision + the fresh-state reconstruct factory.
    result<ActorId> spawned = eng.spawn<Counter>(1, pool.sink());
    check(spawned.has_value(), "spawn<Counter> succeeds", ok);

    // B) Contrast actor: manual registration with NO reconstruct sink (assert-intact default).
    Counter manual_actor;
    auto manual_act = std::make_unique<Activation>(&manual_actor, Counter::dispatch_table(),
                                                   pool.sink(), max_concurrency_of<Counter>(),
                                                   supervision_of<Counter>());
    // NOTE: deliberately NOT calling set_reconstruct — this is the assert-intact 007 default.
    eng.register_activation(actor_id_of<Counter>(2), *manual_act,
                            static_cast<std::uint16_t>(priority_band_of<Counter>()),
                            drain_budget_of<Counter>());

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> a = router.get<Counter>(1);  // spawned (reconstruct wired)
    ActorRef<Counter> b = router.get<Counter>(2);  // manual (assert-intact)
    eng.start();

    // Drive both to count == 5.
    for (int i = 0; i < 5; ++i) {
        a.tell(Bump{false});
        b.tell(Bump{false});
    }
    check(ask_count(a) == 5, "spawned actor counted to 5", ok);
    check(ask_count(b) == 5, "manual actor counted to 5", ok);

    // Fault both: the boom handler does ++count (→6) then throws → Restart supervision fires.
    a.tell(Bump{true});
    b.tell(Bump{true});

    // The ask is FIFO-behind the faulting bump, so it observes the post-restart state.
    const int a_after = ask_count(a);
    const int b_after = ask_count(b);
    check(a_after == 0, "SPAWNED: Restart reconstructed FRESH state (count == 0)", ok);
    check(b_after == 6, "MANUAL: assert-intact Restart KEPT state (count == 6)", ok);

    // The reconstructed actor is fully healthy — it counts again from the fresh baseline.
    a.tell(Bump{false});
    check(ask_count(a) == 1, "reconstructed actor resumes counting from fresh (== 1)", ok);

    eng.stop();
    std::printf("metadata_restart_reconstruct_test: %s (spawned=%d manual=%d)\n",
                ok ? "OK" : "FAIL", a_after, b_after);
    return ok ? 0 : 1;
}
