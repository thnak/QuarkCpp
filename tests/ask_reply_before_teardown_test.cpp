// Tests 006/007 seam — reply-before-teardown: an ask whose handler produces NO reply must still
// resolve the caller's cell (with a failure value), so the caller never hangs. The guarantee is
// carried by the Responder destructor: when the ask payload is reclaimed (on completion, tombstone,
// or supervisor teardown) without a reply, the pending cell is failed. A normal ask still resolves
// with its value. Full supervisor-driven mass-fail of in-flight cells is 007's; this proves the
// per-cell mechanism every such path reuses.
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Req {
    int x;
    bool answer;  // handler replies iff true — else it "forgets" (simulates a drop/teardown)
};

struct Flaky : Actor<Flaky, Sequential> {
    using protocol = Protocol<Ask<Req, int>>;
    void handle(const Ask<Req, int>& m) noexcept {
        if (m.query.answer) m.respond(m.query.x);
        // else: no respond() — the Responder destructor fails the cell on payload reclaim.
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
    Flaky actor;
    auto act = std::make_unique<Activation>(&actor, Flaky::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Flaky>(3), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Flaky> ref = router.get<Flaky>(3);
    eng.start();

    // Normal completion resolves with the value.
    result<int> good = block_on(ref.ask<int>(Req{123, true}));
    check(good.has_value() && good.value() == 123, "answered ask resolves with its value", ok);

    // Handler forgets to reply → the cell is failed on reclaim; block_on returns (does NOT hang).
    result<int> dropped = block_on(ref.ask<int>(Req{456, false}));
    check(!dropped.has_value(), "unanswered ask resolves to an error (no caller hang)", ok);
    check(!dropped.has_value() && dropped.error().code == errc::supervised_stop,
          "unanswered ask fails with supervised_stop", ok);

    // The path still works afterwards (pool/cell recycled cleanly).
    result<int> good2 = block_on(ref.ask<int>(Req{789, true}));
    check(good2.has_value() && good2.value() == 789, "engine healthy after a dropped reply", ok);

    eng.stop();
    std::printf("ask_reply_before_teardown_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
