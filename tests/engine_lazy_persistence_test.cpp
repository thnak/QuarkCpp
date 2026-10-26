// Tests ADR-028 Phase 5 — declare_lazy<A>(store, ...): the broker's one-time lazy construction
// (Phase 4 handle_wake) generically recovers a Persistent<Snapshot> actor's durable state via
// recover_snapshot() before its first message dispatches. Proves: (1) a pre-seeded snapshot is
// actually recovered (not the actor's fresh-default state), (2) no prior snapshot ⇒ seeds from the
// actor's own snapshot_state() default (matches recover_snapshot's existing "seed with initial"
// contract), (3) a failing store dead-letters the original message through the SAME
// construct/destroy/reclaim shape Phase 4 already proved for a wire failure — never a
// half-constructed actor, never a lost message.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
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
    [[nodiscard]] result<FenceToken> acquire_fence(ActorId) { return FenceToken{1}; }
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

// Records the ORDER of store calls the broker makes while lazily activating an actor (review B2):
// the fence must be acquired BEFORE recovery reads the store, or a stale owner can write between the
// read and the fence acquisition. Delegates to an InMemoryStore so recovery behaves normally.
struct OrderRecordingStore {
    InMemoryStore inner;
    mutable std::mutex mu;
    mutable std::vector<char> calls;  // 'F' = acquire_fence, 'L' = load_snapshot / read_log
    void note(char c) const {
        std::lock_guard<std::mutex> g(mu);
        calls.push_back(c);
    }
    [[nodiscard]] result<FenceToken> acquire_fence(ActorId id) {
        note('F');
        return inner.acquire_fence(id);
    }
    [[nodiscard]] SeqNo last_seq(ActorId id) const noexcept { return inner.last_seq(id); }
    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const {
        note('L');
        return inner.load_snapshot(id);
    }
    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken t, const SnapshotRecord& r) {
        return inner.save_snapshot(id, t, r);
    }
    [[nodiscard]] result<void> append(ActorId id, FenceToken t, SeqNo q, std::span<const std::byte> b) {
        return inner.append(id, t, q, b);
    }
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken t, std::span<const EventRecord> b) {
        return inner.append_batch(id, t, b);
    }
    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) const {
        note('L');
        return inner.read_log(id, from);
    }
};
static_assert(Store<OrderRecordingStore>, "OrderRecordingStore must model the 012 Store seam");
// A store whose acquire_fence always FAILS (item 2): the broker must dead-letter the first message —
// an activation that cannot be fenced against the previous owner may not run or flush.
struct FenceFailStore {
    InMemoryStore inner;
    std::atomic<int> acquires{0};
    [[nodiscard]] result<FenceToken> acquire_fence(ActorId) {
        acquires.fetch_add(1, std::memory_order_relaxed);
        return fail(errc::internal, "FenceFailStore: deliberately cannot record the new owner");
    }
    [[nodiscard]] SeqNo last_seq(ActorId id) const noexcept { return inner.last_seq(id); }
    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const {
        return inner.load_snapshot(id);
    }
    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken t, const SnapshotRecord& r) {
        return inner.save_snapshot(id, t, r);
    }
    [[nodiscard]] result<void> append(ActorId id, FenceToken t, SeqNo q, std::span<const std::byte> b) {
        return inner.append(id, t, q, b);
    }
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken t, std::span<const EventRecord> b) {
        return inner.append_batch(id, t, b);
    }
    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) const {
        return inner.read_log(id, from);
    }
};
static_assert(Store<FenceFailStore>, "FenceFailStore must model the 012 Store seam");
struct CountedMsg2 {
    static inline std::atomic<int> destructed{0};
    ~CountedMsg2() { destructed.fetch_add(1, std::memory_order_release); }
};
struct NeedsFenceStore : Actor<NeedsFenceStore, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<CountedMsg2>;
    using PersistState = CounterState;
    static inline std::atomic<int> handled{0};
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{}; }
    void restore_state(CounterState) {}
    void handle(const CountedMsg2&) noexcept { handled.fetch_add(1, std::memory_order_relaxed); }
};
struct OrderedPersisted : Actor<OrderedPersisted, Sequential, Persistent<Snapshot>> {
    using protocol = Protocol<Ping>;
    using PersistState = CounterState;
    static inline std::atomic<int> handled{0};
    [[nodiscard]] CounterState snapshot_state() const { return CounterState{}; }
    void restore_state(CounterState) {}
    void handle(const Ping&) noexcept { handled.fetch_add(1, std::memory_order_release); }
};

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
        const FenceToken f = *store.acquire_fence(seeded_id);
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

    // ---- 5) B2: the fence is acquired BEFORE recovery reads the store ----------------------------
    {
        OrderRecordingStore ostore;
        auto built4 = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
        check(built4.has_value(), "fourth ConfigBuilder", ok);
        Engine<> eng4(*built4);
        detail::MessagePool pool4(64);
        check(eng4.declare_lazy<OrderedPersisted>(ostore).has_value(),
              "declare_lazy<OrderedPersisted>(ostore)", ok);
        eng4.start();
        LocalRouter router4(eng4.post_courier(), pool4);
        router4.tell<OrderedPersisted>(actor_id_of<OrderedPersisted>(1), Ping{});
        check(wait_until([&] { return OrderedPersisted::handled.load(std::memory_order_acquire) == 1; },
                         std::chrono::seconds(2)),
              "the ordered-store actor activates and handles its first message", ok);
        {
            std::lock_guard<std::mutex> g(ostore.mu);
            const auto f = std::find(ostore.calls.begin(), ostore.calls.end(), 'F');
            const auto l = std::find(ostore.calls.begin(), ostore.calls.end(), 'L');
            check(f != ostore.calls.end() && l != ostore.calls.end(),
                  "activation both acquires a fence and reads the store", ok);
            check(f < l, "B2: acquire_fence happens BEFORE the recovery read (stale owner is fenced out)",
                  ok);
        }
        eng4.stop();
    }

    // ---- 6) item 2: a failed acquire_fence dead-letters the first message ----------------------
    {
        FenceFailStore fstore;
        auto built5 = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
        check(built5.has_value(), "fifth ConfigBuilder", ok);
        Engine<> eng5(*built5);
        detail::MessagePool pool5(64);
        check(eng5.declare_lazy<NeedsFenceStore>(fstore).has_value(),
              "declare_lazy<NeedsFenceStore>(fstore)", ok);
        eng5.start();
        LocalRouter router5(eng5.post_courier(), pool5);
        CountedMsg2 m;
        router5.tell<NeedsFenceStore>(actor_id_of<NeedsFenceStore>(1), std::move(m));
        check(wait_until([&] { return CountedMsg2::destructed.load(std::memory_order_acquire) >= 1; },
                         std::chrono::seconds(2)),
              "a failed acquire_fence dead-letters the first message", ok);
        check(fstore.acquires.load() >= 1, "the broker did try to acquire a fence", ok);
        check(NeedsFenceStore::handled.load() == 0, "the actor never handled a message", ok);
        check(eng5.resolve(actor_id_of<NeedsFenceStore>(1)) == nullptr,
              "an unfenced activation is never published to the id table", ok);
        eng5.stop();
    }

    eng.stop();
    std::printf("engine_lazy_persistence_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
