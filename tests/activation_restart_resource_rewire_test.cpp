// Tests the "007 Restart resource re-wire" fix (ADR-028 roadmap "Phase 6", redirected this session):
// `make_reconstruct_sink<A>()`'s placement-new (the OnFailure<Restart> supervision path, 007) leaves
// a fresh actor instance's Cached<>/PerMessage<> members default-constructed (null/unwired) unless
// something re-runs `wire()`. `Activation::reconstruct_now()` now does exactly that, reusing the SAME
// `wire()`/`ResourceScope*` the actor was originally wired with (`set_resource_wire`, wired by both
// `Engine::spawn<A>()` and the ADR-028 Phase 4 broker's `handle_wake`). This is NOT a new resolution —
// `ResourceScope::resolve<T>()` is a scan over an already-immutable table (004/ADR-021) — so the proof
// here is pointer-IDENTITY across a restart, not just non-null.
//
// Three scenarios:
//   1. Direct-Activation unit test (no Engine, mirrors supervision_decision_test.cpp's style): proves
//      the core Activation::reconstruct_now() mechanism in isolation.
//   2. Engine::spawn<A>(key, reclaim, &scope): proves the eager-spawn integration point.
//   3. Engine::declare_lazy<A>(&scope): proves the ADR-028 Phase 4 lazy/broker integration point.
// A 4th, implicit regression pin: every existing supervision test (supervision_decision_test.cpp etc.,
// none of which declare a wire()) must stay green, unaffected (wire_/wire_scope_ both null ⇒ no-op).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/resource.hpp"
#include "quark/core/supervision.hpp"
#include "quark/detail/message_pool.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ---- Scenario 1: direct Activation, no Engine ------------------------------------------------
struct Bump {};
struct Boom {};
struct Config1 {
    int value = 0;
};

struct DirectActor : Actor<DirectActor, Sequential> {
    using protocol = Protocol<Bump, Boom>;
    int n = 0;
    Cached<Config1> cfg_;
    void handle(const Bump&) noexcept { ++n; }
    void handle(const Boom&) { throw std::runtime_error("boom"); }
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, cfg_); }
};

// A tiny message source mirroring supervision_decision_test.cpp's Feeder.
struct Feeder {
    std::vector<Descriptor> descs;
    std::vector<Bump> bumps;
    std::vector<Boom> booms;
    explicit Feeder(std::size_t cap) : descs(cap), bumps(cap), booms(cap) {}
    void bump(Activation& act, std::size_t i) {
        descs[i].payload = &bumps[i];
        stamp<DirectActor, Bump>(descs[i]);
        act.post(&descs[i]);
    }
    void boom(Activation& act, std::size_t i) {
        descs[i].payload = &booms[i];
        stamp<DirectActor, Boom>(descs[i]);
        act.post(&descs[i]);
    }
};

// ---- Scenario 2: Engine::spawn<A>(key, reclaim, &scope) --------------------------------------
struct Config2 {
    int value = 0;
};
struct SpawnedRestart : Actor<SpawnedRestart, Sequential, OnFailure<Restart>> {
    using protocol = Protocol<Bump, Boom>;
    static inline std::atomic<int> constructions{0};
    static inline std::atomic<bool> last_resolved{false};
    static inline std::atomic<Config2*> last_ptr{nullptr};
    int n = 0;
    Cached<Config2> cfg_;
    SpawnedRestart() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Bump&) noexcept {
        ++n;
        last_resolved.store(cfg_.resolved(), std::memory_order_release);
        last_ptr.store(cfg_.resolved() ? &cfg_.get() : nullptr, std::memory_order_release);
    }
    void handle(const Boom&) { throw std::runtime_error("boom"); }
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, cfg_); }
};

// ---- Scenario 3: Engine::declare_lazy<A>(&scope) (ADR-028 Phase 4 broker path) -----------------
struct Config3 {
    int value = 0;
};
struct LazyRestart : Actor<LazyRestart, Sequential, OnFailure<Restart>> {
    using protocol = Protocol<Bump, Boom>;
    static inline std::atomic<int> constructions{0};
    static inline std::atomic<bool> last_resolved{false};
    static inline std::atomic<Config3*> last_ptr{nullptr};
    int n = 0;
    Cached<Config3> cfg_;
    LazyRestart() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Bump&) noexcept {
        ++n;
        last_resolved.store(cfg_.resolved(), std::memory_order_release);
        last_ptr.store(cfg_.resolved() ? &cfg_.get() : nullptr, std::memory_order_release);
    }
    void handle(const Boom&) { throw std::runtime_error("boom"); }
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, cfg_); }
};

}  // namespace

