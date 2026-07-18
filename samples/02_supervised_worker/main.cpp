// Quark sample 02 — Supervised worker (fault containment).
//
// Actors fail in isolation. A handler that throws is CONTAINED at the handler boundary (007 / ADR-009)
// — the worker lane does not abort, the engine keeps running, and later messages are still served.
// A faulting `ask` resolves the caller's reply with an ERROR value, so a caller never hangs waiting on
// a reply that will never come.
//
// What it shows:
//   * the DEFAULT supervision policy (Restart, assert-intact) — no extra wiring needed
//   * a poison `tell` that throws is swallowed at the boundary; the actor keeps draining
//   * a poison `ask` that throws BEFORE respond() comes back as a failed result<> (never a hang)
//   * good messages before AND after the faults are all processed correctly
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 02_supervised_worker
// Run  :  taskset -c 0-3 build/samples/02_supervised_worker
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

struct Work {
    int n;
    bool boom;  // if true the handler throws — simulating a bug / bad input
};
struct Square {
    int n;
    bool boom;
};

// Default supervision == Restart(assert-intact): the guard contains the throw and the actor keeps
// serving. No supervision policy is spelled out here — this is the out-of-the-box behavior.
struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Work, Ask<Square, int>>;

    void handle(const Work& w) {
        if (w.boom) throw std::runtime_error("poison tell");  // contained at the boundary
        processed_ += w.n;
    }
    void handle(const Ask<Square, int>& m) {
        if (m.query.boom) throw std::runtime_error("poison ask");  // faults BEFORE respond()
        m.respond(m.query.n * m.query.n);
    }

private:
    int processed_ = 0;
};

int main() {
    detail::MessagePool pool(1024);
    Worker worker;
    auto activation = std::make_unique<Activation>(&worker, Worker::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Worker>(1), *activation);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Worker> ref = router.get<Worker>(1);
    eng.start();

    bool ok = true;

    // 1) A poison tell throws inside the handler. It is contained — no crash — and the engine lives.
    ref.tell(Work{/*n*/ 5, /*boom*/ true});

    // 2) A GOOD ask still works right after the fault: proves the lane survived and keeps serving.
    result<int> good = block_on(ref.ask<int>(Square{/*n*/ 7, /*boom*/ false}));
    ok &= good.has_value() && good.value() == 49;
    std::printf("after a poison tell, a good ask still answers: 7^2 = %d  (expected 49)\n",
                good.has_value() ? good.value() : -1);

    // 3) A poison ask throws BEFORE respond(). The caller does NOT hang — the reply resolves to an
    //    error, so block_on returns a failed result<> we can branch on.
    result<int> poisoned = block_on(ref.ask<int>(Square{/*n*/ 9, /*boom*/ true}));
    ok &= !poisoned.has_value();
    std::printf("a poison ask resolves to an ERROR (no hang): has_value=%s\n",
                poisoned.has_value() ? "true (BUG)" : "false");

    // 4) And the engine is STILL healthy after the poison ask — one more good ask proves it.
    result<int> again = block_on(ref.ask<int>(Square{/*n*/ 6, /*boom*/ false}));
    ok &= again.has_value() && again.value() == 36;
    std::printf("engine healthy after poison ask: 6^2 = %d  (expected 36)\n",
                again.has_value() ? again.value() : -1);

    eng.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
