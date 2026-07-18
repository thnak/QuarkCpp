// Tests 002-Scheduler §Wakeup — the isolating no-lost-wakeup control. A producer posts ONE message
// at a time to activations spread across shards and, between posts, waits for the message to drain —
// which forces the worker lanes to reach the park/wake boundary EVERY round. If the targeted wakeup
// ever loses the {run-queue tail, idle_mask} Dekker, a `Scheduled` activation is stranded Idle-with-
// work and the round's message is never drained → the round times out.
//
// TEETH: compiled with -DQUARK_SCHED_BROKEN_WAKEUP the engine's park() DROPS the seq_cst barrier +
// rescan, so a producer that enqueues in the announce-idle window is never observed and the message
// IS stranded. In that (control) build a stall is the EXPECTED, correct outcome — it proves the test
// can actually detect a lost wakeup. In the normal build a stall is a bug.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Ping {
    std::uint64_t seq;
};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<std::uint64_t>* dispatched = nullptr;
    void handle(const Ping&) noexcept { dispatched->fetch_add(1, std::memory_order_release); }
};

}  // namespace

int main() {
#if defined(QUARK_SCHED_BROKEN_WAKEUP)
    constexpr bool kControl = true;
    constexpr std::uint64_t kRounds = 2'000'000;              // control must strand well before this
    constexpr std::uint64_t kStallSpins = 50'000'000ULL;     // control strands fast — small bound
    // ONE lane in the control: a lost wakeup strands the activation with NO other worker able to
    // work-STEAL and rescue it. With >1 worker a stolen activation masks the lost wakeup, so the
    // control only strands probabilistically (flaky). Single-lane makes the strand deterministic —
    // the teeth are exactly as sharp, and the isolation is cleaner.
    constexpr std::uint32_t kWorkers = 1;
#else
    constexpr bool kControl = false;
    constexpr std::uint64_t kRounds = 200'000;
    constexpr std::uint64_t kStallSpins = 2'000'000'000ULL;  // per-round strand bound (generous margin)
    constexpr std::uint32_t kWorkers = 3;                    // multi-worker realism (wakeup + steal)
#endif
    constexpr std::uint32_t kShards = 4;
    constexpr std::uint32_t kActors = 8;

    std::atomic<std::uint64_t> dispatched{0};

    // Distinct actors across shards; distinct pre-allocated descriptor + payload per round (no pool
    // reuse race — the producer never touches a descriptor a worker is reclaiming).
    std::vector<Sink> actors(kActors);
    Engine<> eng(EngineConfig{kWorkers, kShards, /*budget*/ 64, /*busy_spin*/ 64});
    std::vector<Schedulable*> sched(kActors);

    // Activations need stable addresses (they hold atomics + a Mailbox — non-movable), so own them
    // behind unique_ptr rather than in a reallocating vector of values.
    std::vector<std::unique_ptr<Activation>> activations;
    activations.reserve(kActors);
    for (std::uint32_t a = 0; a < kActors; ++a) {
        actors[a].dispatched = &dispatched;
        activations.push_back(std::make_unique<Activation>(&actors[a], Sink::dispatch_table()));
        sched[a] = eng.register_activation(ActorId{TypeKey{1}, a}, *activations[a]);
    }

    std::vector<Ping> msgs(kRounds);
    std::vector<Descriptor> descs(kRounds);
    for (std::uint64_t i = 0; i < kRounds; ++i) {
        msgs[i].seq = i;
        descs[i].payload = &msgs[i];
        stamp<Sink, Ping>(descs[i]);
    }

    eng.start();

    bool stalled = false;
    std::uint64_t completed_rounds = 0;
    for (std::uint64_t r = 0; r < kRounds; ++r) {
        const std::uint32_t a = static_cast<std::uint32_t>(r % kActors);
        eng.post(sched[a], &descs[r]);
        // Wait for THIS round's message to drain — forcing the lanes back to the park boundary.
        const std::uint64_t target = r + 1;
        std::uint64_t spins = 0;
        while (dispatched.load(std::memory_order_acquire) < target) {
            if (++spins > kStallSpins) {
                stalled = true;
                break;
            }
        }
        if (stalled) break;
        completed_rounds = target;
    }

    eng.stop();  // wake_all frees any parked/stranded lane so the process exits cleanly

    const std::uint64_t drained = dispatched.load();
    if (kControl) {
        // The control MUST strand to prove the harness has teeth.
        const bool ok = stalled;
        std::printf("sched_no_lost_wakeup_test [CONTROL -DQUARK_SCHED_BROKEN_WAKEUP]: %s  "
                    "(stranded after %" PRIu64 " rounds, drained=%" PRIu64 ")\n",
                    ok ? "OK (correctly stranded — teeth confirmed)" : "FAIL (never stranded!)",
                    completed_rounds, drained);
        return ok ? 0 : 1;
    }
    const bool ok = !stalled && drained == kRounds;
    std::printf("sched_no_lost_wakeup_test: %s  (rounds=%" PRIu64 " drained=%" PRIu64 " stalled=%d)\n",
                ok ? "OK" : "FAIL", kRounds, drained, static_cast<int>(stalled));
    return ok ? 0 : 1;
}
