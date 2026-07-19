// Tests 006 §Publish/Subscribe / ADR-019 GATE 1 — the LOAD-BEARING gate: the publisher NEVER blocks
// or stalls on a slow / full / dead subscriber. A full inbox DROPS (counted, at-most-once), it does
// not back-pressure or delay the fan-out. Deterministic: fill some subscribers' inboxes so they are
// permanently full ("dead"/"slow"), leave others empty, and assert publish() (a) returns, (b) delivers
// to the empty ones, (c) counts drops for the full ones — and that a long run over a full set does NOT
// take materially longer than over an empty set (the fan-out is O(subscribers), stall-free).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/topic.hpp"

using namespace quark;

namespace {
struct Ev { std::uint64_t v; };
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;

    // ---- (A) correctness: a full + a dead subscriber drop; empty ones still get delivered ----------
    {
        Topic<Ev> topic;
        auto empty1 = std::make_unique<BoundedInbox<Ev>>(64);
        auto empty2 = std::make_unique<BoundedInbox<Ev>>(64);
        auto full = std::make_unique<BoundedInbox<Ev>>(4);   // small, will be filled and never drained
        auto dead = std::make_unique<BoundedInbox<Ev>>(4);   // filled and never drained (models a dead actor)
        topic.subscribe(ActorId{{1}, 1}, empty1.get());
        topic.subscribe(ActorId{{1}, 2}, full.get());
        topic.subscribe(ActorId{{1}, 3}, empty2.get());
        topic.subscribe(ActorId{{1}, 4}, dead.get());

        // Saturate `full` and `dead` (fill their 4 slots) by publishing 4 times without draining them,
        // draining only the empty ones between.
        for (int i = 0; i < 4; ++i) {
            auto r = topic.publish(Ev{static_cast<std::uint64_t>(i)});
            check(r.delivered >= 2, "empty subscribers keep receiving while others fill", ok);
            Ev out;
            while (Topic<Ev>::consume(*empty1, out)) {}
            while (Topic<Ev>::consume(*empty2, out)) {}
        }
        // Now `full` and `dead` are saturated. The next publish must DROP for them, DELIVER to the two
        // empty, and RETURN (no stall).
        auto r = topic.publish(Ev{999});
        check(r.delivered == 2, "GATE 1: delivered only to the 2 drainable subscribers", ok);
        check(r.dropped_full == 2, "GATE 1: the full + dead subscribers DROP (counted), not block", ok);
    }

    // ---- (B) no-stall timing: fan-out over an all-FULL set is not materially slower than all-EMPTY --
    {
        constexpr std::uint32_t kN = 64;
        constexpr std::uint64_t kIters = 200'000;

        auto run = [](bool saturate) {
            Topic<Ev> topic;
            std::vector<std::unique_ptr<BoundedInbox<Ev>>> inboxes;
            for (std::uint32_t i = 0; i < kN; ++i) {
                inboxes.push_back(std::make_unique<BoundedInbox<Ev>>(saturate ? 2 : 4096));
                topic.subscribe(ActorId{{1}, i}, inboxes[i].get());
            }
            if (saturate) {  // fill every inbox so every delivery drops
                for (int i = 0; i < 4; ++i) topic.publish(Ev{0});
            }
            auto t0 = std::chrono::steady_clock::now();
            std::uint64_t delivered = 0, dropped = 0;
            for (std::uint64_t k = 0; k < kIters; ++k) {
                auto r = topic.publish(Ev{k});
                delivered += r.delivered;
                dropped += r.dropped_full;
                if (!saturate) {  // drain so the empty run stays empty (no accidental fill)
                    for (auto& in : inboxes) { Ev out; while (Topic<Ev>::consume(*in, out)) {} }
                }
            }
            auto t1 = std::chrono::steady_clock::now();
            double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            return std::pair<double, std::pair<std::uint64_t, std::uint64_t>>{
                ns / static_cast<double>(kIters), {delivered, dropped}};
        };

        auto full_run = run(true);
        auto empty_run = run(false);
        check(full_run.second.second > 0, "saturated run actually dropped (subscribers were full)", ok);
        // The full run must NOT be dramatically slower — a Block-on-full design would spike here. A
        // generous 4x ceiling (the empty run also pays the drain loop, so this is conservative).
        double ratio = full_run.first / (empty_run.first > 1.0 ? empty_run.first : 1.0);
        std::fprintf(stderr, "  publish ns/op: full=%.1f empty=%.1f ratio=%.2f\n",
                     full_run.first, empty_run.first, ratio);
        check(ratio < 4.0, "GATE 1: full-set publish is not materially slower than empty-set (no stall)", ok);
    }

    std::fprintf(stderr, "topic_publisher_no_stall_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
