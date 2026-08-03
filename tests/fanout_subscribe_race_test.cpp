// Tests 006 §Publish/Subscribe (ordered + reliable fan-out) / ADR-039 S1 (subscribe/unsubscribe
// race-free, including the Block-policy departure-wake) + S2r (evict()/departure backlog reclaim,
// exactly-once, no cross-subscriber UAF — reclaim runs in ~LaneEntry() only, never on the producer
// thread). A churn thread repeatedly subscribes -> lets publishes land -> unsubscribes -> drains ->
// DROPS its own LaneEntry handle, while publisher thread(s) fan out continuously. If bounded
// quiescence or the departure-wake were wrong, a publisher would touch a freed/departing lane
// (ASan heap-use-after-free / TSan data race) or a producer would hang forever in push_blocking_while.
//
// MACHINE SAFETY: exactly 2 worker threads per section (1 publisher + 1 churn) — FanOut is
// single-producer only (ADR-039), so a second concurrent publish() would itself be an unsupported
// precondition violation, not a race worth testing. Round counts are bounded (EvictAfter never
// blocks, so it can run cheap/high-volume churn; Block genuinely parks threads each round, so its
// churn section uses far fewer rounds to keep worst-case runtime bounded).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/fanout.hpp"

using namespace quark;

namespace {
std::atomic<std::int64_t> g_live{0};
struct Ev {
    std::uint64_t v = 0;
    Ev() { g_live.fetch_add(1, std::memory_order_relaxed); }
    explicit Ev(std::uint64_t x) : v(x) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev(const Ev& o) : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev(Ev&& o) noexcept : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev& operator=(const Ev& o) { v = o.v; return *this; }
    Ev& operator=(Ev&& o) noexcept { v = o.v; return *this; }
    ~Ev() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;

    // ================================================================================================
    // (A) EvictAfter churn — cheap/non-blocking, so a high round count for a low false-negative rate
    // (mirrors topic_subscribe_race_test.cpp's rationale: the UAF race window is narrow).
    // ================================================================================================
    {
        constexpr std::uint64_t kRounds = 200'000;
        constexpr std::uint32_t kStable = 8;
        using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<256>>>;

        FO fo(256, 4096);
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> post_unsub_deliveries{0};

        std::vector<std::shared_ptr<FO::LaneEntry>> stable;
        for (std::uint32_t i = 0; i < kStable; ++i) {
            auto lane = fo.subscribe(ActorId{{1}, i});
            check(lane != nullptr, "stable subscribe", ok);
            stable.push_back(lane);
        }

        // NOTE: exactly ONE publisher thread — FanOut's precondition is single-producer (ADR-039
        // residual risks), unlike Topic<M>, which explicitly supports concurrent publish() calls. Two
        // producer threads calling publish() concurrently would race on each lane's single-writer
        // `head_` cursor (StreamChannel has no CAS there BY DESIGN — that's the 0-RMW drain path), so
        // this test exercises subscribe/unsubscribe churn concurrently with the ONE allowed publisher,
        // not multi-producer fan-in (which FanOut does not claim to support).
        //
        // The stable lanes are deliberately NOT drained while the publisher runs: a FanOut LaneEntry
        // wraps a StreamChannel — single-consumer by design — so draining them here would need its own
        // dedicated thread to avoid a second-consumer violation. EvictAfter never blocks on a full
        // stable lane (it evicts), so the publisher doesn't need them drained to keep making progress;
        // they're drained once, single-threaded, after the publisher joins.
        auto publisher = [&] {
            std::uint64_t k = 0;
            while (!stop.load(std::memory_order_relaxed)) { (void)fo.publish(Ev{k++}); }
        };
        std::thread p1(publisher);

        std::thread churn([&] {
            const ActorId cid{{7}, 42};
            for (std::uint64_t r = 0; r < kRounds; ++r) {
                auto lane = fo.subscribe(cid);
                std::this_thread::yield();
                { Ev out; for (int spin = 0; spin < 32; ++spin) if (lane->try_pop(out)) break; }
                fo.unsubscribe(cid);  // bounded quiescence: no publish may reference `lane` after this
                Ev out;
                while (lane->try_pop(out)) {}
                std::uint64_t extra = 0;
                while (lane->try_pop(out)) ++extra;  // must stay 0 — nothing lands after unsubscribe
                post_unsub_deliveries.fetch_add(extra, std::memory_order_relaxed);
                lane.reset();  // DROP the subscriber's own handle — refcount may reach 0 -> ~LaneEntry()
            }
            stop.store(true, std::memory_order_relaxed);
        });

        churn.join();
        p1.join();

        for (auto& lane : stable) { Ev out; while (lane->try_pop(out)) {} }
        stable.clear();

        check(post_unsub_deliveries.load() == 0, "S1: no delivery after unsubscribe() returns (EvictAfter)", ok);
        const std::int64_t live = g_live.load();
        check(live == 0, "S2r: every payload reclaimed exactly once under churn (0 live at quiescence)", ok);
        check(fo.in_flight() == 0, "no publish in flight at quiescence", ok);
        std::fprintf(stderr, "  EvictAfter churn: live=%lld post_unsub=%llu\n", static_cast<long long>(live),
                     static_cast<unsigned long long>(post_unsub_deliveries.load()));
    }

