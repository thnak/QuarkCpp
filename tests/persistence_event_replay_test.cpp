// Tests 012-Persistence §EventSourced recovery: append N state-changing events to the log, then
// recover a FRESH instance by REPLAYING the log — reconstructed state matches the online state
// exactly. Also proves: (a) a snapshot checkpoint bounds replay length (events before through_seq
// are folded into the snapshot, not re-read), and (b) 016 migration folds an OLDER event shape
// forward on replay.
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

namespace {

// The reconstructed state: a running balance + a count of applied events.
struct Ledger {
    std::int64_t balance = 0;
    std::uint64_t applied = 0;
};
QUARK_SERIALIZE(Ledger, (1, balance), (2, applied))

// A state-changing event (current schema, v1): a signed delta.
struct Moved {
    std::int64_t delta = 0;
};
QUARK_SERIALIZE(Moved, (1, delta))

// The deterministic fold — the SAME apply used online and during replay.
void apply(Ledger& l, const Moved& m) {
    l.balance += m.delta;
    ++l.applied;
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// --- Migration fixture: an OLD event shape (v1) upcast to a NEW one (v2) on replay --------------
struct MovedV1 {
    std::int32_t delta = 0;  // old: 32-bit
};
struct MovedV2 {
    std::int64_t delta = 0;  // new: widened to 64-bit
    std::uint64_t reason = 0;
};
// QUARK_SERIALIZE must sit in the type's namespace so the generated quark_describe is found by ADL.
QUARK_SERIALIZE(MovedV1, (1, delta))
QUARK_SERIALIZE(MovedV2, (1, delta), (2, reason))

void apply2(Ledger& l, const MovedV2& m) {
    l.balance += m.delta;
    ++l.applied;
}

}  // namespace

// SchemaVersion/Migration specialise types in namespace quark — these belong at global scope.
QUARK_SCHEMA_VERSION(MovedV1, 1);
QUARK_SCHEMA_VERSION(MovedV2, 2);
QUARK_MIGRATE(MovedV1, MovedV2, [](const MovedV1& o) {
    return MovedV2{static_cast<std::int64_t>(o.delta), /*reason=*/0};
});

int main() {
    bool ok = true;
    InMemoryStore store;
    const ActorId id{TypeKey{0x1ED6E7}, 7};

    // --- ONLINE: an EventSourced actor stages+commits N events -------------------------------
    const FenceToken f1 = store.acquire_fence(id);
    EventLog<Moved, InMemoryStore> log(store, id, f1, /*next_seq=*/store.last_seq(id) + 1);

    Ledger online;
    constexpr int N = 64;
    for (int i = 0; i < N; ++i) {
        const std::int64_t delta = (i % 2 == 0) ? (i + 1) : -(i + 1);
        log.stage(Moved{delta});      // stage within the "handler"
        apply(online, Moved{delta});  // online state advances with the same fold
        auto c = log.commit();        // handler-completion commit → durable append
        check(c.has_value(), "commit succeeds", ok);
    }
    check(store.log_size(id) == static_cast<std::size_t>(N), "N events durable in the log", ok);

    // --- RECOVERY: fresh instance replays the log from empty state ----------------------------
    auto rec = recover_event_sourced<Ledger, Moved>(store, id, Ledger{}, apply);
    check(rec.has_value(), "recover_event_sourced succeeds", ok);
    check(rec->state.balance == online.balance && rec->state.applied == online.applied,
          "replayed state == online state", ok);
    check(rec->last_seq == static_cast<SeqNo>(N), "recovery reports the last committed sequence", ok);

    // --- SNAPSHOT bounds replay: checkpoint the state, then only the TAIL replays --------------
    // Write a snapshot through the current seq; subsequent recovery must fold the snapshot and read
    // ZERO tail events (log entries all have seq <= through_seq).
    auto snap_bytes = encode_durable(online);
    check(snap_bytes.has_value(), "encode snapshot", ok);
    SnapshotRecord sr;
    sr.through_seq = static_cast<SeqNo>(N);
    sr.record = std::move(*snap_bytes);
    check(store.save_snapshot(id, f1, sr).has_value(), "snapshot checkpoint saved", ok);

    // Append 3 more tail events after the checkpoint.
    Ledger online2 = online;
    for (int i = 0; i < 3; ++i) {
        log.stage(Moved{100});
        apply(online2, Moved{100});
        (void)log.commit();
    }
    auto rec2 = recover_event_sourced<Ledger, Moved>(store, id, Ledger{}, apply);
    check(rec2.has_value(), "recover after checkpoint succeeds", ok);
    check(rec2->state.balance == online2.balance && rec2->state.applied == online2.applied,
          "snapshot + tail replay == online state", ok);
    check(rec2->last_seq == static_cast<SeqNo>(N + 3), "last_seq advances past the checkpoint", ok);

    // --- MIGRATION on replay: append an OLD (v1) event, recover as the NEW (v2) shape ----------
    InMemoryStore store2;
    const ActorId id2{TypeKey{0x1ED6E8}, 1};
    const FenceToken f2 = store2.acquire_fence(id2);
    // Write a v1 record directly (as an older binary would have), then replay as v2.
    auto v1bytes = encode_durable(MovedV1{static_cast<std::int32_t>(-5)});
    check(v1bytes.has_value(), "encode a v1 event", ok);
    check(store2.append(id2, f2, 1, std::span<const std::byte>(*v1bytes)).has_value(),
          "append v1 event", ok);
    auto mrec = recover_event_sourced<Ledger, MovedV2, Ledger, MovedV1>(store2, id2, Ledger{}, apply2);
    check(mrec.has_value(), "recover migrates v1→v2 on replay", ok);
    check(mrec->state.balance == -5 && mrec->state.applied == 1,
          "migrated event folds forward correctly", ok);

    std::printf("persistence_event_replay_test: %s (online=%lld applied=%llu)\n", ok ? "OK" : "FAIL",
                static_cast<long long>(online.balance),
                static_cast<unsigned long long>(online.applied));
    return ok ? 0 : 1;
}
