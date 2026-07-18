// Tests 017-Delivery-Guarantees §"Worked example" (the PARTITION row) — THE acceptance test the
// effectively-once open question demanded. Under partition, TWO `Order` activations both process the
// SAME client message and both attempt to commit. The `StateStore` accepts ONLY the higher fencing
// token's commit; the lower (zombie) activation's `append_batch` is rejected with `fenced_error`.
// Load-bearing assertions: the zombie produces NO state, NO durable outbox, and NO downstream
// `Reserve` — exactly ONE durable effect and exactly ONE `Reserve` reach the world, because commit
// is fenced AND output is post-commit (a fenced-out commit never makes its outbox durable, so the
// dispatcher never runs for the zombie). Fencing protects state AND output with one gate.
#include <cstdint>
#include <cstdio>

#include "quark/core/delivery.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

struct Reserved {
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Reserved, (1, qty))

struct Reserve {
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Reserve, (1, qty))

struct Stocked {
    std::int64_t qty = 0;
};
QUARK_SERIALIZE(Stocked, (1, qty))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

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

constexpr ActorId client_id{TypeKey{0xC0}, 1};
constexpr ActorId order_id{TypeKey{0x0D}, 1};
constexpr ActorId inv_id{TypeKey{0x1D}, 1};

// Run the partition with the two commit()s attempted in a given order; the outcome must be
// identical (higher token always wins) — order-independence is part of the invariant.
bool run_partition(bool low_commits_first) {
    bool ok = true;
    InMemoryStore store;

    const MsgKey inbound{client_id.hash(), 1};

    // Activation A (the eventual ZOMBIE) takes the fence first: token 1.
    FenceToken tok_a = store.acquire_fence(order_id);
    // Activation B (the TAKEOVER) is re-placed under partition and acquires a strictly higher token:
    // token 2. Acquiring it makes B the current owner — A is now fenced out.
    FenceToken tok_b = store.acquire_fence(order_id);
    check(tok_a < tok_b, "partition: B's fencing token strictly supersedes A's", ok);

    EffectivelyOnceLane<InMemoryStore> a(store, order_id, tok_a, store.last_seq(order_id) + 1);
    EffectivelyOnceLane<InMemoryStore> b(store, order_id, tok_b, store.last_seq(order_id) + 1);

    // BOTH activations handle the same message and derive the SAME deterministic outbound id.
    auto ca = a.begin(inbound);
    check(ca.event(Reserved{5}).has_value(), "partition: A stages effect", ok);
    auto oid_a = ca.send(inv_id, Reserve{5});

    auto cb = b.begin(inbound);
    check(cb.event(Reserved{5}).has_value(), "partition: B stages effect", ok);
    auto oid_b = cb.send(inv_id, Reserve{5});

    check(oid_a.has_value() && oid_b.has_value() && *oid_a == *oid_b,
          "partition: both activations derive the SAME outbound id (pure derivation)", ok);

    // Attempt both commits. The lower token is rejected (fenced_error); the higher is accepted —
    // regardless of the order in which they race.
    if (low_commits_first) {
        auto ra = ca.commit();
        check(!ra && ra.error().code == errc::unavailable,
              "partition: the LOW-token (zombie) commit is FENCED OUT", ok);
        auto rb = cb.commit();
        check(rb.has_value(), "partition: the HIGH-token commit is accepted", ok);
    } else {
        auto rb = cb.commit();
        check(rb.has_value(), "partition: the HIGH-token commit is accepted", ok);
        auto ra = ca.commit();
        check(!ra && ra.error().code == errc::unavailable,
              "partition: the LOW-token (zombie) commit is FENCED OUT", ok);
    }

    // EXACTLY ONE durable effect; the zombie wrote nothing.
    check(count_effects<Reserved>(store, order_id) == 1,
          "partition: exactly ONE durable Reserved effect (zombie contributed none)", ok);

    // The zombie has NO durable outbox — its in-memory outbox mirror only advances on a SUCCESSFUL
    // commit, so its dispatcher has nothing to send.
    check(a.outbox_pending() == 0, "partition: the zombie has NO durable outbox", ok);
    check(b.outbox_pending() == 1, "partition: the winner has exactly one outbox entry", ok);

    // Post-commit dispatch. The zombie sends NOTHING; the winner sends exactly one Reserve.
    std::size_t zombie_sent = 0;
    auto za = a.drain_outbox([&](const OutboxRecord&) -> result<void> {
        ++zombie_sent;
        return {};
    });
    check(za.has_value() && *za == 0 && zombie_sent == 0, "partition: the ZOMBIE sends NOTHING", ok);

    std::size_t reserves_delivered = 0;
    EffectivelyOnceLane<InMemoryStore> inv(store, inv_id, store.acquire_fence(inv_id),
                                           store.last_seq(inv_id) + 1);
    auto zb = b.drain_outbox([&](const OutboxRecord& o) -> result<void> {
        ++reserves_delivered;
        auto msg = decode_durable<Reserve>(detail::blob_to_bytes(o.body));
        if (!msg) return std::unexpected(msg.error());
        if (inv.is_duplicate(o.out_sender, o.out_seq)) return {};
        auto c = inv.begin(o.out_sender, o.out_seq);
        if (auto rc = c.event(Stocked{msg->qty}); !rc) return std::unexpected(rc.error());
        auto done = c.commit();
        if (!done) return std::unexpected(done.error());
        return {};
    });
    check(zb.has_value() && *zb == 1 && reserves_delivered == 1,
          "partition: exactly ONE downstream Reserve is delivered (from the winner)", ok);
    check(count_effects<Stocked>(store, inv_id) == 1,
          "partition: Inventory commits exactly ONE Stocked effect", ok);

    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    check(run_partition(/*low_commits_first=*/true), "partition (zombie commits first)", ok);
    check(run_partition(/*low_commits_first=*/false), "partition (winner commits first)", ok);
    std::printf("delivery_partition_fencing_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
