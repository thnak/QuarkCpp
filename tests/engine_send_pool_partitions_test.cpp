// Tests ADR-060 (#4) EngineConfig::pool_partitions + Engine::make_send_pool(): the config knob reaches
// the pool, the default (1) is unchanged, and for 1 / 2 / 8 partitions with concurrent producers every
// message is handled exactly once (no loss, no duplicate) through the real tell -> mailbox -> drain path.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {
std::atomic<std::uint64_t> g_handled{0};

struct Counter : Actor<Counter, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.test.engine_send_pool_partitions_test.Counter";
    using protocol = Protocol<int>;
    void handle(const int&) noexcept { g_handled.fetch_add(1, std::memory_order_relaxed); }
};

bool run(std::uint32_t partitions, unsigned producers, std::uint64_t per_producer) {
    g_handled.store(0);
    EngineConfig cfg{2, 2, 64, 1024};
    cfg.pool_partitions = partitions;
    Engine eng(cfg);
    auto pool = eng.make_send_pool();
    auto spawned = eng.spawn<Counter>(1, pool->sink());
    if (!spawned) return false;
    LocalRouter router(eng.post_courier(), *pool);
    ActorRef<Counter> ref = router.get<Counter>(1);
    eng.start();

    std::vector<std::thread> ths;
    for (unsigned t = 0; t < producers; ++t)
        ths.emplace_back([&] {
            for (std::uint64_t i = 0; i < per_producer; ++i) ref.tell(static_cast<int>(i));
        });
    for (auto& th : ths) th.join();

    const std::uint64_t want = per_producer * producers;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (g_handled.load(std::memory_order_acquire) < want && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    eng.stop();
    const std::uint64_t got = g_handled.load();
    if (got != want)
        std::fprintf(stderr, "  partitions=%u: handled %llu != sent %llu\n", partitions,
                     static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    return got == want;
}
}  // namespace

int main() {
    bool ok = true;
    ok &= EngineConfig{}.pool_partitions == 0;  // default = auto (ADR-063)
    {  // the auto rule (hw_threads injected so the result does not depend on this machine)
        EngineConfig c{};
        ok &= resolve_pool_partitions(c, 1) == 1;     // 1 hw thread: nothing to spread
        ok &= resolve_pool_partitions(c, 2) == 2;
        ok &= resolve_pool_partitions(c, 3) == 4;     // power of two, never odd
        ok &= resolve_pool_partitions(c, 28) == 4;    // default 4096 cells caps it at 4 x 1024
        c.shard_count = 4;                            // 16384 cells -> up to 16 partitions
        ok &= resolve_pool_partitions(c, 28) == 16;
        ok &= resolve_pool_partitions(c, 64) == 16;   // hard cap
        c.pool_capacity = 1024;                       // 4096 cells total -> 4
        ok &= resolve_pool_partitions(c, 28) == 4;
        c.pool_partitions = 3;                        // explicit value is verbatim
        ok &= resolve_pool_partitions(c, 28) == 3;
        if (!ok) std::fprintf(stderr, "  resolve_pool_partitions rules failed\n");
    }
    {
        auto cfg = ConfigBuilder{}.pool_partitions(4).build();
        ok &= cfg.has_value() && cfg->pool_partitions == 4;
    }
    for (std::uint32_t p : {0u, 1u, 2u, 8u}) ok &= run(p, 4, 50'000);  // 0 = auto default
    std::printf("engine_send_pool_partitions_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
