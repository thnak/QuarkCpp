// Tests ADR-028 Phase 8 §Persistence-flush wiring — the deactivation-time flush reuses the existing,
// proven save_snapshot()/Store seam, wired only for declare_lazy<A>(store,...)-registered actors
// (mirrors tests/engine_lazy_persistence_test.cpp's style).
//
// Proves:
//   1. .passivate() on a declare_lazy<A>(store,...) actor persists its latest snapshot_state() before
//      retiring, under a REAL fence acquired once at construction (not a stale FenceToken{}).
//   2. A store whose save_snapshot() always fails: retirement still commits to Dormant (best-effort,
//      never fatal) and the failure is counted via deactivate_flush_failures.
//   3. A spawn<A>()'d (no-store) Persistent<Snapshot> actor: .passivate() still works, with the flush
//      inertly skipped (documented limitation, not a regression).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Bump {};

struct CounterState {
    std::uint64_t n = 0;
};
QUARK_SERIALIZE(CounterState, (1, n))

struct PersistedCounter : Actor<PersistedCounter, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<Bump>;
    using PersistState = CounterState;

    std::uint64_t n = 0;
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{n}; }
    void restore_state(CounterState st) { n = st.n; }
    void handle(const Bump&) noexcept { ++n; }
};

// A store whose save_snapshot always fails -- proves the best-effort, never-fatal flush contract.
struct FailingSaveStore {
    [[nodiscard]] FenceToken acquire_fence(ActorId id) { return inner.acquire_fence(id); }
    [[nodiscard]] SeqNo last_seq(ActorId id) const noexcept { return inner.last_seq(id); }
    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const {
        return inner.load_snapshot(id);
    }
    [[nodiscard]] result<void> save_snapshot(ActorId, FenceToken, const SnapshotRecord&) {
        return fail(errc::internal, "FailingSaveStore: deliberately broken save_snapshot");
    }
    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        return inner.append(id, tok, seq, bytes);
    }
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok, std::span<const EventRecord> b) {
        return inner.append_batch(id, tok, b);
    }
    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) const {
        return inner.read_log(id, from);
    }
    InMemoryStore inner;
};
static_assert(Store<FailingSaveStore>, "FailingSaveStore must model the 012 Store seam");

// A Persistent<Snapshot> actor spawned WITHOUT a store (spawn<A>(), not declare_lazy<A>(store,...))
// -- proves the flush is simply inactive, never a crash, for the documented eager-spawn limitation.
struct SpawnedPersistent : Actor<SpawnedPersistent, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<Bump>;
    using PersistState = CounterState;
    std::uint64_t n = 0;
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{n}; }
    void restore_state(CounterState st) { n = st.n; }
    void handle(const Bump&) noexcept { ++n; }
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

    // ---- 1) declare_lazy<A>(store,...): passivate() persists state under a REAL fence -------------
    {
        auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
        check(built.has_value(), "ConfigBuilder #1", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(64);
        InMemoryStore store;

        check(eng.declare_lazy<PersistedCounter>(store).has_value(),
              "declare_lazy<PersistedCounter>(store)", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        const ActorId id = actor_id_of<PersistedCounter>(1);
        router.tell<PersistedCounter>(id, Bump{});
        router.tell<PersistedCounter>(id, Bump{});
        router.tell<PersistedCounter>(id, Bump{});

        check(wait_until([&] { return eng.resolve(id) != nullptr; }, std::chrono::seconds(2)),
              "the broker constructs the activation on first touch", ok);
        ActorRef<PersistedCounter> ref{id, &router};
        check(ref.passivate(), "passivate() accepted", ok);

        Schedulable* s = eng.resolve(id);
        check(wait_until([&] { return s->activation->went_dormant(); }, std::chrono::seconds(2)),
              "retires to Dormant", ok);

        check(store.current_owner(id) != FenceToken{0},
              "a REAL fence was acquired at construction (not the stale default FenceToken{})", ok);

        result<std::optional<SnapshotRecord>> loaded = store.load_snapshot(id);
        check(loaded.has_value() && loaded->has_value(),
              "a snapshot was actually saved by the deactivation-time flush", ok);
        if (loaded && loaded->has_value()) {
            result<CounterState> decoded = decode_durable<CounterState>((*loaded)->record);
            check(decoded.has_value() && decoded->n == 3,
                  "the persisted snapshot reflects the latest state (n=3), not stale/zero", ok);
        }
        eng.stop();
    }

    // ---- 2) a failing store: retirement still commits, failure is counted -------------------------
    {
        auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
        check(built.has_value(), "ConfigBuilder #2", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(64);
        FailingSaveStore fstore;

        check(eng.declare_lazy<PersistedCounter>(fstore).has_value(),
              "declare_lazy<PersistedCounter>(fstore)", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        const ActorId id = actor_id_of<PersistedCounter>(2);
        router.tell<PersistedCounter>(id, Bump{});
        check(wait_until([&] { return eng.resolve(id) != nullptr; }, std::chrono::seconds(2)),
              "the broker constructs the activation", ok);
        ActorRef<PersistedCounter> ref{id, &router};
        check(ref.passivate(), "passivate() accepted even though the store will fail the flush", ok);

        Schedulable* s = eng.resolve(id);
        check(wait_until([&] { return s->activation->went_dormant(); }, std::chrono::seconds(2)),
              "retirement STILL commits to Dormant despite the flush failure (best-effort, never fatal)",
              ok);
        check(wait_until(
                  [&] { return eng.metrics_snapshot().deactivate_flush_failures >= 1; },
                  std::chrono::seconds(2)),
              "the flush failure is counted via deactivate_flush_failures", ok);
        eng.stop();
    }

    // ---- 3) spawn<A>() (no store): passivate() works, flush inertly skipped -----------------------
    {
        auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
        check(built.has_value(), "ConfigBuilder #3", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(64);
        result<ActorId> spawned = eng.spawn<SpawnedPersistent>(1, pool.sink());
        check(spawned.has_value(), "spawn<SpawnedPersistent>(1) (no store) succeeds", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        if (spawned) {
            const ActorId id = *spawned;
            ActorRef<SpawnedPersistent> ref{id, &router};
            ref.tell(Bump{});
            check(wait_until([&] { return eng.resolve(id) != nullptr; }, std::chrono::seconds(2)),
                  "resolves", ok);
            check(ref.passivate(), "passivate() accepted on a store-less Persistent<Snapshot> actor", ok);

            Schedulable* s = eng.resolve(id);
            check(wait_until([&] { return s->activation->went_dormant(); }, std::chrono::seconds(2)),
                  "retires to Dormant cleanly -- no crash, flush was simply never wired (documented "
                  "limitation for eagerly-spawn<A>()'d actors)",
                  ok);
        }
        eng.stop();
    }

    std::printf("engine_passivate_persistence_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
