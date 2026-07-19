// Tests 006 §Publish/Subscribe / ADR-019 GATE 2 (at-most-once, counted drops) + GATE 3
// (per-(publisher,subscriber) FIFO) + ActorId set-semantics dedup. Single-thread deterministic: one
// publisher fans K messages over N subscribers with generous inboxes; every subscriber receives each
// message exactly once, in monotone publish order (0 inversions); a double-subscribe yields ONE
// delivery.
#include <cstdint>
#include <cstdio>
#include <vector>
#include <memory>

#include "quark/core/topic.hpp"

using namespace quark;

namespace {
struct Ev {
    std::uint64_t seq;
    std::uint64_t check;
};
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
constexpr std::uint64_t kMix = 2654435761u;
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint32_t kN = 16;      // subscribers
    constexpr std::uint64_t kK = 100'000; // messages

    Topic<Ev> topic;
    std::vector<std::unique_ptr<BoundedInbox<Ev>>> inboxes;
    for (std::uint32_t i = 0; i < kN; ++i) {
        inboxes.push_back(std::make_unique<BoundedInbox<Ev>>(1024));
        check(topic.subscribe(ActorId{{1}, i}, inboxes[i].get()), "subscribe", ok);
    }

    // Set-dedup: a second subscribe of an existing ActorId is rejected (one delivery, GATE 2).
    check(!topic.subscribe(ActorId{{1}, 0}, inboxes[0].get()), "double-subscribe is idempotent (set-dedup)", ok);
    check(topic.subscriber_count() == kN, "subscriber count unchanged by double-subscribe", ok);

    std::vector<std::uint64_t> next_expected(kN, 0);
    std::vector<std::uint64_t> received(kN, 0);
    bool order_ok = true, check_ok = true;

    // Interleave publish + drain so inboxes never overflow (this test is about correctness, not
    // backpressure — GATE 2/3, not GATE 1).
    for (std::uint64_t k = 0; k < kK; ++k) {
        auto r = topic.publish(Ev{k, k ^ kMix});
        check_ok = check_ok && (r.delivered == kN && r.dropped_full == 0);
        // drain everyone
        for (std::uint32_t i = 0; i < kN; ++i) {
            Ev out;
            while (Topic<Ev>::consume(*inboxes[i], out)) {
                if (out.seq != next_expected[i]) order_ok = false;  // per-(pub,sub) FIFO
                if (out.check != (out.seq ^ kMix)) check_ok = false;
                ++next_expected[i];
                ++received[i];
            }
        }
    }

    for (std::uint32_t i = 0; i < kN; ++i)
        check(received[i] == kK, "each subscriber received every message exactly once (at-most-once, no loss)", ok);
    check(order_ok, "per-(publisher,subscriber) FIFO — 0 inversions", ok);
    check(check_ok, "no torn payloads; delivered==N, 0 drops with roomy inboxes", ok);

    std::fprintf(stderr, "topic_at_most_once_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
