// Tests 012-Persistence §"commit sequence number" + ADR-009 C7 — the ALL-OR-NOTHING multi-event
// commit. A store whose ATOMIC batch append fails must leave NOTHING durable: the log stays empty,
// the EventLog's next_seq stays at the store tail (no regression that would wedge the log), and the
// staging buffer is intact so a RETRY on a healthy store commits the whole batch cleanly. This is
// the `torn.cpp` audit probe promoted to a committed test — before the fix a non-first append made
// earlier events durable (log_size==1) and the retry wedged with "sequence not strictly increasing".
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
};
QUARK_SERIALIZE(Debit, (1, amount))

// A Store that delegates to InMemoryStore but can be armed to fail the ATOMIC batch append,
// simulating a real disk/DB whose commit of a multi-event batch errors (disk-full, I/O error, short
// write). The whole batch fails or lands — the store never exposes a partial batch.
class FlakyStore {
public:
    InMemoryStore inner;
    bool fail_batch = false;

    FenceToken acquire_fence(ActorId id) { return inner.acquire_fence(id); }
    SeqNo last_seq(ActorId id) const noexcept { return inner.last_seq(id); }
    result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const { return inner.load_snapshot(id); }
    result<void> save_snapshot(ActorId id, FenceToken t, const SnapshotRecord& s) {
        return inner.save_snapshot(id, t, s);
    }
    result<void> append(ActorId id, FenceToken t, SeqNo seq, std::span<const std::byte> b) {
        return inner.append(id, t, seq, b);
    }
    result<void> append_batch(ActorId id, FenceToken t, std::span<const EventRecord> batch) {
        if (fail_batch) return fail(errc::internal, "simulated disk I/O error on batch append");
        return inner.append_batch(id, t, batch);
    }
    result<EventCursor> read_log(ActorId id, SeqNo from) const { return inner.read_log(id, from); }
    std::size_t log_size(ActorId id) const noexcept { return inner.log_size(id); }
};
static_assert(Store<FlakyStore>, "FlakyStore must model the 012 Store seam");

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // ---- (1) A failed batch commit leaves NOTHING durable and is retryable ----------------------
    {
        FlakyStore store;
        const ActorId id{TypeKey{0xBEEF}, 1};
        const FenceToken f = store.acquire_fence(id);
        EventLog<Debit, FlakyStore> log(store, id, f, store.last_seq(id) + 1);

        store.fail_batch = true;  // the store's atomic batch append will fail
        log.stage(Debit{100});
        log.stage(Debit{200});
        log.stage(Debit{300});
        auto rc = log.commit();

        check(!rc && rc.error().code == errc::internal, "failed commit reports the store error", ok);
        check(store.log_size(id) == 0, "durable log EMPTY after a failed commit (all-or-nothing)", ok);
        check(store.last_seq(id) == 0, "store tail unmoved after a failed commit", ok);
        check(log.next_seq() == store.last_seq(id) + 1,
              "EventLog next_seq stays at the store tail (no regression / no wedge)", ok);
        check(log.staged_count() == 3, "staging buffer intact after a failed commit (retryable)", ok);

        // ---- RETRY on a recovered store: the whole batch commits cleanly ------------------------
        store.fail_batch = false;
        auto rc2 = log.commit();
        check(rc2.has_value() && *rc2 == 3, "retry commits the whole 3-event batch (last seq 3)", ok);
        check(store.log_size(id) == 3, "all three events are now durable", ok);
        check(log.next_seq() == 4 && log.next_seq() == store.last_seq(id) + 1,
              "next_seq advances past the batch and matches the store tail", ok);
        check(log.staged_count() == 0, "staging buffer cleared after a successful commit", ok);

        // The durable events are exactly what was staged, in order.
        auto cur = store.read_log(id, 0);
        check(cur.has_value() && cur->size() == 3, "read_log returns the three events", ok);
        std::int64_t expect[] = {100, 200, 300};
        SeqNo eseq = 1;
        std::size_t i = 0;
        for (const EventRecord& r : *cur) {
            auto ev = decode_record<Debit>(r.record.data(), r.record.size());
            check(ev.has_value() && r.seq == eseq && ev->amount == expect[i],
                  "durable event matches the staged order/value", ok);
            ++eseq;
            ++i;
        }
    }

    // ---- (2) InMemoryStore::append_batch is itself atomic: a mid-batch bad seq rejects it all ---
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0xBEEF}, 2};
        const FenceToken f = store.acquire_fence(id);

        auto enc = [](std::int64_t a) { return *encode_durable(Debit{a}); };
        std::vector<EventRecord> batch;
        batch.push_back(EventRecord{1, enc(10)});
        batch.push_back(EventRecord{1, enc(20)});  // NOT strictly increasing — must reject the batch
        batch.push_back(EventRecord{2, enc(30)});
        auto rc = store.append_batch(id, f, std::span<const EventRecord>(batch));
        check(!rc && rc.error().code == errc::internal, "append_batch rejects a non-increasing seq", ok);
        check(store.log_size(id) == 0, "a rejected batch mutates NO durable state (validate-first)", ok);
        check(store.last_seq(id) == 0, "store tail unchanged after a rejected batch", ok);

        // A well-formed batch of the same events commits atomically.
        std::vector<EventRecord> good;
        good.push_back(EventRecord{1, enc(10)});
        good.push_back(EventRecord{2, enc(20)});
        auto rc2 = store.append_batch(id, f, std::span<const EventRecord>(good));
        check(rc2.has_value() && store.log_size(id) == 2 && store.last_seq(id) == 2,
              "a valid batch commits all events", ok);
    }

    std::printf("persistence_commit_atomicity_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
