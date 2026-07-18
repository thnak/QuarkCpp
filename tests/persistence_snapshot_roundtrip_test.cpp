// Tests 012-Persistence §Snapshot round-trip: an actor's state, captured at a consistent point
// reached via quiesce(Drain) (015), serialized to a 016 durable record, then recovered into a FRESH
// instance, reconstructs BIT-IDENTICAL state. The consistent point is demonstrated on a real
// Activation — quiesce(Drain) resolves synchronously on a Sequential actor (begin_quiesce true) and
// snapshot_sequential drives exactly that ready path.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

namespace {

// The serialized state of a persistent Account actor (non-trivial: int + string + vector).
struct AccountState {
    std::uint64_t balance = 0;
    std::string owner;
    std::vector<std::uint64_t> txns;
};
QUARK_SERIALIZE(AccountState, (1, balance), (2, owner), (3, txns))

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

bool operator==(const AccountState& a, const AccountState& b) {
    return a.balance == b.balance && a.owner == b.owner && a.txns == b.txns;
}

}  // namespace

int main() {
    bool ok = true;
    InMemoryStore store;
    const ActorId id{TypeKey{0xACC0}, 42};

    // A live activation is used ONLY to reach the quiescent snapshot point (Sequential ⇒ synchronous).
    Activation act(nullptr, DispatchTable{});
    const FenceToken fence = store.acquire_fence(id);

    // The "live" actor state at snapshot time.
    AccountState live;
    live.balance = 123456;
    live.owner = "iot-viet-solution";
    live.txns = {10, 20, 30, 40, 50};

    // Capture at the consistent point via quiesce(Drain); through_seq=0 (Snapshot model, no log).
    auto saved = snapshot_sequential<AccountState>(act, store, id, fence, /*through_seq=*/0, live);
    check(saved.has_value(), "snapshot_sequential persists under the fence", ok);
    check(store.load_snapshot(id).value().has_value(), "store holds a snapshot after capture", ok);

    // Recover into a FRESH instance — simulates a restart / re-placement loading durable state.
    auto recovered = recover_snapshot<AccountState>(store, id, AccountState{});
    check(recovered.has_value(), "recover_snapshot succeeds", ok);
    check(recovered->state == live, "recovered state is bit-identical to the snapshotted state", ok);
    check(recovered->fence == fence, "recovered snapshot carries the writing fence token", ok);

    // A never-snapshotted actor recovers to the seeded initial state (nullopt path), not an error.
    const ActorId fresh_id{TypeKey{0xACC0}, 99};
    AccountState seed{7, "seed", {}};
    auto fresh = recover_snapshot<AccountState>(store, fresh_id, seed);
    check(fresh.has_value() && fresh->state == seed && fresh->last_seq == 0,
          "no-snapshot actor recovers to the seeded initial state", ok);

    // A second snapshot overwrites the latest state (latest-state model).
    live.balance = 999;
    live.txns.push_back(60);
    auto again = snapshot_sequential<AccountState>(act, store, id, fence, 0, live);
    check(again.has_value(), "second snapshot overwrites", ok);
    auto r2 = recover_snapshot<AccountState>(store, id, AccountState{});
    check(r2.has_value() && r2->state == live, "recovery returns the LATEST snapshot", ok);

    std::printf("persistence_snapshot_roundtrip_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
