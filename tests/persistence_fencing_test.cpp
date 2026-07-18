// Tests 012-Persistence §"Placement, mobility, and fencing" + ADR-009: the fencing token stops a
// split-brain double-writer. A superseded (older-epoch) activation is REJECTED — it cannot append
// or snapshot after a newer activation took over, so a partitioned old owner cannot corrupt state
// after re-placement.
//
// The proof is a CONTRAST: writer A (fence=1) commits fine until writer B (fence=2) takes over; from
// that point A's writes are refused (errc::unavailable) while B's succeed. Without the fence the
// stale write would land (a double-write); the store's rejection is that fence firing.
#include <cstdint>
#include <cstdio>

#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

namespace {

struct Note {
    std::uint64_t v = 0;
};
QUARK_SERIALIZE(Note, (1, v))

struct State {
    std::uint64_t sum = 0;
};
QUARK_SERIALIZE(State, (1, sum))

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
    const ActorId id{TypeKey{0xFECE}, 1};

    // --- Activation A acquires the first fencing token and writes durably --------------------
    const FenceToken a = store.acquire_fence(id);
    check(a == FenceToken{1}, "first activation gets fence token 1", ok);
    EventLog<Note, InMemoryStore> log_a(store, id, a, store.last_seq(id) + 1);
    log_a.stage(Note{10});
    check(log_a.commit().has_value(), "writer A commits under token 1", ok);
    check(store.log_size(id) == 1, "A's event is durable", ok);

    // --- Activation B takes over (re-placement / restart): a NEWER fencing token ------------
    const FenceToken b = store.acquire_fence(id);
    check(b == FenceToken{2}, "new activation gets a strictly-greater fence token 2", ok);
    check(store.current_owner(id) == b, "store now recognises B as the owner", ok);
    EventLog<Note, InMemoryStore> log_b(store, id, b, store.last_seq(id) + 1);
    log_b.stage(Note{20});
    check(log_b.commit().has_value(), "writer B commits under token 2", ok);
    check(store.log_size(id) == 2, "B's event is durable", ok);

    // --- The STALE writer A (partitioned old owner) tries to write again → FENCED -----------
    log_a.stage(Note{999});
    auto stale = log_a.commit();
    check(!stale && stale.error().code == errc::unavailable,
          "stale writer A (token 1) is fenced out of the log", ok);
    check(store.log_size(id) == 2, "the stale append did NOT land (no double-write)", ok);

    // A stale SNAPSHOT write is fenced too (the fence covers snapshots, not just the log).
    auto stale_snap = save_snapshot<State>(store, id, a, /*through_seq=*/2, State{999});
    check(!stale_snap && stale_snap.error().code == errc::unavailable,
          "stale writer A cannot snapshot either", ok);

    // The current owner B still writes fine — fencing rejects only the SUPERSEDED writer.
    log_b.stage(Note{30});
    check(log_b.commit().has_value(), "current owner B keeps writing after fencing A", ok);
    check(store.log_size(id) == 3, "B's later event lands", ok);
    check(save_snapshot<State>(store, id, b, 3, State{60}).has_value(),
          "current owner B can snapshot", ok);

    // A THIRD activation C fences BOTH A and B (monotonic tokens); B is now stale too.
    const FenceToken c = store.acquire_fence(id);
    check(c == FenceToken{3}, "third activation gets fence token 3", ok);
    log_b.stage(Note{40});
    auto b_after_c = log_b.commit();
    check(!b_after_c && b_after_c.error().code == errc::unavailable,
          "B (token 2) is fenced once C (token 3) takes over", ok);

    std::printf("persistence_fencing_test: %s (owner=%llu log=%zu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(store.current_owner(id).value),
                store.log_size(id));
    return ok ? 0 : 1;
}
