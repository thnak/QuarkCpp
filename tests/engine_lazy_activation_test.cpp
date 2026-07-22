// Tests ADR-028 Phase 4 — Engine::activate()/ActivationBroker: LocalRouter's cold path
// (resolve() == nullptr) now routes a declare_lazy'd type's first-touch through the per-shard
// ActivationBroker instead of dead-lettering, constructing the actor + wiring resources exactly
// once, then delivering the ORIGINAL message with no loss. Proves: exactly-one-construction,
// resolve() null->non-null, interop with the existing Phase 1/2 idle-eviction/reactivation path
// (no broker involvement needed after the first construction), resource wiring via a stored
// ResourceScope, and that a genuinely never-`declare_lazy`'d type still dead-letters synchronously
// (the compatibility regression this phase must not break).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/resource.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {
    int n = 1;
};

// A plain lazily-activated actor with no resources — the common case.
struct LazyWorker : Actor<LazyWorker, Sequential> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<int> constructions{0};
    static inline std::atomic<int> handled{0};
    LazyWorker() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Ping& p) noexcept { handled.fetch_add(p.n, std::memory_order_release); }
};

// Lazily-activated AND IdleTimeout'd — proves a broker-constructed actor fully interoperates with
// the existing Phase 1/2 idle-eviction/reactivation machinery with ZERO further broker involvement
// (Engine::activate() only ever fires once per ActorId, on its true first touch).
struct LazySleepy : Actor<LazySleepy, Sequential, IdleTimeout<20>> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<int> handled{0};
    void handle(const Ping& p) noexcept { handled.fetch_add(p.n, std::memory_order_release); }
};

// Lazily-activated WITH a Cached<> resource — proves declare_lazy<A>(&scope)'s stored ResourceScope
// is actually used by the broker's real first construction (004 §Rules; ADR-021 no re-resolution).
struct DbConn {
    int fd = 42;
};
struct NeedsDbLazy : Actor<NeedsDbLazy, Sequential> {
    using protocol = Protocol<Ping>;
    Cached<DbConn> db_;
    static inline std::atomic<int> seen_fd{-1};
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, db_); }
    void handle(const Ping&) noexcept {
        seen_fd.store(db_.resolved() ? db_->fd : -2, std::memory_order_release);
    }
};

// A message type whose destructor is counted, delivered to an actor type that is NEVER
// declare_lazy'd (nor spawned) — the pre-Phase-4 synchronous dead-letter path this phase must not
// regress: Engine::activate() checks type_registry_ synchronously and returns false immediately for
// an unknown type, so the caller's ORIGINAL dead-letter (immediate pool_->reclaim) still fires.
struct CountedMsg {
    static inline std::atomic<int> destructed{0};
    ~CountedMsg() { destructed.fetch_add(1, std::memory_order_release); }
};
struct NeverDeclared : Actor<NeverDeclared, Sequential> {
    using protocol = Protocol<CountedMsg>;
    void handle(const CountedMsg&) noexcept {}
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

    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).idle_tick_ms(1).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_lazy_activation_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(256);
    // Mirror `spawn<A>(key, pool.sink())`'s existing convention: the pool that builds a message's
    // cell must also be the actor's wired reclaim sink (the default `ReclaimSink{}` only resets the
    // descriptor in place, it does not return the cell to `pool`'s free list).
    check(eng.declare_lazy<LazyWorker>(nullptr, pool.sink()).has_value(), "declare_lazy<LazyWorker>", ok);
    check(eng.declare_lazy<LazySleepy>(nullptr, pool.sink()).has_value(), "declare_lazy<LazySleepy>", ok);

    DbConn conn;
    ResourceScope scope;
    scope.provide(conn, ResourceLifetime::Node);
    check(eng.declare_lazy<NeedsDbLazy>(&scope, pool.sink()).has_value(),
          "declare_lazy<NeedsDbLazy>(&scope, reclaim)", ok);

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    // ---- 1) never-`declare_lazy`'d type: the pre-Phase-4 synchronous dead-letter is unchanged ----
    {
        // A NAMED local (not a temporary bound to tell()'s forwarding-ref argument): its own
        // destructor only fires at THIS block's closing brace, so the check below observes exactly
        // the reclaim-path destruction — isolated from the argument-temporary's own lifetime.
        CountedMsg m;
        router.tell<NeverDeclared>(actor_id_of<NeverDeclared>(1), std::move(m));
        check(CountedMsg::destructed.load(std::memory_order_acquire) == 1,
              "unregistered type dead-letters SYNCHRONOUSLY (no broker round-trip)", ok);
        check(eng.resolve(actor_id_of<NeverDeclared>(1)) == nullptr,
              "an unregistered type never resolves", ok);
    }

    // ---- 2) a never-touched declare_lazy'd id: exactly one construction, resolve() null->non-null -
    {
        const ActorId id = actor_id_of<LazyWorker>(1);
        check(eng.resolve(id) == nullptr, "cold id: resolve() is nullptr before first touch", ok);
        router.tell<LazyWorker>(id, Ping{1});
        check(wait_until([&] { return LazyWorker::constructions.load() >= 1; }, std::chrono::seconds(2)),
              "the broker constructs the actor on first touch", ok);
        check(LazyWorker::constructions.load() == 1, "exactly one construction", ok);
        check(wait_until([&] { return eng.resolve(id) != nullptr; }, std::chrono::seconds(2)),
              "resolve() transitions null -> non-null after the broker publishes", ok);

        // A second, distinct message to the SAME (now-live) id delivers through the ordinary path —
        // no second construction, no lost message (1 + 2 == 3).
        router.tell<LazyWorker>(id, Ping{2});
        check(wait_until([&] { return LazyWorker::handled.load(std::memory_order_acquire) >= 3; },
                         std::chrono::seconds(1)),
              "the second message delivers through the ordinary (non-broker) path, no loss", ok);
        check(LazyWorker::constructions.load() == 1, "still exactly one construction", ok);
    }

    // ---- 3) resource wiring: the broker's first construction actually wires Cached<DbConn> --------
    {
        const ActorId id = actor_id_of<NeedsDbLazy>(1);
        router.tell<NeedsDbLazy>(id, Ping{});
        check(wait_until([&] { return NeedsDbLazy::seen_fd.load(std::memory_order_acquire) == 42; },
                         std::chrono::seconds(2)),
              "declare_lazy<A>(&scope): the lazily-constructed instance is wired (Cached<DbConn> "
              "resolves to the SAME conn, fd == 42) before its first handler runs",
              ok);
    }

    // ---- 4) interop with existing Phase 1/2 idle-eviction: no broker involvement after first touch,
    //         and eviction/reactivation still delivers every message with no loss. -------------------
    {
        const ActorId id = actor_id_of<LazySleepy>(1);
        router.tell<LazySleepy>(id, Ping{1});
        check(wait_until([&] { return LazySleepy::handled.load(std::memory_order_acquire) >= 1; },
                         std::chrono::seconds(2)),
              "the broker-constructed actor handles its first message", ok);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // well past the 20-tick timeout
        // If eviction is broken (or reactivation is broken/lossy), the sum below never reaches 3.
        router.tell<LazySleepy>(id, Ping{2});
        check(wait_until([&] { return LazySleepy::handled.load(std::memory_order_acquire) >= 3; },
                         std::chrono::seconds(2)),
              "a message posted after the idle-timeout reactivates the actor with no loss (1+2=3)", ok);
    }

    eng.stop();
    std::printf("engine_lazy_activation_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
