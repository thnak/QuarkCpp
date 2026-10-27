// Tests ADR-052 (#24b) — idle eviction RECLAIMS memory: after N distinct lazily-activated ids idle
// out, `Shard::lazy_owned` and `id_table` return to baseline (they used to be append-only). Also:
// a reclaimed id re-activates transparently with a fresh construction and no message loss, and with
// reclaim disabled (the default) nothing is freed (old behavior unchanged).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {
struct Ping {
    int n = 1;
};
struct Ephemeral : Actor<Ephemeral, Sequential, IdleTimeout<5>> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<std::uint64_t> constructions{0};
    static inline std::atomic<std::uint64_t> destructions{0};
    static inline std::atomic<std::uint64_t> handled{0};
    Ephemeral() noexcept { constructions.fetch_add(1, std::memory_order_relaxed); }
    ~Ephemeral() { destructions.fetch_add(1, std::memory_order_relaxed); }
    void handle(const Ping&) noexcept { handled.fetch_add(1, std::memory_order_release); }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
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

// Touch `count` distinct ids, in bounded batches (the message pool is finite).
template <class Router>
void touch_all(Router& router, std::uint64_t first_key, std::uint64_t count, bool& ok) {
    constexpr std::uint64_t kBatch = 128;
    for (std::uint64_t done = 0; done < count; done += kBatch) {
        const std::uint64_t n = std::min(kBatch, count - done);
        const std::uint64_t target = Ephemeral::handled.load(std::memory_order_acquire) + n;
        for (std::uint64_t i = 0; i < n; ++i)
            router.template tell<Ephemeral>(actor_id_of<Ephemeral>(first_key + done + i), Ping{});
        if (!wait_until([&] { return Ephemeral::handled.load(std::memory_order_acquire) >= target; },
                        std::chrono::seconds(30))) {
            check(false, "batch delivered", ok);
            return;
        }
    }
}
}  // namespace

int main() {
    bool ok = true;
    const std::uint64_t kKeys = 100'000;

    // ---- A) reclaim enabled -------------------------------------------------------------------
    {
        auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).idle_tick_ms(1)
                         .lazy_reclaim_grace_ms(10).build();
        check(built.has_value(), "config builds", ok);
        Engine<> eng(*built);
        detail::MessagePool pool(256);
        check(eng.declare_lazy<Ephemeral>(nullptr, pool.sink()).has_value(), "declare_lazy", ok);
        eng.start();
        LocalRouter router(eng.post_courier(), pool);

        const std::size_t base_owned = eng.lazy_owned_size(), base_ids = eng.id_table_size();
        check(base_owned == 0 && base_ids == 0, "baseline is empty", ok);

        touch_all(router, 1, kKeys, ok);
        check(Ephemeral::constructions.load() >= kKeys, "at least one construction per distinct key", ok);

        // Everything idles out (5 ms) then is unlinked and freed (+2 x 10 ms grace).
        const bool drained = wait_until(
            [&] { return eng.lazy_owned_size() == base_owned && eng.id_table_size() == base_ids; },
            std::chrono::seconds(60));
        check(drained, "lazy_owned and id_table return to baseline after idle eviction", ok);
        check(Ephemeral::destructions.load() == Ephemeral::constructions.load(),
              "every evicted actor instance was destroyed", ok);

        // A reclaimed id comes back with a fresh construction and loses no message.
        const std::uint64_t c0 = Ephemeral::constructions.load();
        touch_all(router, 1, 64, ok);
        check(Ephemeral::constructions.load() >= c0 + 64, "reclaimed ids re-activate (fresh instance)", ok);
        eng.stop();
    }
    Ephemeral::constructions = 0;
    Ephemeral::destructions = 0;
    Ephemeral::handled = 0;

    // ---- B) default config: reclaim OFF — nothing freed (pre-ADR-052 behavior) ------------------
    {
        auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).idle_tick_ms(1).build();
        Engine<> eng(*built);
        detail::MessagePool pool(256);
        (void)eng.declare_lazy<Ephemeral>(nullptr, pool.sink());
        eng.start();
        LocalRouter router(eng.post_courier(), pool);
        touch_all(router, 1, 1000, ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(eng.lazy_owned_size() == 1000 && eng.id_table_size() == 1000,
              "reclaim disabled by default: entries stay resident", ok);
        eng.stop();
    }
    std::printf("engine_lazy_reclaim_test: %s  (keys=%llu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(kKeys));
    return ok ? 0 : 1;
}
