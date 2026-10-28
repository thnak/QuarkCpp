// Tests ADR-056 (006 §Publish/Subscribe): in-band gap notice for FanOut<EvictAfter<N>>. A subscriber
// that fell behind is told EXACTLY which per-publisher ids were dropped (first/last/count), at the
// exact position in its FIFO; trailing drops are reported by take_trailing_gap(); a healthy
// subscriber sees no gap and no loss; delivered + gap-covered ids partition the published ids.
// Includes a 2-thread stress (TSan-relevant: gap state is consumer-only, producer never touches it).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/fanout.hpp"

using namespace quark;
namespace {
struct Ev {
    std::uint64_t seq;
};
bool ok = true;
void check(bool c, const char* w) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", w);
        ok = false;
    }
}
using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<4>>>;
}  // namespace

int main() {
    // (1) deterministic: cap 4, publish 10 with no draining -> ids 0..3 queued, 4..9 dropped.
    {
        FO fo;
        auto lane = fo.subscribe(ActorId{{1}, 1});
        for (std::uint64_t i = 0; i < 10; ++i) (void)fo.publish(Ev{i});
        check(lane->evicted() == 6, "6 evicted");
        Ev e;
        FanOutGap g;
        for (std::uint64_t i = 0; i < 4; ++i) {
            check(lane->try_pop(e, g) && e.seq == i && !g, "head 0..3 in order, no gap");
        }
        check(!lane->try_pop(e, g), "empty");
        auto t = lane->take_trailing_gap();
        check(t && t.first == 4 && t.last == 9 && t.count == 6, "trailing gap 4..9 count 6");
        check(!lane->take_trailing_gap(), "trailing gap reported once");
        (void)fo.publish(Ev{10});
        check(lane->try_pop(e, g) && e.seq == 10 && !g, "no double-report after trailing take");
    }
    // (2) in-band gap in the middle, exact position.
    {
        FO fo;
        auto lane = fo.subscribe(ActorId{{1}, 1});
        for (std::uint64_t i = 0; i < 6; ++i) (void)fo.publish(Ev{i});  // 0..3 queued, 4,5 dropped
        Ev e;
        FanOutGap g;
        check(lane->try_pop(e, g) && e.seq == 0 && !g, "pop 0");
        (void)fo.publish(Ev{6});  // room for one -> queued
        for (std::uint64_t i = 1; i < 4; ++i) check(lane->try_pop(e, g) && e.seq == i && !g, "1..3");
        check(lane->try_pop(e, g) && e.seq == 6 && g.first == 4 && g.last == 5 && g.count == 2,
              "gap 4..5 immediately before 6");
        check(!lane->take_trailing_gap(), "no trailing gap");
        check(lane->evicted() == 2, "evicted counter agrees");
    }
    // (3) mid-stream attach: pre-attach history is not a gap; healthy peer unaffected by a slow one.
    {
        FO fo;
        for (int i = 0; i < 5; ++i) (void)fo.publish(Ev{static_cast<std::uint64_t>(i)});  // no subs
        auto slow = fo.subscribe(ActorId{{1}, 1});
        auto fast = fo.subscribe(ActorId{{1}, 2});
        Ev e;
        FanOutGap g;
        std::uint64_t fast_n = 0;
        for (std::uint64_t i = 5; i < 205; ++i) {
            (void)fo.publish(Ev{i});
            while (fast->try_pop(e, g)) {
                check(!g && e.seq == 5 + fast_n, "fast: no gap, in order");
                ++fast_n;
            }
        }
        check(fast_n == 200 && fast->evicted() == 0, "healthy subscriber lost nothing");
        check(slow->try_pop(e, g) && e.seq == 5 && !g, "slow baseline = first attached id");
    }
    // (4) two-thread stress: delivered ids + gap ranges partition 0..K-1 exactly.
    {
        constexpr std::uint64_t K = 2'000'000;
        FO fo;
        auto lane = fo.subscribe(ActorId{{1}, 1});
        std::atomic<bool> done{false};
        std::uint64_t delivered = 0, gapped = 0, next = 0;
        bool part_ok = true, started = false;
        std::thread cons([&] {
            Ev e;
            FanOutGap g;
            for (;;) {
                const bool fin = done.load(std::memory_order_acquire);
                bool any = false;
                while (lane->try_pop(e, g)) {
                    any = true;
                    if (!started) {
                        started = true;
                        next = e.seq;  // baseline = first delivered id
                    }
                    if (g) {
                        if (g.first != next || g.count != g.last - g.first + 1) part_ok = false;
                        gapped += g.count;
                        next = g.last + 1;
                    }
                    if (e.seq != next) part_ok = false;
                    ++delivered;
                    next = e.seq + 1;
                }
                if (auto t = lane->take_trailing_gap()) {
                    if (t.first != next) part_ok = false;
                    gapped += t.count;
                    next = t.last + 1;
                    any = true;
                }
                if (fin && !any) break;
                if ((delivered & 1023) == 1023) std::this_thread::yield();  // make it genuinely lag
            }
        });
        for (std::uint64_t i = 0; i < K; ++i) (void)fo.publish(Ev{i});
        done.store(true, std::memory_order_release);
        cons.join();
        std::printf("  stress: delivered=%llu gapped=%llu evicted=%llu\n", static_cast<unsigned long long>(delivered),
                    static_cast<unsigned long long>(gapped), static_cast<unsigned long long>(lane->evicted()));
        check(part_ok, "ranges contiguous, no overlap");
        check(delivered + gapped == K, "delivered + gapped == published (no silent loss)");
        check(gapped == lane->evicted(), "gap total == evicted counter");
        check(gapped > 0, "stress actually produced gaps");
    }
    std::printf(ok ? "fanout_gap_notice_test: PASS\n" : "fanout_gap_notice_test: FAIL\n");
    return ok ? 0 : 1;
}
