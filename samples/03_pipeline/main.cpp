// Quark sample 03 — Two-stage pipeline (actors sending to actors).
//
// Actors talk to each other exactly the way the outside world talks to them: an actor holds an
// `ActorRef` to a downstream actor and `tell`s it from inside a handler. `ActorRef` is a cheap,
// copyable value (an identity + a router pointer), so a stage just stores the next stage's ref.
//
// Topology:  driver --Ingest(n)--> [Doubler] --Deposit(2n)--> [Sink] --ask Total--> driver
//
// What it shows:
//   * two actors of different types living in one engine, addressed by distinct keys
//   * a handler that FORWARDS a derived message downstream (Doubler -> Sink)
//   * a "flush" ask on the middle stage that, by mailbox FIFO, guarantees every Ingest has been
//     processed (and therefore every Deposit already posted to Sink) before we read the final Total
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 03_pipeline
// Run  :  taskset -c 0-3 build/samples/03_pipeline
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

struct Ingest {
    int n;
};
struct Flush {};   // ask: "how many have you processed?" — also a FIFO barrier
struct Deposit {
    int n;
};
struct Total {};   // ask: "what's the running sum?"

// Terminal stage: accumulates deposits, answers the running total.
struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Deposit, Ask<Total, int>>;

    void handle(const Deposit& d) noexcept { sum_ += d.n; }
    void handle(const Ask<Total, int>& m) noexcept { m.respond(sum_); }

private:
    int sum_ = 0;
};

// Middle stage: doubles each value and forwards it downstream. `downstream` is set once, before the
// engine starts; forwarding a `tell` from inside a handler is just another enqueue (MPSC-safe).
struct Doubler : Actor<Doubler, Sequential> {
    using protocol = Protocol<Ingest, Ask<Flush, int>>;

    ActorRef<Sink> downstream{};  // wired at bring-up

    void handle(const Ingest& in) noexcept {
        ++count_;
        downstream.tell(Deposit{in.n * 2});
    }
    void handle(const Ask<Flush, int>& m) noexcept { m.respond(count_); }

private:
    int count_ = 0;
};

int main() {
    detail::MessagePool pool(1024);

    Sink sink;
    Doubler doubler;
    auto sink_act = std::make_unique<Activation>(&sink, Sink::dispatch_table(), pool.sink());
    auto dbl_act = std::make_unique<Activation>(&doubler, Doubler::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{/*workers*/ 2, /*shards*/ 1, /*budget*/ 64, 64});
    eng.register_activation(actor_id_of<Sink>(1), *sink_act);
    eng.register_activation(actor_id_of<Doubler>(2), *dbl_act);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Sink> sink_ref = router.get<Sink>(1);
    ActorRef<Doubler> dbl_ref = router.get<Doubler>(2);

    // Wire the pipeline: the Doubler forwards to the Sink. Set before start() so the ref is live the
    // first time a handler runs.
    doubler.downstream = sink_ref;

    eng.start();

    // Feed 1..10 into the head of the pipeline. Each becomes a Deposit(2n) at the Sink.
    int expected = 0;
    for (int n = 1; n <= 10; ++n) {
        dbl_ref.tell(Ingest{n});
        expected += n * 2;  // 2*(1+..+10) == 110
    }

    // Flush barrier: this ask FIFO-follows the 10 Ingests, so when it resolves the Doubler has
    // processed all of them and every Deposit is already sitting in the Sink's mailbox.
    result<int> processed = block_on(dbl_ref.ask<int>(Flush{}));
    std::printf("Doubler processed %d ingests  (expected 10)\n",
                processed.has_value() ? processed.value() : -1);

    // Now the Total ask enqueues AFTER those deposits -> the Sink drains them first, then answers.
    result<int> total = block_on(sink_ref.ask<int>(Total{}));
    std::printf("Sink total of doubled values:  %d  (expected %d)\n",
                total.has_value() ? total.value() : -1, expected);

    eng.stop();
    const bool ok = processed.has_value() && processed.value() == 10 &&
                    total.has_value() && total.value() == expected;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
