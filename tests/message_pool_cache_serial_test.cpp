// ADR-077/ADR-037: the per-acquire magazine fast path caches a (pool serial, partition, LocalCache*)
// in the 8-slot per-pool TLS table. Serial numbers are never reused, so a stale slot from a destroyed
// pool can never satisfy the fast check; the 64-slot LocalCacheTable can still evict a magazine on a
// hash collision, so the cached pointer re-validates bound+serial. This test exercises:
//   * >8 interleaved pools on one thread (TLS slot eviction),
//   * acquire/reclaim across pools with retained slots (home-partition routing),
//   * a destroyed-then-recreated pool (serial churn),
//   * a multi-partition pool with cells acquired on different threads and reclaimed on one.
// It asserts cell conservation, not timing.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {
std::atomic<std::uint64_t> g_destroyed{0};
void count_destroy(void*) noexcept { g_destroyed.fetch_add(1, std::memory_order_relaxed); }

bool check(bool cond, const char* what) {
    if (!cond) std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
    return cond;
}
}  // namespace

int main() {
    bool ok = true;

    // ---- 1) 9 pools (> the 8-slot per-pool TLS table) interleaved on one thread ------------------
    {
        constexpr std::size_t kPools = 9;
        constexpr std::size_t kCap = 64;
        std::array<std::unique_ptr<detail::MessagePool>, kPools> pools;
        for (auto& p : pools) p = std::make_unique<detail::MessagePool>(kCap);

        // Acquire from every pool, retain the slots, then reclaim in reverse/interleaved order so each
        // thread-local lookup has to rebind across pools (and the 8-slot table wraps).
        std::array<std::array<detail::MessagePool::Slot, 16>, kPools> slots{};
        for (std::size_t i = 0; i < kPools; ++i)
            for (std::size_t k = 0; k < slots[i].size(); ++k) {
                slots[i][k] = pools[i]->acquire(&count_destroy);
                // The pool hands back {descriptor, inline payload}; the caller (make_descriptor) is what
                // normally stamps Descriptor::payload. reclaim() only runs the thunk when it is set.
                slots[i][k].desc->payload = slots[i][k].payload;
            }

        g_destroyed.store(0, std::memory_order_relaxed);
        for (std::size_t r = 0; r < kPools; ++r) {
            const std::size_t i = kPools - 1 - r;  // reverse order, then forward in a second pass
            for (auto& s : slots[i]) pools[i]->reclaim(s.desc);
        }
        for (std::size_t i = 0; i < kPools; ++i)
            for (std::size_t k = 0; k < slots[i].size(); ++k)
                slots[i][k] = pools[i]->acquire(nullptr);  // reuse the returned cells
        for (std::size_t i = 0; i < kPools; ++i)
            for (auto& s : slots[i]) pools[i]->reclaim(s.desc);
        ok &= check(g_destroyed.load() == kPools * 16, "one destroy thunk per reclaimed cell");

        detail::flush_current_thread_message_caches();
        for (auto& p : pools) ok &= check(p->total_cells() == kCap, "9-pool cell conservation");

        // Destroy all, then build a fresh set: the new serials must never hit the old cached slots.
        pools = {};
        for (auto& p : pools) p = std::make_unique<detail::MessagePool>(kCap);
        for (auto& p : pools) {
            auto s = p->acquire(nullptr);
            p->reclaim(s.desc);
        }
        detail::flush_current_thread_message_caches();
        for (auto& p : pools) ok &= check(p->total_cells() == kCap, "recreated-pool cell conservation");
    }

    // ---- 2) multi-partition: cells acquired on 4 threads, reclaimed on the main thread -------------
    {
        constexpr std::size_t kParts = 4;
        constexpr std::size_t kCap = 256;
        detail::MessagePool pool(kCap, kParts);
        std::array<std::vector<detail::MessagePool::Slot>, kParts> per_thread;
        std::array<std::thread, kParts> workers;
        for (std::size_t t = 0; t < kParts; ++t) {
            workers[t] = std::thread([&, t] {
                per_thread[t].reserve(16);
                for (int k = 0; k < 16; ++k) per_thread[t].push_back(pool.acquire(nullptr));
            });
        }
        for (auto& w : workers) w.join();
        for (auto& v : per_thread)
            for (auto& s : v) pool.reclaim(s.desc);
        detail::flush_current_thread_message_caches();
        ok &= check(pool.total_cells() == kCap, "multi-partition cell conservation");
    }

    std::printf("message_pool_cache_serial_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
