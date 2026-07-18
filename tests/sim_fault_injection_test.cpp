// Tests 014-Testing-Model §Fault injection — seeded/explicit faults are observable AND reproducible,
// and drive real 007 supervision deterministically:
//   * transport DROP — the Nth message to a target is dropped at the post seam; observable via the
//     sim's dropped()/delivered() counters and the actor's own state; replays identically.
//   * handler FAULT — a cooperating handler throws on the scheduled dispatch, driving a Restart
//     through the production handler-boundary guard (fresh-state reconstruct), deterministically.
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "quark/core/actor.hpp"
#include "quark/core/sim_scheduler.hpp"

using namespace quark;

namespace {

struct Bump {};

// ---- Transport-drop target: a plain counter ---------------------------------------------------
struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Bump>;
    int n = 0;
    void handle(const Bump&) noexcept { ++n; }
};

// ---- Handler-fault target: throws on the injected dispatch, driving supervision Restart ---------
// The fault decision is queried from the sim via file-scope wiring (NOT actor state) so it survives
// the reconstruct a Restart performs (which default-constructs a fresh actor, wiping members).
SimEngine* g_sim = nullptr;
ActorId g_fid{};

struct Faulty : Actor<Faulty, Sequential> {  // default supervision = Restart (007)
    using protocol = Protocol<Bump>;
    int n = 0;
    void handle(const Bump&) {
        if (g_sim && g_sim->consume_handler_fault(g_fid)) throw std::runtime_error("injected");
        ++n;
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// A full transport-drop run: 5 bumps with the 3rd dropped. Returns the observed final count.
int run_drop(std::uint64_t seed, std::uint64_t& dropped, std::uint64_t& posted,
             std::uint64_t& delivered, bool& no_lost) {
    SimEngine sim{seed};
    auto ref = sim.spawn<Counter>(1);
    sim.drop_next_to(actor_id_of<Counter>(1), 3);  // drop the 3rd delivery
    for (int i = 0; i < 5; ++i) ref.tell(Bump{});
    sim.run_until_idle();
    dropped = sim.dropped();
    posted = sim.posted();
    delivered = sim.delivered();
    no_lost = sim.no_lost_message();
    return sim.actor<Counter>(1).n;
}

// A full handler-fault run: 5 bumps, fault on the 3rd dispatch ⇒ Restart resets the counter.
int run_fault(std::uint64_t seed, std::uint32_t& restarts, std::uint32_t& faults) {
    SimEngine sim{seed};
    auto ref = sim.spawn<Faulty>(1);
    g_sim = &sim;
    g_fid = actor_id_of<Faulty>(1);
    sim.arm_handler_fault(g_fid, 3);
    for (int i = 0; i < 5; ++i) ref.tell(Bump{});
    sim.run_until_idle();
    restarts = sim.activation<Faulty>(1).restarts_total();
    faults = sim.activation<Faulty>(1).faults();
    const int n = sim.actor<Faulty>(1).n;
    g_sim = nullptr;
    return n;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Transport drop: observable + reproducible ---------------------------------------------
    {
        std::uint64_t dropped = 0, posted = 0, delivered = 0;
        bool no_lost = false;
        const int n1 = run_drop(7, dropped, posted, delivered, no_lost);
        check(n1 == 4, "drop: 5 bumps - 1 dropped = 4 handled", ok);
        check(dropped == 1, "drop: exactly one message dropped at the transport", ok);
        check(posted == 4 && delivered == 4, "drop: 4 accepted, 4 delivered (dropped never enqueued)",
              ok);
        check(no_lost, "drop: no lost message (every accepted message reached the actor)", ok);

        // Reproducible: a second run with the same seed yields the identical outcome.
        std::uint64_t d2 = 0, p2 = 0, dl2 = 0;
        bool nl2 = false;
        const int n2 = run_drop(7, d2, p2, dl2, nl2);
        check(n1 == n2 && dropped == d2 && posted == p2, "drop: replays identically under same seed",
              ok);
    }

    // ---- Handler fault → supervision Restart: deterministic ------------------------------------
    {
        std::uint32_t restarts = 0, faults = 0;
        const int n1 = run_fault(123, restarts, faults);
        // bumps 1,2 → n=2; bump 3 faults → Restart reconstructs fresh (n=0); bumps 4,5 → n=2.
        check(n1 == 2, "fault: Restart reset state, then two post-restart bumps ⇒ n==2", ok);
        check(restarts == 1, "fault: exactly one supervised Restart driven", ok);
        check(faults == 1, "fault: exactly one handler fault contained at the boundary", ok);

        std::uint32_t r2 = 0, f2 = 0;
        const int n2 = run_fault(123, r2, f2);
        check(n1 == n2 && restarts == r2 && faults == f2, "fault: replays identically under same seed",
              ok);
    }

    // ---- Seeded random drop is deterministic under a fixed seed --------------------------------
    {
        auto run_random = [](std::uint64_t seed) {
            SimEngine sim{seed};
            auto ref = sim.spawn<Counter>(1);
            sim.set_random_drop(0.5);
            for (int i = 0; i < 200; ++i) ref.tell(Bump{});
            sim.run_until_idle();
            return sim.dropped();
        };
        const std::uint64_t a = run_random(555);
        const std::uint64_t b = run_random(555);
        check(a == b, "random drop: identical drop count under the same seed (reproducible)", ok);
        check(a > 0 && a < 200, "random drop: some but not all messages dropped (p=0.5)", ok);
    }

    std::printf("sim_fault_injection_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
