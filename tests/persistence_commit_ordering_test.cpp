// Tests 012-Persistence §"commit sequence number" + ADR-009 C7 staging fence:
//  (1) commit sequence numbers are STRICTLY INCREASING across commits (and across a multi-event
//      commit), and the store enforces it;
//  (2) replay honours seq order (a monotonically decoded fold reproduces the emission order);
//  (3) the STAGING FENCE: a handler that STAGES then THROWS commits NOTHING (rollback discards) →
//      durable log empty, reconstructed state 0; the normal path commits [100].
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"

using namespace quark;

namespace {

struct Balance {
    std::int64_t v = 0;
};
QUARK_SERIALIZE(Balance, (1, v))

struct Debit {
    std::int64_t amount = 0;
    std::uint64_t order = 0;  // emission order marker — replay must see these strictly increasing
};
QUARK_SERIALIZE(Debit, (1, amount), (2, order))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // ---- (1)+(2) strictly-increasing commit sequence + ordered replay -----------------------
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0xC0DE}, 1};
        const FenceToken f = store.acquire_fence(id);
        EventLog<Debit, InMemoryStore> log(store, id, f, store.last_seq(id) + 1);

        // Single-event commits: each takes the next seq (1,2,3,…).
        for (std::uint64_t i = 0; i < 20; ++i) {
            log.stage(Debit{static_cast<std::int64_t>(i), i});
            auto c = log.commit();
            check(c.has_value() && *c == i + 1, "single-event commit takes the next seq", ok);
        }
        // A MULTI-event commit: three events staged in one handler, committed together, each takes a
        // distinct strictly-increasing seq (21,22,23).
        log.stage(Debit{100, 20});
        log.stage(Debit{200, 21});
        log.stage(Debit{300, 22});
        auto multi = log.commit();
        check(multi.has_value() && *multi == 23, "multi-event commit ends at the last seq", ok);

        // The store's log is strictly increasing in seq AND the payload emission order matches.
        auto cur = store.read_log(id, 0);
        check(cur.has_value(), "read_log", ok);
        SeqNo prev = 0;
        std::uint64_t prev_order = 0;
        bool first = true;
        bool seq_mono = true, order_mono = true;
        for (const EventRecord& rec : *cur) {
            if (!first && !(rec.seq > prev)) seq_mono = false;
            auto ev = decode_record<Debit>(rec.record.data(), rec.record.size());
            check(ev.has_value(), "decode logged event", ok);
            if (!first && !(ev->order >= prev_order)) order_mono = false;
            prev = rec.seq;
            prev_order = ev->order;
            first = false;
        }
        check(seq_mono, "commit sequence numbers strictly increasing in the log", ok);
        check(order_mono, "replay reads events in emission order", ok);
        check(store.last_seq(id) == 23, "store last_seq == highest committed", ok);

        // The store REJECTS a non-increasing seq (a caller bug) — proving the invariant is enforced,
        // not merely respected by the EventLog.
        std::byte dummy[1]{};
        auto bad = store.append(id, f, /*seq=*/10, std::span<const std::byte>(dummy, 0));
        check(!bad && bad.error().code == errc::internal, "store rejects a non-increasing seq", ok);
    }

    // ---- (3) staging fence: stage-then-throw commits nothing (ADR-009 C7) -------------------
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0xC0DE}, 2};
        const FenceToken f = store.acquire_fence(id);
        EventLog<Debit, InMemoryStore> log(store, id, f, store.last_seq(id) + 1);

        // POISON handler: stage debit(100) … then throw → rollback (commit nothing).
        log.stage(Debit{100, 0});
        check(log.staged_count() == 1, "event staged (buffered, not durable)", ok);
        // Simulate the handler-boundary throw: the engine's drain_step would call rollback() (the
        // reported commit hook). Nothing is appended.
        log.rollback();
        check(log.staged_count() == 0, "rollback discards the staging buffer", ok);
        check(store.log_size(id) == 0, "durable log EMPTY after a throwing handler", ok);
        check(log.next_seq() == 1, "a rolled-back commit consumes no sequence number", ok);

        // Reconstruct: replay the (empty) log → balance 0 (no partial pre-throw append resurrected).
        auto rec = recover_event_sourced<Balance, Debit>(
            store, id, Balance{}, [](Balance& b, const Debit& d) { b.v -= d.amount; });
        check(rec.has_value() && rec->state.v == 0, "reconstructed balance 0 (poison committed nothing)", ok);

        // NORMAL path on the same actor: stage debit(100) then commit → log [100], balance -100.
        log.stage(Debit{100, 0});
        auto c = log.commit();
        check(c.has_value() && *c == 1, "normal commit succeeds at seq 1", ok);
        check(store.log_size(id) == 1, "durable log has exactly the one committed event", ok);
        auto rec2 = recover_event_sourced<Balance, Debit>(
            store, id, Balance{}, [](Balance& b, const Debit& d) { b.v -= d.amount; });
        check(rec2.has_value() && rec2->state.v == -100, "normal path reconstructs balance -100", ok);
    }

    std::printf("persistence_commit_ordering_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
