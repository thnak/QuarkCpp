// Tests 007-Failure-and-Supervision / ADR-009 — the handler-boundary guard END-TO-END through the
// real Engine (worker lanes + mailbox), proving the two headline containment properties:
//   1. A throwing handler is CONTAINED at the boundary — the worker lane never aborts; the engine
//      keeps running and services later messages.
//   2. A faulting `ask` resolves the caller's reply with an ERROR value — the caller never hangs
//      (reply-before-teardown, ADR-009 S2): the poison ask's ReplyCell is failed on reclaim, BEFORE
//      any restart touches state.
// The default supervision policy (Restart, assert-intact) keeps the actor draining across faults.
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Ping {
    bool boom;
};
struct Q {
    int x;
    bool boom;
};

struct Svc : Actor<Svc, Sequential> {  // default supervision = Restart (assert-intact)
    using protocol = Protocol<Ping, Ask<Q, int>>;

    void handle(const Ping& p) {
        if (p.boom) throw std::runtime_error("tell boom");  // faults at the boundary
    }
    void handle(const Ask<Q, int>& m) {
        if (m.query.boom) throw std::runtime_error("ask boom");  // faults BEFORE respond()
        m.respond(m.query.x * 2);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    detail::MessagePool pool(1024);
    Svc actor;
    auto act = std::make_unique<Activation>(&actor, Svc::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Svc>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Svc> ref = router.get<Svc>(1);
    eng.start();

    // 1) A faulting ask resolves to an ERROR (no hang) — the guard failed the reply cell on reclaim.
    result<int> bad = block_on(ref.ask<int>(Q{5, /*boom*/ true}));
    check(!bad.has_value(), "faulting ask resolves to an error (no caller hang)", ok);

    // 2) The engine keeps running after the fault: the next ask is serviced normally.
    result<int> good = block_on(ref.ask<int>(Q{7, /*boom*/ false}));
    check(good.has_value() && good.value() == 14, "engine healthy after an ask fault", ok);

    // 3) A throwing TELL is contained too; a following ask (FIFO after it) still resolves.
    ref.tell(Ping{/*boom*/ true});
    result<int> good2 = block_on(ref.ask<int>(Q{9, /*boom*/ false}));
    check(good2.has_value() && good2.value() == 18, "engine healthy after a tell fault", ok);

    // 4) Several interleaved faults do not degrade the lane.
    for (int i = 0; i < 50; ++i) {
        (void)block_on(ref.ask<int>(Q{i, /*boom*/ (i % 3) == 0}));
    }
    result<int> final = block_on(ref.ask<int>(Q{21, false}));
    check(final.has_value() && final.value() == 42, "lane survives a burst of faults", ok);

    eng.stop();
    std::printf("supervision_engine_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