int main() {
    bool ok = true;

    // ==== 1) direct Activation: the core reconstruct_now() re-wire mechanism =====================
    {
        Config1 cfg{7};
        ResourceScope scope;
        scope.provide(cfg, ResourceLifetime::Activation);

        DirectActor a;
        check(a.wire(scope).has_value(), "S1: initial wire (mirrors spawn<A>'s one-time wire)", ok);
        check(a.cfg_.resolved() && &a.cfg_.get() == &cfg, "S1: initial wire resolves to cfg", ok);

        Activation act{&a, DirectActor::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Restart}};
        act.set_reconstruct(make_reconstruct_sink<DirectActor>());
        act.set_resource_wire(make_wire_fn<DirectActor>(), &scope);

        Feeder f(4);
        f.bump(act, 0);
        f.bump(act, 1);
        f.boom(act, 2);  // throws -> Restart -> reconstruct_now(): placement-new + re-wire
        f.bump(act, 3);
        drive(act);

        check(a.n == 1, "S1: fresh state after restart (reset then one bump) == 1", ok);
        check(act.restarts_total() == 1, "S1: exactly one restart", ok);
        check(a.cfg_.resolved(), "S1: Cached<Config1> is resolved again after restart (not null)", ok);
        check(&a.cfg_.get() == &cfg,
              "S1: re-wire resolves to the SAME instance (no re-resolution, ADR-021)", ok);
    }

    // ==== 2) Engine::spawn<A>(key, reclaim, &scope) ==============================================
    {
        auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
        check(built.has_value(), "S2: ConfigBuilder produces a valid EngineConfig", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(256);

        Config2 cfg{11};
        ResourceScope scope;
        scope.provide(cfg, ResourceLifetime::Activation);

        auto id = eng.spawn<SpawnedRestart>(1, pool.sink(), &scope);
        check(id.has_value(), "S2: spawn<SpawnedRestart>(key, reclaim, &scope)", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        router.tell<SpawnedRestart>(*id, Bump{});
        check(wait_until([&] { return SpawnedRestart::last_resolved.load(std::memory_order_acquire); },
                         std::chrono::seconds(2)),
              "S2: initial wire resolves before restart", ok);
        router.tell<SpawnedRestart>(*id, Boom{});  // throws -> Restart -> reconstruct_now() re-wires
        router.tell<SpawnedRestart>(*id, Bump{});
        check(wait_until(
                  [&] {
                      return SpawnedRestart::constructions.load(std::memory_order_acquire) >= 2 &&
                             SpawnedRestart::last_ptr.load(std::memory_order_acquire) == &cfg;
                  },
                  std::chrono::seconds(2)),
              "S2: post-restart handle() observes Cached<Config2> resolved to the SAME instance", ok);
        eng.stop();
    }

    // ==== 3) Engine::declare_lazy<A>(&scope) (ADR-028 Phase 4 broker construction) =================
    {
        auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
        check(built.has_value(), "S3: ConfigBuilder produces a valid EngineConfig", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(256);

        Config3 cfg{13};
        ResourceScope scope;
        scope.provide(cfg, ResourceLifetime::Activation);
        check(eng.declare_lazy<LazyRestart>(&scope, pool.sink()).has_value(),
              "S3: declare_lazy<LazyRestart>(&scope, reclaim)", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        const ActorId id = actor_id_of<LazyRestart>(1);
        router.tell<LazyRestart>(id, Bump{});  // first touch -> broker constructs + wires
        check(wait_until([&] { return LazyRestart::last_resolved.load(std::memory_order_acquire); },
                         std::chrono::seconds(2)),
              "S3: the broker's first construction wires Cached<Config3>", ok);
        router.tell<LazyRestart>(id, Boom{});  // throws -> Restart -> reconstruct_now() re-wires
        router.tell<LazyRestart>(id, Bump{});
        check(wait_until(
                  [&] {
                      return LazyRestart::constructions.load(std::memory_order_acquire) >= 2 &&
                             LazyRestart::last_ptr.load(std::memory_order_acquire) == &cfg;
                  },
                  std::chrono::seconds(2)),
              "S3: post-restart handle() observes Cached<Config3> resolved to the SAME instance "
              "(lazy/broker path also re-wired)",
              ok);
        eng.stop();
    }

    std::printf("activation_restart_resource_rewire_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
