// Quark sample 01 — Hello, Counter.
//
// The smallest complete Quark program: one actor, authored purely through the 005 developer surface,
// driven by `tell` (fire-and-forget) and `ask` (request/reply) over the real engine. If you read one
// sample, read this one — every other sample is this shape plus one idea.
//
// What it shows:
//   * declaring an actor with CRTP + policies (Sequential execution, a priority band, a drain budget)
//   * a `using protocol` that enumerates the messages the actor accepts (tells + ask envelopes)
//   * ordinary `handle(...)` overloads — one per message; the compiler wires the dispatch table
//   * the standard bring-up: MessagePool -> Activation -> Engine -> register -> LocalRouter -> ActorRef
//   * `ref.tell(...)` accumulates state; `block_on(ref.ask<R>(...))` reads it back
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 01_hello_counter
// Run  :  taskset -c 0-3 build/samples/01_hello_counter      (pin to <=4 cores — never saturate)
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/spawn.hpp"

using namespace quark;

// --- Messages: plain structs. A `tell` carries one; an `ask` pairs a query with a reply type. -----
struct Add {
    int amount;
};
struct GetTotal {};

// --- The actor. Policies in the CRTP base ARE the actor's metadata, read by the engine at compile
// time (band, budget, reentrancy). `protocol` is the closed set of messages it dispatches. ---------
struct Counter : Actor<Counter, Sequential, Priority<0>, DrainBudget<16>> {
    using protocol = Protocol<Add, Ask<GetTotal, int>>;

    void handle(const Add& a) noexcept { total_ += a.amount; }                 // tell: mutate state
    void handle(const Ask<GetTotal, int>& m) noexcept { m.respond(total_); }   // ask: answer the caller

private:
    int total_ = 0;
};

int main() {
    // Bring-up. A MessagePool backs zero-alloc message envelopes; the Activation binds the actor
    // instance to its (compile-time) dispatch table; the Engine owns the worker lane(s).
    detail::MessagePool pool(1024);
    Counter counter;
    auto activation = std::make_unique<Activation>(&counter, Counter::dispatch_table(), pool.sink());

    Engine<PriorityBands<2>> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});

    // The 005 registration path resolves the band/budget from Counter's policies. Key 42 is this
    // actor's identity; a matching `router.get<Counter>(42)` addresses it.
    register_actor<Counter>(eng, /*key*/ 42, *activation);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Counter> counter_ref = router.get<Counter>(42);
    eng.start();

    // tell: 1 + 2 + ... + 100 == 5050, fire-and-forget (no reply, no blocking, no hot-path alloc).
    for (int i = 1; i <= 100; ++i) counter_ref.tell(Add{i});

    // ask: request/reply. Sequential execution + mailbox FIFO means this observes ALL prior tells.
    // block_on drives the caller side until the reply (or an error) resolves.
    result<int> total = block_on(counter_ref.ask<int>(GetTotal{}));

    std::printf("Counter total after 100 tells: %d  (expected 5050)\n",
                total.has_value() ? total.value() : -1);

    // State persists across messages — a second batch keeps accumulating on top.
    counter_ref.tell(Add{1000});
    result<int> total2 = block_on(counter_ref.ask<int>(GetTotal{}));
    std::printf("Counter total after +1000:     %d  (expected 6050)\n",
                total2.has_value() ? total2.value() : -1);

    eng.stop();

    const bool ok = total.has_value() && total.value() == 5050 &&
                    total2.has_value() && total2.value() == 6050;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
