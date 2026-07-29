// Tests ADR-028 Phase 8 §On-demand passivation — a REAL, running Engine, exercising
// ActorRef<A>::passivate() end to end (mirrors tests/engine_idle_timeout_eviction_test.cpp's style):
//
//   (a) .passivate() on a live KeepAlive Sequential actor (no IdleTimeout at all -- proving the
//       on-demand path is fully independent of the automatic wheel) drains queued mail then reaches
//       Dormant.
//   (b) A message racing the retirement still dispatches (the SAME abort-eviction Dekker sequence,
//       now exercised via the on-demand trigger instead of the timer).
//   (c) .passivate() on a never-touched ActorId returns false -- nothing to passivate.
//   (d) Two racing .passivate() calls from different threads are idempotent -- retire exactly once,
//       no corruption (the interlock, ADR-028 Phase 8 §2).
//   (e) .passivate() on an already-Dormant actor round-trips cleanly (transiently reactivates,
//       drains the one control descriptor, re-retires).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {
struct Ping {
    int n;
};

// KeepAlive (no IdleTimeout declared) -- the automatic wheel never touches this actor at all; any
// retirement observed here can ONLY have come from an explicit .passivate() call.
struct KeptAlive : Actor<KeptAlive, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<int> handled{0};
    void handle(const Ping& p) noexcept { handled.fetch_add(p.n, std::memory_order_release); }
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_passivate_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(64);
    KeptAlive actor;
    Activation act{&actor, KeptAlive::dispatch_table(), pool.sink()};
    eng.register_activation(actor_id_of<KeptAlive>(1), act,
                            static_cast<std::uint16_t>(priority_band_of<KeptAlive>()),
                            drain_budget_of<KeptAlive>(), /*idle_ticks=*/0);  // KeepAlive: never auto-evicts

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<KeptAlive> ref = router.get<KeptAlive>(1);
    eng.start();

    // ---- (a) tell + passivate() drains queued mail then reaches Dormant ---------------------------
    ref.tell(Ping{1});
    check(ref.passivate(), "passivate() on a live, resolvable actor returns true (accepted)", ok);
    check(wait_until([&] { return actor.handled.load(std::memory_order_acquire) >= 1; },
                     std::chrono::seconds(2)),
          "the message queued BEFORE passivate() still delivers (soft: drains in-flight work first)", ok);
    check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
          "the activation reaches Dormant once genuinely empty", ok);
    check(!act.armed_deactivate_entry(), "no wheel entry was ever armed -- purely on-demand", ok);

    // ---- (b) a message racing the retirement still dispatches (abort-eviction preserved) ----------
    // Reuses the same actor/id (now Dormant): a message posted to a Dormant actor reactivates it (the
    // SAME abort-eviction Dekker sequence proven for the automatic path) -- it does NOT idle back out
    // to Dormant on its own (KeepAlive, no wheel armed), so a second explicit passivate() re-retires it.
    ref.tell(Ping{2});
    check(wait_until([&] { return actor.handled.load(std::memory_order_acquire) >= 3; },
                     std::chrono::seconds(2)),
          "a message posted to a Dormant actor reactivates it and is delivered (no loss)", ok);
    check(ref.passivate(), "a second explicit passivate() re-retires the reactivated actor", ok);
    check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
          "Dormant again after the second passivate()", ok);

    // ---- (c) passivate() on a never-touched ActorId returns false ---------------------------------
    ActorRef<KeptAlive> never_touched = router.get<KeptAlive>(999);
    check(!never_touched.passivate(), "passivate() on an unregistered id returns false (nothing to do)", ok);

    // ---- (d) two racing passivate() calls are idempotent -- retire exactly once, no corruption ----
    ref.tell(Ping{4});  // reactivates (d) fresh: Dormant -> Scheduled -> Running -> drains -> Idle
    check(wait_until([&] { return actor.handled.load(std::memory_order_acquire) >= 7; },
                     std::chrono::seconds(2)),
          "the reactivating message for case (d) delivers", ok);
    {
        std::thread t1([&] { (void)ref.passivate(); });
        std::thread t2([&] { (void)ref.passivate(); });
        t1.join();
        t2.join();
    }
    check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
          "racing passivate() calls still converge on exactly one clean retirement (the interlock)", ok);

    // ---- (e) passivate() on an already-Dormant actor round-trips cleanly --------------------------
    check(ref.passivate(), "passivate() on an already-Dormant actor still returns true", ok);
    check(wait_until([&] { return act.state() == ExecState::Dormant; }, std::chrono::seconds(2)),
          "it transiently reactivates, drains the one control descriptor, and re-retires to Dormant, "
          "with no message loss and no crash",
          ok);
    check(actor.handled.load(std::memory_order_acquire) == 7,
          "no spurious dispatch happened across the Dormant round-trip (handled count unchanged)", ok);

    eng.stop();
    std::printf("engine_passivate_test: %s  (handled=%d)\n", ok ? "OK" : "FAIL", actor.handled.load());
    return ok ? 0 : 1;
}