    // ================================================================================================
    // (B) Block churn — each round genuinely parks/wakes threads (push_blocking_while / departure-wake
    // / ~LaneEntry() reclaim of whatever a parked-then-departed publish left queued), so far fewer
    // rounds. Proves S1's departure-wake and S2r's reclaim hold under real concurrency, not just the
    // single-threaded scenario in fanout_block_test.cpp.
    // ================================================================================================
    {
        constexpr std::uint64_t kRounds = 2'000;
        using FO = FanOut<Ev, OnSlowSubscriber<Block>>;

        FO fo(2, 512);  // tiny lane capacity -> publishers stall against the churn lane routinely
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> post_unsub_deliveries{0};

        auto lane_stable = fo.subscribe(ActorId{{1}, 0});
        check(lane_stable != nullptr, "stable subscribe", ok);

        auto publisher = [&] {
            std::uint64_t k = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                (void)fo.publish(Ev{k++});
                Ev out;
                for (int d = 0; d < 4; ++d) if (!lane_stable->try_pop(out)) break;
            }
        };
        std::thread p1(publisher);

        std::thread churn([&] {
            const ActorId cid{{7}, 99};
            for (std::uint64_t r = 0; r < kRounds; ++r) {
                auto lane = fo.subscribe(cid);
                std::this_thread::sleep_for(std::chrono::microseconds(50));  // let publishers park on it
                Ev out;
                for (int spin = 0; spin < 4; ++spin) if (!lane->try_pop(out)) break;
                fo.unsubscribe(cid);  // must wake any producer parked on `lane` (S1) then be quiescent
                while (lane->try_pop(out)) {}
                std::uint64_t extra = 0;
                while (lane->try_pop(out)) ++extra;
                post_unsub_deliveries.fetch_add(extra, std::memory_order_relaxed);
                lane.reset();
            }
            stop.store(true, std::memory_order_relaxed);
        });

        churn.join();
        p1.join();

        { Ev out; while (lane_stable->try_pop(out)) {} }
        lane_stable.reset();

        check(post_unsub_deliveries.load() == 0, "S1: no delivery after unsubscribe() returns (Block)", ok);
        const std::int64_t live = g_live.load();
        check(live == 0, "S2r: every payload reclaimed exactly once under Block churn (0 live at quiescence)", ok);
        check(fo.in_flight() == 0, "no publish in flight at quiescence", ok);
        std::fprintf(stderr, "  Block churn: live=%lld post_unsub=%llu\n", static_cast<long long>(live),
                     static_cast<unsigned long long>(post_unsub_deliveries.load()));
    }

    std::fprintf(stderr, "fanout_subscribe_race_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
