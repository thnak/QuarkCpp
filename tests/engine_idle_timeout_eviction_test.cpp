// Tests 011-Timers-and-Scheduled-Work §Idle-timeout deactivation / ADR-028 Phase 2 — the per-shard
// TimingWheel actually wired into a REAL, running Engine: an `IdleTimeout<Ms>` actor with no traffic
// is evicted to Dormant, and a message posted after eviction reactivates it with no message loss.
//
// Unlike the other ADR-028 tests (which hand-drive a bare Activation/ExecStateCell deterministically),
// this exercises the full real-clock, real-thread path: Engine::spawn<A>'s ms->ticks conversion,
// Engine::register_activation's idle_ticks wiring, the per-shard Shard::wheel/wheel_pool,
// Engine::arm_deactivate/advance_wheel/on_deactivate_fire, AND the engine-wide backstop thread (011
// §"engine-wide backstop" — without it, a fully-idle shard's worker parks and the wheel never ticks).
// A small `idle_tick_ms` (via ConfigBuilder) keeps the test fast; real-time waits are spin-with-
// timeout, never a fixed sleep, so this is not flaky under CI scheduling jitter.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {
struct Ping {
    int n;
};

// 20ms IdleTimeout — with a 1ms wheel tick (set below) this is 20 ticks, evicting promptly without
// racing the test's own message delivery.
struct Sleepy : Actor<Sleepy, Sequential, IdleTimeout<20>> {
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

// Bounded spin-wait on a predicate, real wall-clock timeout — never a fixed sleep (flake-proof: it
// returns as soon as the predicate is true, and only fails if it's ACTUALLY stuck past the deadline).
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

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).idle_tick_ms(1).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_idle_timeout_eviction_test: FAIL (config build failed)\n");
        return 1;
    }

    const EngineConfig cfg = *built;
    detail::MessagePool pool(64);
    Sleepy actor;
    Activation act{&actor, Sleepy::dispatch_table(), pool.sink()};

    Engine<> eng(cfg);
    // Mirrors Engine::spawn<A>()'s own ms->ticks conversion exactly (engine.hpp) — using
    // register_activation directly here (like tests/tell_fifo_test.cpp) so this test keeps a typed
    // reference to both the actor and its Activation for direct observation (`went_dormant()`,
    // `state()`), which spawn<A>()'s type-erased ownership doesn't expose.
    constexpr std::uint64_t idle_ms = idle_timeout_ms_of<Sleepy>();
    const std::uint32_t idle_ticks =
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1, idle_ms / cfg.idle_tick_ms));
    eng.register_activation(actor_id_of<Sleepy>(1), act,
                            static_cast<std::uint16_t>(priority_band_of<Sleepy>()),
                            drain_budget_of<Sleepy>(), idle_ticks);

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Sleepy> ref = router.get<Sleepy>(1);

    eng.start();

    // ---- Phase A: a message delivers normally, then the actor idles out to Dormant ---------------
    ref.tell(Ping{1});
    check(wait_until([&] { return actor.handled.load(std::memory_order_acquire) >= 1; },
                     std::chrono::seconds(2)),
          "first message delivered", ok);

    check(wait_until([&] { return act.went_dormant(); }, std::chrono::seconds(2)),
          "idle-timeout eviction fires within 2s of the actor going idle (011/ADR-028 Phase 2)", ok);
    check(act.state() == ExecState::Dormant, "activation is Dormant after eviction", ok);

    // ---- Phase B: reactivation — a message posted to the now-Dormant actor is NOT lost ------------
    ref.tell(Ping{2});
    check(wait_until([&] { return actor.handled.load(std::memory_order_acquire) >= 3; },
                     std::chrono::seconds(2)),
          "the post-eviction message reactivates the actor and is delivered (no loss)", ok);
    check(!act.went_dormant(), "no longer Dormant once the reactivating message is drained", ok);

    eng.stop();
    std::printf("engine_idle_timeout_eviction_test: %s  (handled=%d)\n", ok ? "OK" : "FAIL",
                actor.handled.load());
    return ok ? 0 : 1;
}
