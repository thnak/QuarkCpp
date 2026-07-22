// Tests ADR-028 Phase 5 — declare_lazy<A>(store, ...): the broker's one-time lazy construction
// (Phase 4 handle_wake) generically recovers a Persistent<Snapshot> actor's durable state via
// recover_snapshot() before its first message dispatches. Proves: (1) a pre-seeded snapshot is
// actually recovered (not the actor's fresh-default state), (2) no prior snapshot ⇒ seeds from the
// actor's own snapshot_state() default (matches recover_snapshot's existing "seed with initial"
// contract), (3) a failing store dead-letters the original message through the SAME
// construct/destroy/reclaim shape Phase 4 already proved for a wire failure — never a
// half-constructed actor, never a lost message.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {};

// --- A Persistent<Snapshot> actor with the ADR-028 Phase 5 PersistState contract ----------------
struct CounterState {
    std::uint64_t n = 0;
};
QUARK_SERIALIZE(CounterState, (1, n))

struct PersistedCounter : Actor<PersistedCounter, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<Ping>;
    using PersistState = CounterState;

    std::uint64_t n = 0;
    static inline std::atomic<int> constructions{0};
    static inline std::atomic<std::uint64_t> seen_on_first_handle{999999};  // sentinel: never observed

    PersistedCounter() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{n}; }
    void restore_state(CounterState st) { n = st.n; }

    void handle(const Ping&) noexcept {
        // Only the FIRST handle on a fresh process run matters for the recovery assertion below —
        // each scenario below uses its own ActorId, so this always observes the just-recovered value.
        seen_on_first_handle.store(n, std::memory_order_release);
        ++n;
    }
};

// A store whose load_snapshot always fails — proves the recovery-failure dead-letter path.
struct FailingStore {
    [[nodiscard]] FenceToken acquire_fence(ActorId) { return FenceToken{1}; }
    [[nodiscard]] SeqNo last_seq(ActorId) const noexcept { return 0; }
    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId) const {
        return fail(errc::internal, "FailingStore: deliberately broken load_snapshot");
    }
    [[nodiscard]] result<void> save_snapshot(ActorId, FenceToken, const SnapshotRecord&) { return {}; }
    [[nodiscard]] result<void> append(ActorId, FenceToken, SeqNo, std::span<const std::byte>) {
        return {};
    }
    [[nodiscard]] result<void> append_batch(ActorId, FenceToken, std::span<const EventRecord>) {
        return {};
    }
    [[nodiscard]] result<EventCursor> read_log(ActorId, SeqNo) const { return EventCursor{}; }
};
static_assert(Store<FailingStore>, "FailingStore must model the 012 Store seam");

// The pre-Phase-4 synchronous dead-letter regression pin (mirrors engine_lazy_activation_test.cpp):
// a message type whose destructor is counted, delivered when construction fails.
struct CountedMsg {
    static inline std::atomic<int> destructed{0};
    ~CountedMsg() { destructed.fetch_add(1, std::memory_order_release); }
};
struct NeedsFailingStore : Actor<NeedsFailingStore, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<CountedMsg>;
    using PersistState = CounterState;
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{}; }
    void restore_state(CounterState) {}
    void handle(const CountedMsg&) noexcept {}
};

