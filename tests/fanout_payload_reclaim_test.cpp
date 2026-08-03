// Tests 006 §Publish/Subscribe (ordered + reliable fan-out) / ADR-039 — the shared payload is
// reclaimed EXACTLY ONCE (SharedPayloadPool<M> cell returns to the pool) whether a lane consumes,
// gets evicted (EvictAfter), or is destroyed with a queued backlog via ~LaneEntry() (S2r's fix: reclaim
// runs there, single-threaded, refcount-gated — never on the producer thread). One payload copy per
// publish, independent of N (reused verbatim from Topic<M>/003 — the pool mechanics themselves are
// already proven in topic_payload_reclaim_test.cpp; this file proves FanOut's own reclaim call sites).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/fanout.hpp"

using namespace quark;

namespace {
std::atomic<std::int64_t> g_ctor{0};
struct Counted {
    std::uint64_t v = 0;
    Counted() = default;
    explicit Counted(std::uint64_t x) : v(x) {}
    Counted(const Counted& o) : v(o.v) { g_ctor.fetch_add(1, std::memory_order_relaxed); }
    Counted(Counted&& o) noexcept : v(o.v) { g_ctor.fetch_add(1, std::memory_order_relaxed); }
    Counted& operator=(const Counted& o) { v = o.v; return *this; }
    Counted& operator=(Counted&& o) noexcept { v = o.v; return *this; }
};
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;
    constexpr std::size_t kWarm = 256;

    // ---- (A) 1 copy/publish independent of N; reclaimed exactly once when fully drained -----------
    for (std::uint32_t N : {1u, 16u, 256u}) {
        using FO = FanOut<Counted, OnSlowSubscriber<EvictAfter<64>>>;
        FO fo(64, kWarm);
        std::vector<std::shared_ptr<FO::LaneEntry>> lanes;
        for (std::uint32_t i = 0; i < N; ++i) lanes.push_back(fo.subscribe(ActorId{{1}, i}));
        const std::size_t free_before = fo.pool().free_count();

        g_ctor.store(0);
        auto r = fo.publish(Counted{42});
        check(g_ctor.load() == 1, "1 payload construction per publish, independent of N", ok);
        check(r.delivered == N, "delivered to all N", ok);
        check(fo.pool().free_count() == free_before - 1, "exactly one pool cell in use during fan-out", ok);

        std::uint64_t got = 0;
        for (auto& lane : lanes) { Counted out; while (lane->try_pop(out)) ++got; }
        check(got == N, "each subscriber consumed exactly one copy", ok);
        check(fo.pool().free_count() == free_before,
              "GATE 4 analogue: shared payload reclaimed EXACTLY ONCE after all consume (pool restored)", ok);
    }

    // ---- (B) reclaim on EVICT (small lane overflows) -----------------------------------------------
    {
        using FO = FanOut<Counted, OnSlowSubscriber<EvictAfter<2>>>;
        FO fo(2, kWarm);
        auto lane = fo.subscribe(ActorId{{1}, 1});
        const std::size_t free_before = fo.pool().free_count();
        for (int i = 0; i < 10; ++i) (void)fo.publish(Counted{static_cast<std::uint64_t>(i)});
        // The lane holds at most 2 undelivered refs; the rest were evicted and released at eviction
        // time (not leaked, not left pinned).
        Counted out;
        while (lane->try_pop(out)) {}
        check(fo.pool().free_count() == free_before,
              "GATE 4 analogue: every payload reclaimed exactly once across eviction (no leak)", ok);
    }

    // ---- (C) reclaim on ~LaneEntry() with a queued backlog (S2r: destructor-only reclaim) ---------
    {
        using FO = FanOut<Counted, OnSlowSubscriber<EvictAfter<8>>>;
        FO fo(8, kWarm);
        auto lane = fo.subscribe(ActorId{{1}, 1});
        const std::size_t free_before = fo.pool().free_count();
        for (int i = 0; i < 8; ++i) (void)fo.publish(Counted{static_cast<std::uint64_t>(i)});  // fills it, 0 drained
        check(fo.pool().free_count() == free_before - 8, "8 undrained refs pinned in the lane's backlog", ok);
        fo.unsubscribe(ActorId{{1}, 1});
        lane.reset();  // DROPS the last shared_ptr<LaneEntry> ref -> ~LaneEntry() must reclaim all 8
        check(fo.pool().free_count() == free_before,
              "S2r: ~LaneEntry() reclaims a full undrained backlog exactly once (no leak)", ok);
    }

    // ---- (D) reclaim on ~LaneEntry() with a queued backlog under Block ------------------------------
    {
        using FO = FanOut<Counted, OnSlowSubscriber<Block>>;
        FO fo(8, kWarm);
        auto lane = fo.subscribe(ActorId{{1}, 1});
        const std::size_t free_before = fo.pool().free_count();
        for (int i = 0; i < 8; ++i) (void)fo.publish(Counted{static_cast<std::uint64_t>(i)});  // exactly fills it
        check(fo.pool().free_count() == free_before - 8, "8 undrained refs pinned in the Block lane's backlog", ok);
        fo.unsubscribe(ActorId{{1}, 1});
        lane.reset();
        check(fo.pool().free_count() == free_before,
              "S2r: ~LaneEntry() reclaims a full undrained Block backlog exactly once (no leak)", ok);
    }

    std::fprintf(stderr, "fanout_payload_reclaim_test: %s (ctor=%lld)\n", ok ? "PASS" : "FAIL",
                 static_cast<long long>(g_ctor.load()));
    return ok ? 0 : 1;
}
