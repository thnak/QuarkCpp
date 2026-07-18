// Tests 017-Delivery-Guarantees §"The mechanism" + §"Worked example" — the effectively-once EFFECT
// chain Client → Order → Inventory (both EffectivelyOnce), across the failure-mode table. The
// load-bearing invariant: the effect (durable state + outbound + reply) happens EXACTLY ONCE despite
//   * duplicate delivery of the client message,
//   * crash BEFORE commit (nothing durable → reprocess fresh),
//   * crash AFTER commit before sending (durable outbox re-drained; deterministic id dedup'd),
//   * the outbound `Reserve` delivered twice (retry → skip + re-ack).
// Determinism: no random / real time. `derive_outbound` is a PURE function; a replayed handler
// reproduces the SAME outbound id, which is what lets the downstream receiver recognize the copy.
#include <cstdint>
#include <cstdio>
#include <optional>

#include "quark/core/delivery.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

// --- The worked-example message + event vocabulary (all Described / 016 durable). ----------------
struct PlaceOrder {
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(PlaceOrder, (1, qty))

struct OrderAck {
    bool accepted = false;
};
QUARK_SERIALIZE(OrderAck, (1, accepted))

struct Reserved {  // Order's durable EFFECT (the state-changing event)
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Reserved, (1, qty))

struct Reserve {  // Order → Inventory outbound
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Reserve, (1, qty))

struct Stocked {  // Inventory's durable EFFECT
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Stocked, (1, qty))

struct ReserveAck {
    bool ok = false;
};
QUARK_SERIALIZE(ReserveAck, (1, ok))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Count durable DOMAIN-EVENT records of type `Ev` in an actor's log (the "durable effect" count) —
// skips watermark/outbox/reply records by matching the 016 header type_key.
template <Described Ev>
[[nodiscard]] std::size_t count_effects(InMemoryStore& store, ActorId id) {
    auto cur = store.read_log(id, 0);
    if (!cur) return 0;
    std::size_t n = 0;
    for (const EventRecord& r : *cur) {
        auto h = peek_record_header(r.record.data(), r.record.size());
        if (h && h->type == TypeKey{fingerprint_v<Ev>}) ++n;
    }
    return n;
}

// Stable sender identities (ActorId hashes) used as watermark keys.
constexpr ActorId client_id{TypeKey{0xC0}, 1};
constexpr ActorId order_id{TypeKey{0x0D}, 1};
constexpr ActorId inv_id{TypeKey{0x1D}, 1};

// An Inventory "node": a lane + a delivered-Reserve sink. Reused across scenarios so its watermark
// persists (the durable dedup state a real Inventory keeps). Returns whether it applied an effect.
struct Inventory {
    InMemoryStore& store;
    EffectivelyOnceLane<InMemoryStore> lane;
    std::size_t reserves_seen = 0;   // total Reserve messages the sink received (incl. duplicates)
    std::size_t acks_emitted = 0;

    explicit Inventory(InMemoryStore& s, FenceToken f)
        : store(s), lane(s, inv_id, f, s.last_seq(inv_id) + 1) {}

    // The post-commit dispatch SINK for Order's outbox: receives a durable OutboxRecord carrying a
    // Reserve, dedups by its DETERMINISTIC id, and commits Inventory's own effect exactly once.
    result<void> deliver(const OutboxRecord& o) {
        ++reserves_seen;
        auto msg = decode_durable<Reserve>(detail::blob_to_bytes(o.body));
        if (!msg) return std::unexpected(msg.error());
        if (lane.is_duplicate(o.out_sender, o.out_seq)) {
            // Duplicate Reserve (retry / crash re-drain) → skip effect, re-ack (idempotent).
            ++acks_emitted;
            return {};
        }
        auto c = lane.begin(o.out_sender, o.out_seq);
        if (auto rc = c.event(Stocked{msg->qty}); !rc) return std::unexpected(rc.error());
        if (auto rc = c.reply(ReserveAck{true}); !rc) return std::unexpected(rc.error());
        auto done = c.commit();
        if (!done) return std::unexpected(done.error());
        ++acks_emitted;
        return {};
    }
};

}  // namespace