// --- S3 concurrency pin: N threads race the SAME cold Persistent<Snapshot> id ------------------
struct RacePersisted : Actor<RacePersisted, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<Ping>;
    using PersistState = CounterState;
    static inline std::atomic<int> recover_calls{0};
    static inline std::atomic<long long> handled{0};
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{}; }
    void restore_state(CounterState) { recover_calls.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Ping&) noexcept { handled.fetch_add(1, std::memory_order_release); }
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

    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_lazy_persistence_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(256);
    InMemoryStore store;

    // ---- 1) a pre-seeded snapshot IS recovered before the first handle() runs -------------------
    const ActorId seeded_id = actor_id_of<PersistedCounter>(1);
    {
        const FenceToken f = store.acquire_fence(seeded_id);
        check(save_snapshot<CounterState>(store, seeded_id, f, /*through_seq=*/0, CounterState{42})
                  .has_value(),
              "pre-seed a snapshot (n=42)", ok);
    }
    check(eng.declare_lazy<PersistedCounter>(store).has_value(),
          "declare_lazy<PersistedCounter>(store)", ok);

    // ---- 2) no prior snapshot ⇒ seeds from the actor's own snapshot_state() default (n=0) --------
    const ActorId cold_id = actor_id_of<PersistedCounter>(2);

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    router.tell<PersistedCounter>(seeded_id, Ping{});
    check(wait_until([&] { return PersistedCounter::constructions.load() >= 1; }, std::chrono::seconds(2)),
          "the broker constructs the actor on first touch", ok);
    check(wait_until(
              [&] { return PersistedCounter::seen_on_first_handle.load(std::memory_order_acquire) == 42; },
              std::chrono::seconds(2)),
          "the pre-seeded snapshot (n=42) was recovered BEFORE the first handle() ran", ok);

    router.tell<PersistedCounter>(cold_id, Ping{});
    check(wait_until([&] { return PersistedCounter::constructions.load() >= 2; }, std::chrono::seconds(2)),
          "a second, distinct id also lazily constructs", ok);
    check(wait_until(
              [&] { return PersistedCounter::seen_on_first_handle.load(std::memory_order_acquire) == 0; },
              std::chrono::seconds(2)),
          "no prior snapshot for this id ⇒ seeded from snapshot_state()'s own default (n=0)", ok);

    // ---- 3) a failing store dead-letters the original message via the broker ----------------------
    // (unlike a never-`declare_lazy`'d type's IMMEDIATE synchronous dead-letter, this type WAS
    // declared — activate() hands off to the broker, so the dead-letter happens asynchronously once
    // the broker's handle_wake actually runs; wait for it like every other broker-path assertion.)
    {
        FailingStore fstore;
        auto built2 = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
        check(built2.has_value(), "second ConfigBuilder", ok);
        Engine<> eng2(*built2);
        detail::MessagePool pool2(64);
        check(eng2.declare_lazy<NeedsFailingStore>(fstore).has_value(),
              "declare_lazy<NeedsFailingStore>(fstore)", ok);
        eng2.start();
        LocalRouter router2(eng2.post_courier(), pool2);

        CountedMsg m;
        router2.tell<NeedsFailingStore>(actor_id_of<NeedsFailingStore>(1), std::move(m));
        check(wait_until([&] { return CountedMsg::destructed.load(std::memory_order_acquire) == 1; },
                         std::chrono::seconds(2)),
              "a recovery failure dead-letters the original message (construct+destroy via the "
              "broker, no half-built actor left reachable)",
              ok);
        check(eng2.resolve(actor_id_of<NeedsFailingStore>(1)) == nullptr,
              "a failed recovery never publishes the id-table entry", ok);
        eng2.stop();
    }

    // ---- 4) S3 concurrency pin: N threads racing the SAME cold Persistent<Snapshot> id -----------
    // recover runs inside the already-Sequential, already-proven broker step (Phase 4 S1/bug-(a)) —
    // this is a regression pin, not a new concurrency mechanism.
    {
        InMemoryStore rstore;
        auto built3 = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
        check(built3.has_value(), "third ConfigBuilder", ok);
        Engine<> eng3(*built3);
        detail::MessagePool pool3(4096);
        check(eng3.declare_lazy<RacePersisted>(rstore).has_value(),
              "declare_lazy<RacePersisted>(rstore)", ok);
        eng3.start();
        LocalRouter router3(eng3.post_courier(), pool3);

        constexpr int kThreads = 8;
        constexpr int kPerThread = 500;
        const ActorId race_id = actor_id_of<RacePersisted>(1);
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) router3.tell<RacePersisted>(race_id, Ping{});
            });
        }
        for (auto& th : threads) th.join();

        constexpr long long expected = static_cast<long long>(kThreads) * kPerThread;
        check(wait_until([&] { return RacePersisted::handled.load(std::memory_order_acquire) == expected; },
                         std::chrono::seconds(5)),
              "every racing message is delivered exactly once", ok);
        check(RacePersisted::recover_calls.load(std::memory_order_relaxed) == 1,
              "S3: recover() runs exactly once across all racing first-touches", ok);
        eng3.stop();
    }

    eng.stop();
    std::printf("engine_lazy_persistence_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
