// Tests 006 §Publish/Subscribe (ordered + reliable fan-out) / ADR-039 C1 (per-(publisher,subscriber)
// FIFO, 0 inversion/duplication while attached) + C2 (EvictAfter gap signal exactly-once, never
// silent at the FanOut boundary) for `FanOut<M, OnSlowSubscriber<EvictAfter<N>>>`. Single-thread
// deterministic: keep-up subscribers must see every message with 0 inversions/evictions; a
// deliberately lagging subscriber must be evicted exactly `published - delivered` times, and every
// message it DOES receive must still be in monotone id order (eviction drops the newest push that
// doesn't fit — it never reorders or duplicates what's already queued).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/fanout.hpp"

using namespace quark;

namespace {
struct Ev {
    std::uint64_t seq;
    std::uint64_t check;
};
constexpr std::uint64_t kMix = 2654435761u;
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;

    // ---- (A) keep-up subscribers: 0 evictions, 0 inversions, every message exactly once -----------
    {
        constexpr std::uint32_t kN = 8;
        constexpr std::uint64_t kK = 50'000;
        using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<1024>>>;
        FO fo;
        std::vector<std::shared_ptr<FO::LaneEntry>> lanes;
        for (std::uint32_t i = 0; i < kN; ++i) {
            auto lane = fo.subscribe(ActorId{{1}, i});
            check(lane != nullptr, "subscribe", ok);
            lanes.push_back(lane);
        }
        std::vector<std::uint64_t> next_expected(kN, 0), received(kN, 0);
        bool order_ok = true, check_ok = true;
        for (std::uint64_t k = 0; k < kK; ++k) {
            auto r = fo.publish(Ev{k, k ^ kMix});
            check_ok = check_ok && (r.delivered == kN && r.evicted == 0);
            for (std::uint32_t i = 0; i < kN; ++i) {
                Ev out;
                while (lanes[i]->try_pop(out)) {
                    if (out.seq != next_expected[i]) order_ok = false;
                    if (out.check != (out.seq ^ kMix)) check_ok = false;
                    ++next_expected[i];
                    ++received[i];
                }
            }
        }
        for (std::uint32_t i = 0; i < kN; ++i) {
            check(received[i] == kK, "keep-up subscriber received every message exactly once", ok);
            check(lanes[i]->evicted() == 0, "keep-up subscriber never evicted", ok);
        }
        check(order_ok, "C1: per-(publisher,subscriber) FIFO — 0 inversions", ok);
        check(check_ok, "no torn payloads; delivered==N, 0 evictions with roomy lanes", ok);
    }

    // ---- (B) a genuinely lagging subscriber: exactly-once gap signal, remaining FIFO --------------
    {
        constexpr std::uint32_t kCap = 64;
        constexpr std::uint64_t kK = 200'000;
        using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<kCap>>>;
        FO fo;
        auto keepup = fo.subscribe(ActorId{{1}, 1});
        auto lagger = fo.subscribe(ActorId{{1}, 2});
        check(keepup && lagger, "subscribe", ok);

        std::uint64_t next_keepup = 0, keepup_recv = 0;
        std::uint64_t next_lagger_floor = 0, lagger_recv = 0;
        std::uint64_t total_evicted = 0;
        bool order_ok = true;

        for (std::uint64_t k = 0; k < kK; ++k) {
            auto r = fo.publish(Ev{k, k ^ kMix});
            total_evicted += r.evicted;
            // Drain the keep-up lane every iteration (never evicted).
            Ev out;
            while (keepup->try_pop(out)) {
                if (out.seq != next_keepup) order_ok = false;
                ++next_keepup;
                ++keepup_recv;
            }
            // Drain the lagging lane only every 37th publish, and only a little — deliberately slower
            // than the publish rate so it repeatedly fills and gets evicted.
            if (k % 37 == 0) {
                for (int d = 0; d < 5 && lagger->try_pop(out); ++d) {
                    if (out.seq < next_lagger_floor) order_ok = false;  // no going backwards
                    next_lagger_floor = out.seq + 1;
                    ++lagger_recv;
                }
            }
        }
        // Final drain of both.
        Ev out;
        while (keepup->try_pop(out)) {
            if (out.seq != next_keepup) order_ok = false;
            ++next_keepup;
            ++keepup_recv;
        }
        while (lagger->try_pop(out)) {
            if (out.seq < next_lagger_floor) order_ok = false;
            next_lagger_floor = out.seq + 1;
            ++lagger_recv;
        }

        check(keepup_recv == kK, "keep-up lane received every message despite a lagging sibling", ok);
        check(keepup->evicted() == 0, "keep-up lane unaffected by the lagging sibling's evictions", ok);
        check(order_ok, "C1: FIFO holds for both lanes (lagging lane never goes backwards)", ok);
        check(lagger_recv > 0 && lagger_recv < kK, "lagging lane fell behind (received some, not all)", ok);
        check(lagger->evicted() > 0, "lagging lane was evicted at least once", ok);
        check(lagger->evicted() + lagger_recv == kK,
              "C2: every published message is EITHER delivered XOR evicted for the lagging lane, "
              "exactly-once accounting at the FanOut boundary",
              ok);
        check(total_evicted == lagger->evicted(),
              "C2: publish()'s per-call receipt sum matches the lane's own exactly-once counter", ok);
        std::fprintf(stderr, "  lagger: received=%llu evicted=%llu (of %llu published)\n",
                     static_cast<unsigned long long>(lagger_recv),
                     static_cast<unsigned long long>(lagger->evicted()),
                     static_cast<unsigned long long>(kK));
    }

    // ---- (C) EvictAfter never removes the lane from membership; unsubscribe does -------------------
    {
        using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<4>>>;
        FO fo;
        auto lane = fo.subscribe(ActorId{{1}, 1});
        for (int i = 0; i < 20; ++i) fo.publish(Ev{static_cast<std::uint64_t>(i), 0});
        check(fo.subscriber_count() == 1, "a lagging/evicting lane stays subscribed (not ejected)", ok);
        check(fo.unsubscribe(ActorId{{1}, 1}), "unsubscribe still works after evictions", ok);
        check(fo.subscriber_count() == 0, "unsubscribe removes membership", ok);
    }

    std::fprintf(stderr, "fanout_evict_after_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
