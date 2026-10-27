// Tests 017-Delivery-Guarantees (#19, ADR-051): AtLeastOnce retry+ack over a LOSSY link, the durable
// outbox "delivered" marker surviving crash+recover, and bounded watermark/reply-cache maps.
// Deterministic: seeded xorshift PRNG drops ~30% of frames (data AND acks) in-process; no netem.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "quark/core/delivery.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
    bool drop() { return next() % 100 < 30; }
};

struct Ping {
    std::int64_t n = 0;
};
QUARK_SERIALIZE(Ping, (1, n))

constexpr ActorId owner_id{TypeKey{0x0A}, 7};

}  // namespace

int main() {
    bool ok = true;

    // (1) Lossy link: N senders x M messages, 30% loss on data and ack frames, receiver watermark cap
    //     smaller than the sender count (forces eviction + base re-admission).
    for (std::size_t cap : {std::size_t{0}, std::size_t{2}}) {
        Rng rng{0x9E3779B97F4A7C15ULL};
        constexpr int kSenders = 5, kMsgs = 120;
        std::vector<ReliableSender> senders;
        for (int i = 0; i < kSenders; ++i) senders.emplace_back(1000 + i);
        ReliableReceiver rx(cap);
        // (sender, seq) -> delivery count
        std::map<std::pair<std::uint64_t, std::uint64_t>, int> got;
        for (auto& s : senders)
            for (int m = 0; m < kMsgs; ++m) s.enqueue("m" + std::to_string(m));

        std::size_t dropped = 0, total = 0;
        int rounds = 0;
        auto all_acked = [&] {
            for (auto& s : senders) if (s.pending() != 0) return false;
            return true;
        };
        while (!all_acked() && rounds < 100000) {
            ++rounds;
            for (auto& s : senders) {
                s.pump([&](const ReliableFrame& f, const std::string&) {
                    ++total;
                    if (rng.drop()) { ++dropped; return; }
                    if (rx.accept(f) == ReliableReceiver::Verdict::Deliver) ++got[{f.sender, f.seq}];
                    ++total;
                    if (rng.drop()) { ++dropped; return; }  // ack lost
                    s.on_ack(rx.ack_for(f.sender));
                }, /*window=*/8);
            }
        }
        check(all_acked(), "lossy: every sender fully acked", ok);
        bool all_once = true;
        for (int i = 0; i < kSenders; ++i)
            for (int m = 1; m <= kMsgs; ++m) {
                auto it = got.find({std::uint64_t(1000 + i), std::uint64_t(m)});
                if (it == got.end() || it->second < 1) all_once = false;
            }
        check(all_once, "lossy: every message delivered >= 1 time", ok);
        check(dropped * 100 / total >= 20 && dropped * 100 / total <= 40, "lossy: ~30% of frames dropped", ok);
        if (cap != 0) {
            check(rx.tracked_senders() <= cap, "lossy: receiver watermark map stays within cap", ok);
            check(rx.evictions() > 0, "lossy: eviction path was exercised", ok);
        }
    }

    // (2) Delivered marker survives crash + recover.
    {
        InMemoryStore store;
        FenceToken f = *store.acquire_fence(owner_id);
        EffectivelyOnceLane<InMemoryStore> lane(store, owner_id, f, store.last_seq(owner_id) + 1);
        const ActorId target{TypeKey{0x0B}, 1};
        for (std::uint64_t i = 1; i <= 3; ++i) {
            auto c = lane.begin(MsgKey{50, i});
            check(c.send(target, Ping{std::int64_t(i)}).has_value(), "marker: stage outbound", ok);
            check(c.commit().has_value(), "marker: commit", ok);
        }
        check(lane.outbox_pending() == 3, "marker: three pending", ok);

        // Receiver acks only the FIRST record (the other acks are "lost").
        std::size_t calls1 = 0;
        const std::uint64_t first_seq = lane.outbox().front().out_seq;
        auto n = lane.deliver_outbox([&](const OutboxRecord& o) -> result<bool> {
            ++calls1;
            return o.out_seq == first_seq;
        });
        check(n.has_value() && *n == 1, "marker: one entry acked+marked", ok);
        check(calls1 == 3, "marker: all three were sent once", ok);
        check(lane.outbox_pending() == 2, "marker: two remain pending", ok);

        // drain_outbox no longer re-sends the delivered entry.
        auto d = lane.drain_outbox([&](const OutboxRecord&) -> result<void> { return {}; });
        check(d.has_value() && *d == 2, "marker: drain skips delivered entry", ok);

        // Crash: fresh lane over the same store; recover() rebuilds pending minus delivered.
        FenceToken f2 = *store.acquire_fence(owner_id);
        EffectivelyOnceLane<InMemoryStore> lane2(store, owner_id, f2, 1);
        check(lane2.recover([](std::span<const std::byte>) -> result<void> { return {}; }).has_value(),
              "marker: recover ok", ok);
        check(lane2.outbox_pending() == 2, "marker: delivered marker survived crash", ok);
        auto d2 = lane2.drain_outbox([&](const OutboxRecord&) -> result<void> { return {}; });
        check(d2.has_value() && *d2 == 2, "marker: recovered drain skips delivered entry", ok);

        // A zombie (old token) cannot mark delivered.
        auto z = lane.mark_delivered(lane.outbox().front().out_sender, lane.outbox().front().out_seq);
        check(!z.has_value(), "marker: fenced zombie cannot mark delivered", ok);
        check(lane2.mark_delivered(0xdead, 1).has_value(), "marker: unknown id is an idempotent no-op", ok);
    }

    // (3) Bounded watermark + reply-cache maps.
    {
        InMemoryStore store;
        FenceToken f = *store.acquire_fence(owner_id);
        EffectivelyOnceLane<InMemoryStore> lane(store, owner_id, f, 1);
        lane.set_limits(4, 3);
        for (std::uint64_t snd = 1; snd <= 10; ++snd) {
            auto c = lane.begin(MsgKey{snd, 1});
            check(c.reply(Ping{1}).has_value(), "bound: stage reply", ok);
            check(c.commit().has_value(), "bound: commit", ok);
        }
        check(lane.watermark_count() == 4, "bound: watermark map capped at 4", ok);
        check(lane.reply_cache_size() == 3, "bound: reply cache capped at 3", ok);
        check(lane.watermark_evictions() == 6 && lane.reply_cache_evictions() == 7, "bound: eviction counts", ok);
        check(lane.is_duplicate(10, 1), "bound: newest sender still deduped", ok);
        check(!lane.is_duplicate(1, 1), "bound: evicted sender no longer deduped (documented trade-off)", ok);
        check(lane.cached_reply(10).has_value() && !lane.cached_reply(1).has_value(), "bound: reply LRU", ok);

        // recover() re-applies the cap on a fresh lane.
        FenceToken f2 = *store.acquire_fence(owner_id);
        EffectivelyOnceLane<InMemoryStore> l2(store, owner_id, f2, 1);
        l2.set_limits(4, 3);
        check(l2.recover([](std::span<const std::byte>) -> result<void> { return {}; }).has_value(), "bound: recover", ok);
        check(l2.watermark_count() == 4 && l2.is_duplicate(10, 1), "bound: recovered map capped, newest kept", ok);
    }

    std::printf("delivery_at_least_once_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
