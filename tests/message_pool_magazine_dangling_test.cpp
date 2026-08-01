// ADR-037 (TLS acquire/reclaim magazine, message_pool.hpp) S2/C3: a thread's magazine binds a
// Partition* via a std::weak_ptr<PartitionToken> liveness check, never a raw pointer compare, so a
// MessagePool destroyed while a thread's magazine still references it can never let that thread's
// NEXT pool (possibly allocated at the exact same address the allocator just freed) hand out a
// stale, freed cell. This is the exact fatal use-after-free the ADR-037 red team found in the
// original raw-`Partition*` draft (reproduced under TSan+ASan) and the fix this test locks in.
//
// Round (a): thread exits (holding a resident, un-reclaimed magazine cell) while the pool it used
// stays alive across the join — the thread's LocalCacheTable destructor flushes automatically at
// thread-exit; the pool must still be intact (never touched-after-free from the OTHER direction).
//
// Round (b): the dangerous case. A thread acquires from pool A (leaving a resident magazine cell,
// never reclaimed, never flushed), THEN pool A is destroyed WHILE that thread's magazine still binds
// A's partition, THEN the same thread constructs pool B (a same-shape pool very plausibly reusing
// A's just-freed heap address) and hammers it. Every cell B hands out must be a live, correctly
// constructed Msg — a stale A-owned cell surfacing here would show up as a live-count mismatch and,
// under ASan, a heap-use-after-free report.
//
// Run under ASan/UBSan (and TSan, though this file is single-threaded-per-round so TSan mainly
// exercises the cross-thread pool teardown ordering, not a data race).
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

std::atomic<long> g_live{0};

struct Msg {
    int token;
    explicit Msg(int t) noexcept : token(t) { g_live.fetch_add(1, std::memory_order_relaxed); }
    ~Msg() { g_live.fetch_add(-1, std::memory_order_relaxed); }
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

bool round_a_thread_exit_before_pool_destruction() {
    bool ok = true;
    auto poolA = std::make_unique<detail::MessagePool>(64, 1);
    std::thread th([&] {
        detail::MessagePool::Slot slot = poolA->acquire(&detail::destroy_payload<Msg>);
        ::new (slot.payload) Msg(1);
        slot.desc->payload = slot.payload;
        poolA->reclaim(slot.desc);
        // reclaim() pushed this cell back into the thread's magazine (not flushed to free_head,
        // since the magazine is far from its kReturnBatch threshold) — it, and the other free cells
        // this acquire()'s refill() pulled in, stay resident in this thread's magazine, bound to
        // poolA's partition, when the thread exits below. Thread exit must flush them automatically.
    });
    th.join();
    // poolA destructs here; th has already exited and joined, so its LocalCacheTable destructor
    // already ran (and found poolA's token still lockable, since poolA was alive at that point).
    check(g_live.load() == 0, "round(a): live Msg count back to 0 after thread exit", ok);
    return ok;
}

bool round_b_pool_destroyed_with_resident_magazine_then_touch_pool_b() {
    bool ok = true;
    constexpr int kCycles = 100'000;
    std::thread th([&] {
        auto poolA = std::make_unique<detail::MessagePool>(64, 1);
        // No destructor thunk: this cell is deliberately abandoned (never reclaimed), so it carries
        // no live-tracked Msg to leak — the invariant under test is safety (no UAF touching A's freed
        // Partition/Cell storage), not accounting for an intentionally-abandoned handle. The refill()
        // this acquire() triggers still leaves OTHER free cells resident in this thread's magazine
        // (bound to A's partition) — that's the actual case S2 exists to cover.
        detail::MessagePool::Slot slot = poolA->acquire(nullptr);
        (void)slot;
        poolA.reset();  // A destroyed WHILE this thread's magazine still references A's partition.

        // A same-shape pool, constructed immediately after A's destruction — plausibly reusing A's
        // just-freed heap address for its Partition[] array.
        auto poolB = std::make_unique<detail::MessagePool>(64, 1);
        for (int i = 0; i < kCycles; ++i) {
            detail::MessagePool::Slot s = poolB->acquire(&detail::destroy_payload<Msg>);
            ::new (s.payload) Msg(i);
            s.desc->payload = s.payload;
            // If B ever handed out a stale A-owned cell, this dereference (or the payload
            // construction above) is exactly what ASan would catch as heap-use-after-free.
            (void)s.desc->generation();
            poolB->reclaim(s.desc);
        }
        detail::flush_current_thread_message_caches();
    });
    th.join();
    check(g_live.load() == 0, "round(b): live Msg count back to 0 after B's cycles", ok);
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    g_live.store(0, std::memory_order_relaxed);
    ok &= round_a_thread_exit_before_pool_destruction();
    g_live.store(0, std::memory_order_relaxed);
    ok &= round_b_pool_destroyed_with_resident_magazine_then_touch_pool_b();
    std::printf("message_pool_magazine_dangling_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
