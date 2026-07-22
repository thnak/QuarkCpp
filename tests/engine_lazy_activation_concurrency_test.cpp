// Tests ADR-028 Phase 4 concurrency guarantees under a REAL multi-worker, multi-shard Engine:
//   S1 exactly-one-construction — N threads racing tell() to the SAME never-touched cold ActorId
//      must construct the actor exactly once and lose no message (mirrors the ADR's proven S1).
//   bug-(a) regression pin — many DISTINCT ids (scattered across shards by shard_of()'s hash)
//      lazily activated CONCURRENTLY from many threads must not race: each shard's broker owns its
//      own `Shard::lazy_owned`/`id_table`, never a single Engine-wide container. A regression back
//      to a shared/Engine-wide ownership container is exactly the bug the ADR's proving run found
//      and required fixed — this is the shape that would show it under TSan.
// Run under TSan (see the project's build matrix) as the primary evidence; the assertions below
// hold under any sanitizer configuration.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {};

// S1: exactly-one-construction under N threads racing the SAME cold id.
struct RaceActor : Actor<RaceActor, Sequential> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<int> constructions{0};
    static inline std::atomic<long long> handled{0};
    RaceActor() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Ping&) noexcept { handled.fetch_add(1, std::memory_order_release); }
};

// bug-(a) pin: many distinct ids of this type, scattered across shards, activated concurrently.
struct ScatterActor : Actor<ScatterActor, Sequential> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<long long> handled{0};
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

    auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig", ok);
    if (!built) {
        std::printf("engine_lazy_activation_concurrency_test: FAIL (config build failed)\n");
        return 1;
    }

    Engine<> eng(*built);
    detail::MessagePool pool(4096);
    // Mirror `spawn<A>(key, pool.sink())`'s existing convention: whichever pool built a message's
    // cell must also be the actor's wired reclaim sink (the default `ReclaimSink{}` only resets the
    // descriptor in place — it does NOT return the cell to `pool`'s free list — see engine.hpp's
    // `LazyOwned`/`declare_lazy` comments).
    check(eng.declare_lazy<RaceActor>(nullptr, pool.sink()).has_value(), "declare_lazy<RaceActor>", ok);
    check(eng.declare_lazy<ScatterActor>(nullptr, pool.sink()).has_value(), "declare_lazy<ScatterActor>",
          ok);
    eng.start();

    LocalRouter router(eng.post_courier(), pool);

    // ---- 1) S1 — N threads race the SAME cold ActorId. -----------------------------------------
    {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 500;
        const ActorId id = actor_id_of<RaceActor>(1);

        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) router.tell<RaceActor>(id, Ping{});
            });
        }
        for (auto& th : threads) th.join();

        constexpr long long expected = static_cast<long long>(kThreads) * kPerThread;
        check(wait_until([&] { return RaceActor::handled.load(std::memory_order_acquire) == expected; },
                         std::chrono::seconds(5)),
              "every racing message is delivered exactly once (no loss, no duplicate dispatch)", ok);
        check(RaceActor::constructions.load(std::memory_order_relaxed) == 1,
              "S1: exactly one construction across all racing first-touches", ok);
    }

    // ---- 2) bug-(a) pin — many distinct ids, scattered across shards, activated concurrently. ----
    {
        constexpr int kKeys = 64;
        constexpr int kPerKey = 20;

        std::vector<std::thread> threads;
        threads.reserve(kKeys);
        for (int k = 0; k < kKeys; ++k) {
            threads.emplace_back([&, k] {
                const ActorId id = actor_id_of<ScatterActor>(static_cast<std::uint64_t>(k + 1000));
                for (int i = 0; i < kPerKey; ++i) router.tell<ScatterActor>(id, Ping{});
            });
        }
        for (auto& th : threads) th.join();

        constexpr long long expected = static_cast<long long>(kKeys) * kPerKey;
        check(wait_until([&] { return ScatterActor::handled.load(std::memory_order_acquire) == expected; },
                         std::chrono::seconds(5)),
              "every message across kKeys concurrently-activated, shard-scattered actors is delivered "
              "(per-shard-only broker ownership — bug (a) regression pin)",
              ok);
    }

    eng.stop();
    std::printf("engine_lazy_activation_concurrency_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
