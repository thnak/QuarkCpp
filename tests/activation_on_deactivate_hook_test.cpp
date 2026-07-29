// Tests ADR-028 Phase 8 §on_deactivate() lifecycle hook — a REAL, running Engine, using the
// spawn<A>() path (so the hook is wired exactly the way a real caller would get it, via
// make_deactivate_hook_sink<A>() inside Engine::spawn<A>()), mirroring
// tests/engine_idle_timeout_eviction_test.cpp's real-clock style.
//
// Proves the hook fires from the ONE call site (close_out_retire()) identically whether triggered
// by the automatic idle-timeout wheel or the on-demand passivate() API — no divergent code path.
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

// 20ms IdleTimeout (same shape as engine_idle_timeout_eviction_test.cpp's Sleepy) plus an
// on_deactivate() hook that increments a shared counter every time it fires.
struct HookedSleepy : Actor<HookedSleepy, Sequential, IdleTimeout<20>> {
    using protocol = Protocol<Ping>;
    std::atomic<int> handled{0};
    static inline std::atomic<int> deactivate_calls{0};
    void handle(const Ping& p) noexcept { handled.fetch_add(p.n, std::memory_order_release); }
    void on_deactivate() noexcept { deactivate_calls.fetch_add(1, std::memory_order_release); }
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

    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).idle_tick_ms(1).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("activation_on_deactivate_hook_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(64);
    result<ActorId> spawned = eng.spawn<HookedSleepy>(1, pool.sink());
    check(spawned.has_value(), "spawn<HookedSleepy>(1) succeeds", ok);
    if (!spawned) {
        std::printf("activation_on_deactivate_hook_test: FAIL (spawn failed)\n");
        return 1;
    }
    const ActorId id = *spawned;

    LocalRouter router(eng.post_courier(), pool);
    ActorRef<HookedSleepy> ref{id, &router};
    eng.start();

    Schedulable* s = eng.resolve(id);
    check(s != nullptr, "resolve(id) finds the freshly-spawned activation", ok);
    Activation* act = s->activation;

    // ---- fires once via the automatic idle-timeout wheel -------------------------------------------
    ref.tell(Ping{1});
    check(wait_until([&] { return act->went_dormant(); }, std::chrono::seconds(2)),
          "idle-timeout eviction fires (011/ADR-028 Phase 2)", ok);
    check(wait_until([&] { return HookedSleepy::deactivate_calls.load(std::memory_order_acquire) >= 1; },
                     std::chrono::seconds(2)),
          "on_deactivate() fired exactly once via the AUTOMATIC wheel trigger", ok);
    check(HookedSleepy::deactivate_calls.load() == 1, "still exactly one call so far", ok);

    // ---- fires again via the on-demand passivate() API, after reactivation ------------------------
    ref.tell(Ping{2});  // reactivates: Dormant -> Scheduled -> Running -> drains
    check(wait_until([&] { return !act->went_dormant(); }, std::chrono::seconds(2)),
          "reactivated by the message (no longer Dormant)", ok);
    check(ref.passivate(), "explicit passivate() accepted on the now-live actor", ok);
    check(wait_until([&] { return act->went_dormant(); }, std::chrono::seconds(2)),
          "passivate() drives it back to Dormant", ok);
    check(wait_until([&] { return HookedSleepy::deactivate_calls.load(std::memory_order_acquire) >= 2; },
                     std::chrono::seconds(2)),
          "on_deactivate() fired a SECOND time via the ON-DEMAND trigger -- the SAME call site serves "
          "both triggers identically",
          ok);
    check(HookedSleepy::deactivate_calls.load() == 2, "exactly two calls total, one per retirement", ok);

    eng.stop();
    std::printf("activation_on_deactivate_hook_test: %s  (deactivate_calls=%d)\n", ok ? "OK" : "FAIL",
                HookedSleepy::deactivate_calls.load());
    return ok ? 0 : 1;
}
