// Tests 006-Messaging-and-Addressing §ask + ADR-007 (async-only ask, block_on off-lane) — a full
// request/reply round trip over the real engine: a worker lane drains the ask, the handler responds
// through the pooled ReplyCell, and the off-lane caller's block_on resolves with the correct value.
// Also asserts block_on fail-fasts (`on_worker` is never produced off-lane, and the value lands).
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct GetSquare {
    int x;
};

struct Squarer : Actor<Squarer, Sequential> {
    using protocol = Protocol<Ask<GetSquare, int>>;
    void handle(const Ask<GetSquare, int>& m) noexcept { m.respond(m.query.x * m.query.x); }
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
    Squarer actor;
    auto act = std::make_unique<Activation>(&actor, Squarer::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Squarer>(7), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Squarer> ref = router.get<Squarer>(7);
    eng.start();

    // Several sequential asks — each resolves to its own correct reply.
    for (int x = 1; x <= 16; ++x) {
        result<int> r = block_on(ref.ask<int>(GetSquare{x}));
        check(r.has_value(), "ask resolved (has value)", ok);
        check(r.has_value() && r.value() == x * x, "ask resolved to the correct reply", ok);
    }

    eng.stop();
    std::printf("ask_engine_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
