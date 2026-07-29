// Tests ADR-021 (004 §"Node/Shard resolution ordering", 008 §"Metadata compilation") — the eager
// Node/Shard resource resolution pass that runs synchronously inside `Engine`'s constructor, strictly
// before any worker thread exists:
//   * a Node-scoped resource shared by many shards (here 8, well over the ADR's "3+ shards" bar) is
//     constructed EXACTLY ONCE — a counting factory proves it, and every shard's resolved `Cached<T>`
//     pointer is IDENTICAL (not just non-null) — the same instance, never re-resolved per shard;
//   * a Shard-scoped resource is constructed exactly ONCE PER CONFIGURED SHARD — including a shard
//     that hosts no actor at all (004's named "wasted resources" trade-off) — and each shard resolves
//     to its OWN distinct instance;
//   * the first-ever activation on a cold shard never observes an unresolved `Cached<T>` for either
//     lifetime (mirrors Eager's C1) — proven end-to-end through a real `Engine::spawn<A>()` +
//     dispatch, not just the resolution pass in isolation.
// Claim (b) — zero atomics/locks touched during the resolution pass — is argued from the code (no
// atomic/CAS/std::call_once appears anywhere in `resolve_node_shard_resources`/`ResourceArena::
// emplace`/`ResourceScope::provide_raw`) and confirmed empirically: this exact binary is also built
// and run repeatedly under ThreadSanitizer (see the prover's report) — a genuinely single-threaded
// cold phase, so a clean TSan run is expected and confirms nothing is racing during construction.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/resource.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

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

// ---- Node-scoped resource: shared, must be constructed EXACTLY ONCE ---------------------------
struct ConnPool {
    int tag;
};
std::atomic<int> g_node_factory_calls{0};
ConnPool make_conn_pool() {
    g_node_factory_calls.fetch_add(1, std::memory_order_relaxed);
    return ConnPool{4242};
}

// ---- Shard-scoped resource: one fresh instance PER configured shard ---------------------------
struct ShardCache {
    std::uint32_t shard;
};
std::atomic<int> g_shard_factory_calls{0};
ShardCache make_shard_cache(std::uint32_t sid) {
    g_shard_factory_calls.fetch_add(1, std::memory_order_relaxed);
    return ShardCache{sid};
}

struct Ping {};
struct Consumer : Actor<Consumer, Sequential> {
    using protocol = Protocol<Ping>;
    static inline std::atomic<bool> last_pool_resolved{false};
    static inline std::atomic<ConnPool*> last_pool_ptr{nullptr};
    static inline std::atomic<std::uint32_t> last_cache_shard{0xFFFF'FFFFu};
    static inline std::atomic<int> handled{0};
    Cached<ConnPool> pool_;
    Cached<ShardCache> cache_;
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, pool_, cache_); }
    void handle(const Ping&) noexcept {
        last_pool_resolved.store(pool_.resolved(), std::memory_order_release);
        last_pool_ptr.store(pool_.resolved() ? &pool_.get() : nullptr, std::memory_order_release);
        last_cache_shard.store(cache_.resolved() ? cache_->shard : 0xFFFF'FFFFu,
                               std::memory_order_release);
        handled.fetch_add(1, std::memory_order_release);
    }
};

}  // namespace

int main() {
    bool ok = true;
    static constexpr std::uint32_t kShardCount = 8;  // well over the ADR's "3+ shards" bar

    NodeShardResourcePlan plan;
    plan.provide_node<ConnPool>(&make_conn_pool);
    plan.provide_shard<ShardCache>(&make_shard_cache);

    Engine<> eng(EngineConfig{2, kShardCount, 1024, 64}, plan);

    // ---- 1) Node factory ran EXACTLY ONCE, regardless of shard_count = 8 ----------------------
    check(g_node_factory_calls.load() == 1, "Node factory invoked exactly once (counting factory)", ok);
    check(eng.node_resources_constructed() == 1, "arena holds exactly one Node-resource instance", ok);

    // ---- 2) Shard factory ran EXACTLY ONCE PER SHARD, for EVERY configured shard (even shards
    //         that will host no actor — the accepted 004 trade-off) --------------------------------
    check(g_shard_factory_calls.load() == static_cast<int>(kShardCount),
          "Shard factory invoked exactly once per configured shard", ok);
    for (std::uint32_t sh = 0; sh < kShardCount; ++sh)
        check(eng.shard_resources_constructed(sh) == 1,
              "each shard's arena holds exactly one Shard-resource instance", ok);

    // ---- 3) Node-scoped: IDENTICAL pointer resolved from every shard's scope (not re-resolved) --
    ConnPool* first_pool_ptr = nullptr;
    for (std::uint32_t sh = 0; sh < kShardCount; ++sh) {
        Cached<ConnPool> c;
        result<void> w = c.wire(eng.shard_resource_scope(sh));
        check(w.has_value(), "Cached<ConnPool>::wire succeeds from every shard's scope", ok);
        check(c.resolved(), "Cached<ConnPool> resolved (never null) from every shard's scope", ok);
        if (sh == 0) {
            first_pool_ptr = &c.get();
        } else {
            check(&c.get() == first_pool_ptr,
                  "Node resource pointer is IDENTICAL across every shard (constructed once, shared)",
                  ok);
        }
    }
    check(first_pool_ptr != nullptr && first_pool_ptr->tag == 4242,
          "Node resource carries the value its (single) factory call produced", ok);

    // ---- 4) Shard-scoped: DISTINCT instance per shard, correct per-shard value -----------------
    for (std::uint32_t sh = 0; sh < kShardCount; ++sh) {
        Cached<ShardCache> c;
        result<void> w = c.wire(eng.shard_resource_scope(sh));
        check(w.has_value(), "Cached<ShardCache>::wire succeeds", ok);
        check(c.resolved() && c->shard == sh, "Shard resource resolves to THIS shard's own value", ok);
    }

    // ---- 5) First-ever activation on a cold shard never sees an unresolved Cached<T> (mirrors
    //         Eager's C1) — proven end-to-end via a real spawn<A>() + dispatch on several shards. ----
    detail::MessagePool pool(256);
    LocalRouter router(eng.post_courier(), pool);
    eng.start();

    for (std::uint32_t key = 1; key <= 5; ++key) {
        const ActorId id = actor_id_of<Consumer>(key);
        const std::uint32_t sid = eng.shard_of(id);
        result<ActorId> spawned = eng.spawn<Consumer>(key, pool.sink(), &eng.shard_resource_scope(sid));
        check(spawned.has_value(), "spawn<Consumer> succeeds using the Engine's resolved shard scope",
              ok);
        Consumer::last_pool_resolved.store(false, std::memory_order_release);
        Consumer::handled.store(0, std::memory_order_release);
        router.tell<Consumer>(*spawned, Ping{});
        check(wait_until([&] { return Consumer::handled.load(std::memory_order_acquire) == 1; },
                         std::chrono::seconds(2)),
              "the first-ever message dispatches", ok);
        check(Consumer::last_pool_resolved.load(std::memory_order_acquire),
              "first activation's Cached<ConnPool> is already resolved (never null/unresolved)", ok);
        check(Consumer::last_pool_ptr.load(std::memory_order_acquire) == first_pool_ptr,
              "first activation's Node resource is the SAME shared instance", ok);
        check(Consumer::last_cache_shard.load(std::memory_order_acquire) == sid,
              "first activation's Shard resource matches its OWN shard", ok);
    }

    eng.stop();

    std::printf("resource_node_shard_eager_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