int main() {
    bool ok = true;

    // =========================================================================================
    // (1) HAPPY PATH — Order commits {Reserved, watermark++, outbox:Reserve, reply}; the dispatcher
    //     sends Reserve; Inventory commits ONCE. One durable effect each, one Reserve.
    // =========================================================================================
    MsgKey reserve_id_happy{};
    {
        InMemoryStore store;
        Inventory inv(store, store.acquire_fence(inv_id));

        FenceToken f = store.acquire_fence(order_id);
        EffectivelyOnceLane<InMemoryStore> order(store, order_id, f, store.last_seq(order_id) + 1);

        const MsgKey inbound{client_id.hash(), 1};
        check(!order.is_duplicate(inbound), "happy: first client msg is not a duplicate", ok);

        auto c = order.begin(inbound);
        check(c.event(Reserved{7}).has_value(), "happy: stage Reserved effect", ok);
        auto oid = c.send(inv_id, Reserve{7});
        check(oid.has_value(), "happy: stage outbound Reserve", ok);
        reserve_id_happy = *oid;
        check(c.reply(OrderAck{true}).has_value(), "happy: stage reply", ok);
        auto rc = c.commit();
        check(rc.has_value(), "happy: atomic fenced commit accepted", ok);

        check(count_effects<Reserved>(store, order_id) == 1, "happy: exactly one durable Reserved", ok);
        check(order.watermark_of(client_id.hash()) == 1, "happy: watermark[client] advanced to 1", ok);
        check(order.outbox_pending() == 1, "happy: one pending outbox entry", ok);

        // Post-commit dispatch: deliver the Reserve to Inventory.
        auto sent = order.drain_outbox([&](const OutboxRecord& o) { return inv.deliver(o); });
        check(sent.has_value() && *sent == 1, "happy: dispatcher sent exactly one Reserve", ok);
        check(inv.reserves_seen == 1, "happy: Inventory received exactly one Reserve", ok);
        check(count_effects<Stocked>(store, inv_id) == 1, "happy: Inventory committed exactly one Stocked", ok);

        // =====================================================================================
        // (2) DUPLICATE DELIVERY of the client msg — same (sender, seq). seq <= watermark ⇒ skip
        //     the effect, re-emit the recorded reply. NO second Reserved, NO second Reserve.
        // =====================================================================================
        check(order.is_duplicate(inbound), "dup: second client copy recognized (seq <= watermark)", ok);
        auto cached = order.cached_reply(client_id.hash());
        check(cached.has_value(), "dup: recorded reply present for re-emit", ok);
        if (cached) {
            auto rep = EffectivelyOnceLane<InMemoryStore>::decode_reply<OrderAck>(*cached);
            check(rep.has_value() && rep->accepted, "dup: cached reply is the identical OrderAck", ok);
            check(cached->in_seq == 1, "dup: cached reply answers inbound seq 1", ok);
        }
        // The duplicate is skipped (no reprocessing) — effects unchanged.
        check(count_effects<Reserved>(store, order_id) == 1, "dup: still exactly one durable Reserved", ok);
        check(count_effects<Stocked>(store, inv_id) == 1, "dup: still exactly one Stocked downstream", ok);

        // =====================================================================================
        // (5) RESERVE DELIVERED TWICE (retry) — re-drain Order's durable outbox. Inventory sees the
        //     SAME deterministic id (seq <= watermark) ⇒ skips the effect, re-acks.
        // =====================================================================================
        auto again = order.drain_outbox([&](const OutboxRecord& o) { return inv.deliver(o); });
        check(again.has_value() && *again == 1, "retry: dispatcher re-sent the Reserve", ok);
        check(inv.reserves_seen == 2, "retry: Inventory received the Reserve a second time", ok);
        check(count_effects<Stocked>(store, inv_id) == 1,
              "retry: Inventory dedup'd by deterministic id — STILL one Stocked", ok);
    }

    // =========================================================================================
    // (3) ORDER CRASHES BEFORE COMMIT — the handler runs but never commits. Nothing durable. On
    //     recovery the retried client msg is reprocessed FRESH; the effect happens once.
    // =========================================================================================
    {
        InMemoryStore store;
        const MsgKey inbound{client_id.hash(), 1};

        // Activation A: handle, derive the outbound id, but CRASH before commit (drop the Commit).
        MsgKey derived_a{};
        {
            FenceToken fa = store.acquire_fence(order_id);
            EffectivelyOnceLane<InMemoryStore> a(store, order_id, fa, store.last_seq(order_id) + 1);
            auto c = a.begin(inbound);
            check(c.event(Reserved{9}).has_value(), "crash-pre: A stages effect", ok);
            auto oid = c.send(inv_id, Reserve{9});
            check(oid.has_value(), "crash-pre: A derives outbound id", ok);
            derived_a = *oid;
            // ... crash. No commit(). `c` is dropped.
        }
        check(store.log_size(order_id) == 0, "crash-pre: NOTHING durable after a crash before commit", ok);

        // Recovery: activation B acquires a fresh (higher) fence, rebuilds from the empty log.
        FenceToken fb = store.acquire_fence(order_id);
        EffectivelyOnceLane<InMemoryStore> b(store, order_id, fb, store.last_seq(order_id) + 1);
        check(b.recover([](std::span<const std::byte>) -> result<void> { return {}; }).has_value(),
              "crash-pre: recovery from empty log", ok);
        check(!b.is_duplicate(inbound), "crash-pre: the retried client msg is NOT a duplicate (fresh)", ok);

        auto c = b.begin(inbound);
        check(c.event(Reserved{9}).has_value(), "crash-pre: B re-stages the effect", ok);
        auto oid_b = c.send(inv_id, Reserve{9});
        check(oid_b.has_value(), "crash-pre: B re-derives outbound id", ok);
        // DETERMINISM: the replayed handler reproduces the SAME outbound id (pure derive).
        check(*oid_b == derived_a, "crash-pre: replayed handler reproduces the SAME outbound id", ok);
        check(c.reply(OrderAck{true}).has_value(), "crash-pre: B stages reply", ok);
        check(c.commit().has_value(), "crash-pre: B commits the effect", ok);
        check(count_effects<Reserved>(store, order_id) == 1, "crash-pre: effect happens EXACTLY ONCE", ok);
    }

    // =========================================================================================
    // (4) ORDER CRASHES AFTER COMMIT, BEFORE SENDING Reserve — the commit (effect + durable outbox)
    //     landed, but the dispatcher never ran. On recovery the durable outbox is re-drained and the
    //     Reserve is sent (with its deterministic id); Inventory commits once. Even if the outbound
    //     were also sent pre-crash, Inventory dedups the copy.
    // =========================================================================================
    {
        InMemoryStore store;
        const MsgKey inbound{client_id.hash(), 1};

        // Activation A: commit the effect + outbox, then CRASH before drain_outbox.
        MsgKey derived_a{};
        {
            FenceToken fa = store.acquire_fence(order_id);
            EffectivelyOnceLane<InMemoryStore> a(store, order_id, fa, store.last_seq(order_id) + 1);
            auto c = a.begin(inbound);
            check(c.event(Reserved{4}).has_value(), "crash-post: A stages effect", ok);
            auto oid = c.send(inv_id, Reserve{4});
            derived_a = *oid;
            check(c.reply(OrderAck{true}).has_value(), "crash-post: A stages reply", ok);
            check(c.commit().has_value(), "crash-post: A commits (effect + outbox durable)", ok);
            // ... crash BEFORE the dispatcher runs. Reserve NEVER sent by A.
        }
        check(count_effects<Reserved>(store, order_id) == 1, "crash-post: effect is durable", ok);

        // Recovery: activation B rebuilds watermark + outbox + reply cache from the WAL.
        FenceToken fb = store.acquire_fence(order_id);
        EffectivelyOnceLane<InMemoryStore> b(store, order_id, fb, store.last_seq(order_id) + 1);
        std::size_t domain_events_replayed = 0;
        check(b.recover([&](std::span<const std::byte>) -> result<void> {
                  ++domain_events_replayed;
                  return {};
              }).has_value(),
              "crash-post: recovery replays the WAL", ok);
        check(domain_events_replayed == 1, "crash-post: exactly one domain event replayed into state", ok);
        check(b.is_duplicate(inbound), "crash-post: recovered watermark recognizes the client msg", ok);
        check(b.outbox_pending() == 1, "crash-post: the durable outbox survived the crash", ok);
        check(b.outbox().front().out_seq == derived_a.seq,
              "crash-post: re-drained outbox carries the SAME deterministic id", ok);

        // Inventory (fresh node here) receives the re-drained Reserve.
        Inventory inv(store, store.acquire_fence(inv_id));
        auto sent = b.drain_outbox([&](const OutboxRecord& o) { return inv.deliver(o); });
        check(sent.has_value() && *sent == 1, "crash-post: recovery re-drains the outbox → Reserve sent", ok);
        check(count_effects<Stocked>(store, inv_id) == 1, "crash-post: Inventory effect once", ok);

        // Deliver it AGAIN (model: A had actually sent it before crashing) → dedup by deterministic id.
        auto dup = b.drain_outbox([&](const OutboxRecord& o) { return inv.deliver(o); });
        check(dup.has_value() && *dup == 1, "crash-post: a second delivery is attempted", ok);
        check(count_effects<Stocked>(store, inv_id) == 1,
              "crash-post: deterministic-id dedup keeps Inventory at EXACTLY ONE Stocked", ok);
    }

    std::printf("delivery_effectively_once_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
