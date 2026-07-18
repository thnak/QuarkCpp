// Quark sample 07 — Persistence: snapshots, event-sourcing, and fencing (012).
//
// Durability is opt-in and lives behind a `Store` seam. This sample uses the reference `InMemoryStore`
// (a logical fence gate + append-only log + latest snapshot) to show the three things that matter:
//
//   ACT 1 — SNAPSHOT model: capture an actor's whole state at a consistent point, recover it into a
//           FRESH instance bit-identically (a restart / re-placement reloading its state).
//   ACT 2 — EVENT-SOURCED model: append state-changing events to a log, then reconstruct current
//           state by REPLAYING them through the same deterministic fold used online.
//   ACT 3 — FENCING: a fence token stops a split-brain double-writer. When a newer activation takes
//           over, the older one's writes are refused (errc::unavailable) — no double-write.
//
// State is serialized with the 016 QUARK_SERIALIZE macro (field ids make it schema-evolution safe).
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 07_persistence
// Run  :  taskset -c 0-3 build/samples/07_persistence
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

// --- ACT 1 state: a non-trivial account (int + string + vector) --------------------------------
struct AccountState {
    std::uint64_t balance = 0;
    std::string owner;
    std::vector<std::uint64_t> txns;
};
QUARK_SERIALIZE(AccountState, (1, balance), (2, owner), (3, txns))
bool operator==(const AccountState& a, const AccountState& b) {
    return a.balance == b.balance && a.owner == b.owner && a.txns == b.txns;
}

// --- ACT 2 state + event: a ledger reconstructed by replaying signed deltas ---------------------
struct Ledger {
    std::int64_t balance = 0;
    std::uint64_t applied = 0;
};
QUARK_SERIALIZE(Ledger, (1, balance), (2, applied))  // State is snapshottable -> must be Described
struct Moved {
    std::int64_t delta;
};
QUARK_SERIALIZE(Moved, (1, delta))
void apply(Ledger& l, const Moved& m) { l.balance += m.delta; ++l.applied; }  // the deterministic fold

// --- ACT 3 state: minimal, just to exercise the fence ------------------------------------------
struct Counter {
    std::uint64_t v = 0;
};
QUARK_SERIALIZE(Counter, (1, v))

int main() {
    bool ok = true;

    // ===== ACT 1 — snapshot round-trip ==========================================================
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0xACC0}, 42};
        Activation act(nullptr, DispatchTable{});  // used only to reach the quiescent snapshot point
        const FenceToken fence = store.acquire_fence(id);

        AccountState live{123456, "iot-viet-solution", {10, 20, 30, 40, 50}};
        auto saved = snapshot_sequential<AccountState>(act, store, id, fence, /*through_seq=*/0, live);
        ok &= saved.has_value();

        // Recover into a brand-new instance (simulating a restart on another node).
        auto recovered = recover_snapshot<AccountState>(store, id, AccountState{});
        ok &= recovered.has_value() && recovered->state == live;
        std::printf("[snapshot]  recovered balance=%llu owner=%s txns=%zu  (bit-identical: %s)\n",
                    (unsigned long long)recovered->state.balance, recovered->state.owner.c_str(),
                    recovered->state.txns.size(), (recovered->state == live) ? "yes" : "NO");
    }

    // ===== ACT 2 — event-sourced replay =========================================================
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0x1ED9}, 7};
        const FenceToken f = store.acquire_fence(id);
        EventLog<Moved, InMemoryStore> log(store, id, f, /*next_seq=*/store.last_seq(id) + 1);

        Ledger online;
        constexpr int N = 64;
        for (int i = 0; i < N; ++i) {
            const std::int64_t delta = (i % 2 == 0) ? (i + 1) : -(i + 1);
            log.stage(Moved{delta});      // stage inside the "handler"
            apply(online, Moved{delta});  // advance online state with the same fold
            auto c = log.commit();        // handler-completion commit -> durable append
            ok &= c.has_value();
        }

        // Reconstruct from an EMPTY ledger by replaying the durable log.
        auto rec = recover_event_sourced<Ledger, Moved>(store, id, Ledger{}, apply);
        ok &= rec.has_value() && rec->state.balance == online.balance &&
              rec->state.applied == online.applied;
        std::printf("[events]    committed %d events; replayed balance=%lld applied=%llu  (== online %lld: %s)\n",
                    N, (long long)rec->state.balance, (unsigned long long)rec->state.applied,
                    (long long)online.balance,
                    (rec->state.balance == online.balance) ? "yes" : "NO");
    }

    // ===== ACT 3 — fencing (split-brain protection) =============================================
    {
        InMemoryStore store;
        const ActorId id{TypeKey{0xF3AC}, 1};

        const FenceToken a = store.acquire_fence(id);   // activation A takes ownership (token 1)
        auto a_ok = save_snapshot<Counter>(store, id, a, /*through_seq=*/0, Counter{100});
        ok &= a_ok.has_value();

        const FenceToken b = store.acquire_fence(id);   // activation B takes over (token 2)
        // From here A is SUPERSEDED. Its writes must be refused; B's must succeed.
        auto a_stale = save_snapshot<Counter>(store, id, a, 0, Counter{999});
        auto b_ok = save_snapshot<Counter>(store, id, b, 0, Counter{200});
        const bool fenced = !a_stale.has_value() && a_stale.error().code == errc::unavailable;
        ok &= fenced && b_ok.has_value();
        std::printf("[fencing]   A=token%llu B=token%llu; stale A write refused: %s; B write ok: %s\n",
                    (unsigned long long)a.value, (unsigned long long)b.value,
                    fenced ? "yes (errc::unavailable)" : "NO (double-write!)",
                    b_ok.has_value() ? "yes" : "NO");
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
