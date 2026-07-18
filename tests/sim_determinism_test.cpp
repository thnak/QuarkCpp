// Tests 014-Testing-Model §Deterministic simulation — the replay invariant: the SAME seed produces
// a BYTE-IDENTICAL message interleaving across two independent runs, and a DIFFERENT seed diverges.
// A ring of relays is flooded with tokens so that many activations are runnable at once — the seeded
// picker's choice among them IS the schedule. We witness the schedule two ways and compare both:
//   * the sim's own step trace + digest (which activation ran each step), and
//   * an application-level delivery log the handlers append to (the observed message order).
// Same seed ⇒ identical on both; a set of alternate seeds ⇒ different digests (reproducible-but-
// controllable nondeterminism, 014 §Alternatives considered).
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/sim_scheduler.hpp"

using namespace quark;

namespace {

struct Tok {
    int hops = 0;
};

// A relay: on each token, record its arrival, then forward a decremented token to the next relay in
// the ring until the hop budget is spent. `log`/`me` are wired by the harness (no restart here).
struct Relay : Actor<Relay, Sequential> {
    using protocol = Protocol<Tok>;
    ActorRef<Relay> next{};
    std::vector<int>* log = nullptr;
    int me = 0;
    int received = 0;

    void handle(const Tok& t) noexcept {
        ++received;
        if (log) log->push_back(me);
        if (t.hops > 0) next.tell(Tok{t.hops - 1});
    }
};

struct ScenarioResult {
    std::uint64_t digest = 0;
    std::vector<SimStep> trace;
    std::vector<int> applog;
    std::uint64_t posted = 0;
    std::uint64_t delivered = 0;
};

// One full deterministic run of the ring scenario under `seed`.
ScenarioResult run_scenario(std::uint64_t seed, unsigned k, int hops) {
    ScenarioResult out;
    SimEngine sim{seed};
    std::vector<ActorRef<Relay>> refs;
    for (unsigned i = 0; i < k; ++i) refs.push_back(sim.spawn<Relay>(i));
    for (unsigned i = 0; i < k; ++i) {
        Relay& r = sim.actor<Relay>(i);
        r.next = refs[(i + 1) % k];
        r.log = &out.applog;
        r.me = static_cast<int>(i);
    }
    // Flood: seed every relay at once ⇒ k concurrent runnables ⇒ the seed governs the interleaving.
    for (unsigned i = 0; i < k; ++i) refs[i].tell(Tok{hops});
    sim.run_until_idle();

    out.digest = sim.trace_digest();
    out.trace = sim.trace();
    out.posted = sim.posted();
    out.delivered = sim.delivered();
    return out;
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    constexpr unsigned kRelays = 6;
    constexpr int kHops = 4;

    // ---- Same seed ⇒ byte-identical schedule (the replay proof) --------------------------------
    const ScenarioResult a = run_scenario(42, kRelays, kHops);
    const ScenarioResult b = run_scenario(42, kRelays, kHops);

    check(a.digest == b.digest, "same seed: trace digest identical", ok);
    check(a.trace == b.trace, "same seed: step trace byte-identical", ok);
    check(a.applog == b.applog, "same seed: application delivery order identical", ok);
    check(!a.trace.empty(), "the scenario actually ran steps", ok);
    check(a.posted == a.delivered, "no lost message: every posted message was delivered", ok);
    check(a.posted == b.posted && a.delivered == b.delivered, "same seed: counters identical", ok);

    // ---- Different seeds ⇒ divergent schedules (controllable nondeterminism) --------------------
    unsigned differed = 0;
    for (std::uint64_t s : {1ULL, 2ULL, 7ULL, 9999ULL}) {
        const ScenarioResult c = run_scenario(s, kRelays, kHops);
        // Whatever the interleaving, the WORK is conserved (same total messages) — only the ORDER
        // changes with the seed.
        check(c.posted == a.posted, "different seed: same total work (conserved)", ok);
        check(c.delivered == c.posted, "different seed: no lost message", ok);
        if (c.digest != a.digest) ++differed;
    }
    check(differed >= 3, "different seeds diverge from the seed-42 schedule", ok);

    std::printf("sim_determinism_test: %s  (steps=%zu posted=%" PRIu64 " differed_seeds=%u/4)\n",
                ok ? "OK" : "FAIL", a.trace.size(), a.posted, differed);
    return ok ? 0 : 1;
}
