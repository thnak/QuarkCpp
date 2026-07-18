// Tests 025 §Part C §Failure and lifecycle: a faulting stateless activation is DISCARDED and its
// message DEAD-LETTERED (007) — there is no state to reconstruct, no Restart/quiesce/fencing. A poison
// message routed to one activation faults and dead-letters; a good message routed to a DIFFERENT
// activation is handled normally (the pool re-grows on demand — other activations are unaffected).
#include <cstdint>
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/stateless_pool.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct Job {
    bool poison = false;
};

struct FaultWorker : Actor<FaultWorker, Sequential> {
    using protocol = Protocol<Job>;
    std::uint64_t handled = 0;
    void handle(const Job& j) {
        if (j.poison) throw std::runtime_error("poison");  // a faulting handler (007)
        ++handled;
    }
};

// A dead-letter sink that counts routed failures (007).
std::uint64_t g_dead_letters = 0;
void dl_fn(Descriptor*, error, void*) noexcept { ++g_dead_letters; }

void drive(Activation& act) {
    while (true) {
        if (!act.try_acquire()) break;
        for (;;) {
            const auto o = act.drain_step(64);
            if (o == Activation::DrainOutcome::DrainedEmpty) {
                if (act.close_out()) continue;
                return;
            }
            if (o == Activation::DrainOutcome::BudgetExhausted) {
                act.yield_to_scheduled();
                break;
            }
            break;
        }
    }
}

}  // namespace

int main() {
    bool ok = true;

    DeadLetterSink dl{&dl_fn, nullptr};
    StatelessPool<FaultWorker> pool(2, PoolRoute::RoundRobin, dl);

    Job poison{true};
    Job good{false};
    Descriptor dp;
    dp.payload = &poison;
    stamp<FaultWorker, Job>(dp);
    Descriptor dg;
    dg.payload = &good;
    stamp<FaultWorker, Job>(dg);

    // Pin poison to slot 0, good to slot 1 (post_to bypasses the router for a deterministic outcome).
    pool.post_to(0, &dp);
    pool.post_to(1, &dg);

    drive(pool.activation(0));  // faults on poison → dead-lettered + activation stopped (discarded)
    drive(pool.activation(1));  // handles the good message normally

    check(g_dead_letters == 1, "the poison message was dead-lettered (007)", ok);
    check(pool.activation(0).is_stopped(),
          "the faulting activation is DISCARDED (Stop — no Restart/quiesce, 025 §Failure)", ok);
    check(pool.activation(0).dead_letters() == 1, "the faulting activation recorded 1 dead-letter", ok);
    check(pool.actor(1).handled == 1, "a good message on ANOTHER activation is unaffected", ok);
    check(!pool.activation(1).is_stopped(), "the healthy activation keeps running", ok);

    std::printf("  dead_letters=%llu  slot0.stopped=%d  slot1.handled=%llu\n",
                (unsigned long long)g_dead_letters, pool.activation(0).is_stopped(),
                (unsigned long long)pool.actor(1).handled);
    std::printf("stateless_pool_fault_deadletter_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
