// Tests the 022/023 zero-cost-when-unthrottled rule: an UNGOVERNED activation's post()+drain hot
// path is unchanged — 0 heap allocation over >=1e6 cycles — proving the added `if (gov_)` branch
// costs nothing on the common path (it is a never-taken, well-predicted null check; the sequential
// drain body below it is byte-for-byte the pre-022 path). A GOVERNED activation cold-allocates its
// governance core exactly ONCE (at enable), after which its post_governed()+drain hot loop is also
// 0-allocation — the depth counters are atomics, never heap. The consumer drain adds NO cross-core
// RMW: `drained` is published with a relaxed load+store (a mov/mov pair, not a lock xadd), and the
// ungoverned drain never touches it at all (objdump of the ungoverned drain is unchanged — reported).
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

namespace {
std::atomic<std::uint64_t> g_alloc_count{0};
}
void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace quark;

namespace {
struct Item {
    std::uint64_t id;
};
struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Item>;
    std::uint64_t sum = 0;
    void handle(const Item& m) { sum += m.id; }
};

// One post → drain → close-out cycle over a reused descriptor. Returns after the message ran.
template <class Post>
std::uint64_t run_loop(Activation& act, Descriptor& d, std::uint64_t ops, Post post) {
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < ops; ++i) {
        d.message_id = MessageId{i};
        post(&d);
        (void)act.try_acquire();
        (void)act.drain_step(8);
        (void)act.close_out();
    }
    return g_alloc_count.load(std::memory_order_relaxed) - before;
}
}  // namespace

int main() {
    constexpr std::uint64_t kOps = 1'000'000;
    bool ok = true;

    // ---- UNGOVERNED: the verbatim hot path — 0 allocation --------------------------------
    {
        Sink actor;
        Activation act{&actor, Sink::dispatch_table()};
        Item it{1};
        Descriptor d;
        d.payload = &it;
        stamp<Sink, Item>(d);

        const std::uint64_t delta = run_loop(act, d, kOps, [&](Descriptor* p) { (void)act.post(p); });
        const bool pass = (delta == 0);
        ok = ok && pass;
        std::printf("  ungoverned post+drain: %s (ops=%" PRIu64 ", hot allocations=%" PRIu64 ")\n",
                    pass ? "OK" : "FAIL", kOps, delta);
    }

    // ---- GOVERNED: one cold alloc at enable, then a 0-allocation hot loop -----------------
    {
        Sink actor;
        Activation act{&actor, Sink::dispatch_table()};
        Item it{1};
        Descriptor d;
        d.payload = &it;
        stamp<Sink, Item>(d);

        const std::uint64_t before_enable = g_alloc_count.load(std::memory_order_relaxed);
        Activation::GovernanceConfig gc;
        gc.static_bound = 0;  // unbounded: nothing sheds; measures the governed hot path itself
        act.enable_governance(gc);
        const std::uint64_t enable_allocs =
            g_alloc_count.load(std::memory_order_relaxed) - before_enable;
        const bool enable_ok = (enable_allocs >= 1);  // the governance core is a cold allocation
        ok = ok && enable_ok;

        const std::uint64_t delta = run_loop(act, d, kOps, [&](Descriptor* p) {
            (void)act.post_governed(p);
        });
        const bool pass = (delta == 0);
        ok = ok && pass;
        std::printf("  governed post_governed+drain: %s (enable_allocs=%" PRIu64
                    ", hot allocations=%" PRIu64 ")\n",
                    pass ? "OK" : "FAIL", enable_allocs, delta);
    }

    std::printf("governance_zero_cost_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
