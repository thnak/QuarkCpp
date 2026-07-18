// Tests 012-Persistence §Snapshot — the store must not trust `through_seq`. A snapshot names the
// EXACT committed log prefix it subsumes, so `through_seq` may never run ahead of the actual appended
// tail. An over-advanced through_seq would make recovery skip un-replayed seqs (silent DATA LOSS)
// and jump last_seq past never-appended seqs (wedging future appends). save_snapshot must REJECT it
// and leave the store unchanged; a through_seq at-or-below the tail is accepted.
#include <cstdint>
#include <cstdio>

#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

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

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    InMemoryStore store;
    const ActorId id{TypeKey{0x5A11}, 1};
    const FenceToken f = store.acquire_fence(id);

    // Commit exactly ONE event → the appended tail is seq 1.
    EventLog<Debit, InMemoryStore> log(store, id, f, store.last_seq(id) + 1);
    log.stage(Debit{100});
    check(log.commit().has_value(), "one event committed", ok);
    check(store.log_size(id) == 1 && store.last_seq(id) == 1, "tail is at seq 1", ok);

    // A snapshot claiming through_seq = 5 (WAY past the seq-1 tail) must be rejected.
    auto sbytes = encode_durable(Balance{-100});
    check(sbytes.has_value(), "encode snapshot state", ok);
    SnapshotRecord ahead;
    ahead.through_seq = 5;
    ahead.record = *sbytes;
    auto rc = store.save_snapshot(id, f, ahead);
    check(!rc && rc.error().code == errc::internal, "through_seq past the tail is rejected", ok);

    // No data loss: the store is unchanged — no snapshot stored, tail still at seq 1, event intact.
    check(!store.load_snapshot(id).value().has_value(), "rejected snapshot did NOT land", ok);
    check(store.last_seq(id) == 1, "last_seq NOT advanced by the rejected snapshot", ok);
    check(store.log_size(id) == 1, "the committed event is still durable", ok);

    // Recovery still replays seq 1 correctly (the would-be data loss is averted).
    auto rec = recover_event_sourced<Balance, Debit>(
        store, id, Balance{}, [](Balance& b, const Debit& d) { b.v -= d.amount; });
    check(rec.has_value() && rec->state.v == -100 && rec->last_seq == 1,
          "recovery replays the event (no seqs silently skipped)", ok);

    // A well-formed snapshot exactly AT the tail (through_seq == 1) is accepted.
    SnapshotRecord atTail;
    atTail.through_seq = 1;
    atTail.record = *sbytes;
    auto rc2 = store.save_snapshot(id, f, atTail);
    check(rc2.has_value() && store.load_snapshot(id).value().has_value(),
          "a through_seq at the tail is accepted", ok);

    std::printf("persistence_snapshot_through_seq_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
