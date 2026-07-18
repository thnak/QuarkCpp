// Tests 005-Developer-Model end-to-end — an actor authored PURELY through the 005 developer surface:
// declared with CRTP policies, a `using protocol`, ordinary `handle` overloads; spawned via the
// `register_actor` registration path (band/budget resolved from its policies); and driven with the
// 006 `ActorRef` `tell` / `ask` verbs over the real engine. This is the "one clean way to write an
// actor" story, tying 001 (Actor CRTP), 005 (policies + registration), and 006 (ActorRef/tell/ask).
#include <cassert>
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

namespace {

// --- Messages ---------------------------------------------------------------------------------
struct Add {
    int amount;
};
struct GetTotal {};

// --- The actor, declared entirely through the 005 surface. ------------------------------------
//   * CRTP base with policies: Sequential execution, high priority, a small drain budget.
//   * `using protocol` enumerates the dispatched message set (a tell + an ask envelope).
//   * ordinary member `handle` overloads (sync tell; ask via the reply-carrying envelope).
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    void handle(const Add& a) noexcept { total_ += a.amount; }
    void handle(const Ask<GetTotal, int>& m) noexcept { m.respond(total_); }

private:
    int total_ = 0;
};

// The policies the author declared are visible to the engine as metadata (compile-time).
static_assert(is_reentrant_v<Counter> == false);
static_assert(priority_band_of<Counter>() == 0);
static_assert(drain_budget_of<Counter>() == 16);

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // --- Wiring: pool + actor instance + activation + engine + router (the standard bring-up). ---
    detail::MessagePool pool(1024);
    Counter counter;
    auto act = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

    // A 2-band engine so the resolved Priority<0> band is exercised (not clamped away by K==1).
    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});

    // The 005 registration path: band/budget resolved from Counter's policies. Keyed like ActorRef.
    Schedulable* s = register_actor<Counter>(eng, /*key*/ 42, *act);
    check(s->band == 0, "Counter Priority<0> resolved to band 0", ok);
    check(s->budget == 16, "Counter DrainBudget<16> resolved to budget 16", ok);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> ref = router.get<Counter>(42);
    eng.start();

    // --- tell: fire-and-forget accumulation (compile error if Add were not in the protocol). -----
    int expected = 0;
    for (int i = 1; i <= 100; ++i) {
        ref.tell(Add{i});
        expected += i;
    }

    // --- ask: request/reply for the running total. Sequential ⇒ the ask observes all prior tells
    // (mailbox FIFO — requests never reorder past the tells enqueued before them). ----------------
    result<int> r = block_on(ref.ask<int>(GetTotal{}));
    check(r.has_value(), "ask resolved (has value)", ok);
    check(r.has_value() && r.value() == expected, "ask observed all prior tells (FIFO, correct sum)", ok);

    // A second round proves the actor keeps state across messages.
    ref.tell(Add{1000});
    expected += 1000;
    result<int> r2 = block_on(ref.ask<int>(GetTotal{}));
    check(r2.has_value() && r2.value() == expected, "state persists across messages", ok);

    eng.stop();
    std::printf("authoring_e2e_test: %s  (total=%d)\n", ok ? "OK" : "FAIL",
                r2.has_value() ? r2.value() : -1);
    return ok ? 0 : 1;
}
