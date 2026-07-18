// Tests 014-Testing-Model §Unit harness (`TestKit`) end-to-end + §Deterministic simulation multi-
// actor final state:
//   * TestKit drives ONE actor: an inbound `ask` gets a reply, the actor's outbound `tell` is
//     captured (content-asserted), state is exposed for assertions, and a timer fires under virtual
//     time — all on one thread, no engine.
//   * SimEngine runs a two-actor ping-pong to a deterministic final state via the public API.
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/sim_scheduler.hpp"
#include "quark/core/testkit.hpp"

using namespace quark;
using namespace std::chrono_literals;

namespace {

// ---- TestKit scenario: an Order actor that replies + reserves + ships + ticks ------------------
struct Query {
    int qty = 0;
};
struct Confirm {
    bool ok = false;
};
struct Reserve {
    int qty = 0;
};
struct Ship {};
struct Tick {};

struct Inventory : Actor<Inventory, Sequential> {  // capture target only (never instantiated)
    using protocol = Protocol<Reserve>;
    void handle(const Reserve&) noexcept {}
};

struct Order : Actor<Order, Sequential> {
    using protocol = Protocol<Ask<Query, Confirm>, Ship, Tick>;
    ActorRef<Inventory> inv{};
    bool shipped = false;
    int ticks = 0;
    int last_qty = -1;

    void handle(const Ask<Query, Confirm>& m) {
        last_qty = m.query.qty;
        inv.tell(Reserve{m.query.qty});   // outbound (captured by TestKit)
        m.respond(Confirm{true});          // reply (resolves the ask cell)
    }
    void handle(const Ship&) noexcept { shipped = true; }
    void handle(const Tick&) noexcept { ++ticks; }
};

// ---- SimEngine ping-pong ----------------------------------------------------------------------
struct Ball {
    int n = 0;
};
struct Player : Actor<Player, Sequential> {
    using protocol = Protocol<Ball>;
    ActorRef<Player> partner{};
    int hits = 0;
    void handle(const Ball& b) noexcept {
        ++hits;
        if (b.n > 0) partner.tell(Ball{b.n - 1});
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

    // ---- TestKit end-to-end --------------------------------------------------------------------
    {
        TestKit<Order> kit;
        kit.actor().inv = kit.ref<Inventory>(1);  // a CAPTURING ref (sends are recorded)

        // Inbound ask → reply value + outbound capture.
        result<Confirm> reply = kit.ask<Confirm>(Query{7});
        check(reply.has_value(), "ask: the actor replied (cell resolved, no hang)", ok);
        check(reply.has_value() && reply->ok, "ask: reply value is Confirm{ok=true}", ok);
        check(kit.count_tells_to<Inventory, Reserve>() == 1, "ask: one outbound Reserve captured", ok);
        check(!kit.tells_to<Inventory, Reserve>().empty() &&
                  kit.tells_to<Inventory, Reserve>()[0].qty == 7,
              "ask: captured Reserve carries qty=7 (exact outbound content)", ok);

        // Inbound tell → state assertion.
        check(!kit.actor().shipped, "pre: not shipped", ok);
        kit.tell(Ship{});
        check(kit.assert_state([](const Order& o) { return o.shipped; }), "tell: actor shipped", ok);

        // Virtual time → a scheduled Tick fires only after enough time is advanced.
        kit.schedule_after(5ms, Tick{});
        kit.advance(4ms);
        check(kit.actor().ticks == 0, "timer: not yet fired at t=4ms (< 5ms)", ok);
        kit.advance(2ms);
        check(kit.actor().ticks == 1, "timer: fired exactly once by t=6ms", ok);
    }

    // ---- SimEngine ping-pong to a deterministic final state ------------------------------------
    {
        for (std::uint64_t seed : {1ULL, 42ULL, 9999ULL}) {
            SimEngine sim{seed};
            auto a = sim.spawn<Player>(1);
            auto b = sim.spawn<Player>(2);
            sim.actor<Player>(1).partner = b;
            sim.actor<Player>(2).partner = a;

            a.tell(Ball{10});
            sim.run_until_idle();

            // A receives 10,8,6,4,2,0 (6 hits); B receives 9,7,5,3,1 (5 hits). Independent of seed
            // because a strict ping-pong has one runnable actor at a time — the FINAL STATE is stable.
            check(sim.actor<Player>(1).hits == 6, "ping-pong: player A hit 6 times", ok);
            check(sim.actor<Player>(2).hits == 5, "ping-pong: player B hit 5 times", ok);
            check(sim.posted() == sim.delivered(), "ping-pong: no lost message", ok);
            check(sim.no_lost_message(), "ping-pong: idle with every message accounted for", ok);
        }
    }

    std::printf("sim_testkit_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
