// Shared 012 `Store`-conformance harness for the durable adapters (FileStore/SqliteStore/RocksStore).
// It exercises the seam properties every backend must honour and, crucially, models a "crash" by
// asking the caller's factory to REOPEN the store on the same path — so an acknowledged write must
// still be there. Raw Store ops only (no Described types) so a single header is ODR-safe across the
// per-backend check TUs.
//
// `Make` is a callable returning a fresh Store instance bound to the SAME durable path each time it
// is invoked (so the second call is the "restart"). Returns 0 on success, 1 on any failed invariant.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark::testkit {

inline std::vector<std::byte> cbytes(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
}

template <class Make>
int run_store_conformance(const char* label, Make make) {
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) { std::fprintf(stderr, "  [%s] CHECK FAILED: %s\n", label, what); ok = false; }
    };
    const ActorId id{TypeKey{0xF11E}, 7};

    // 1) write, "crash" (destroy), reopen, verify durable.
    {
        auto fs = make();
        const FenceToken t = fs.acquire_fence(id);
        check(t == FenceToken{1}, "first fence token is 1");
        check(fs.append(id, t, 1, cbytes("event-one")).has_value(), "append seq 1");
        check(fs.append(id, t, 2, cbytes("event-two")).has_value(), "append seq 2");
        check(fs.append(id, t, 3, cbytes("event-three")).has_value(), "append seq 3");
        SnapshotRecord snap;
        snap.through_seq = 2;
        snap.record = cbytes("snap@2");
        check(fs.save_snapshot(id, t, snap).has_value(), "save snapshot through_seq=2");
    }
    {
        auto fs = make();  // reopen
        check(fs.current_owner(id) == FenceToken{1}, "owner survived reopen");
        check(fs.last_seq(id) == 3, "last_seq survived reopen");
        auto s = fs.load_snapshot(id);
        check(s.has_value() && s->has_value(), "snapshot present after reopen");
        check(s.has_value() && s->has_value() && (*s)->through_seq == 2, "snapshot through_seq durable");
        check(s.has_value() && s->has_value() && (*s)->record == cbytes("snap@2"), "snapshot bytes durable");
        auto cur = fs.read_log(id, 1);
        check(cur.has_value() && cur->size() == 3, "all 3 events durable");
        check(cur.has_value() && cur->size() == 3 && cur->entries[0].record == cbytes("event-one"),
              "event 1 bytes durable");
        check(cur.has_value() && cur->size() == 3 && cur->entries[2].record == cbytes("event-three"),
              "event 3 bytes durable");

        // 2) fencing durable across reopen.
        const FenceToken t2 = fs.acquire_fence(id);
        check(t2 == FenceToken{2}, "new owner strictly greater than the durable token");
        auto stale = fs.append(id, FenceToken{1}, 4, cbytes("stale"));
        check(!stale.has_value() && stale.error().code == errc::unavailable, "stale writer fenced out");
        check(fs.last_seq(id) == 3, "stale append did not land");

        // 3) append_batch all-or-none.
        std::vector<EventRecord> bad{EventRecord{4, cbytes("ok")}, EventRecord{4, cbytes("dup")}};
        auto rc = fs.append_batch(id, t2, std::span<const EventRecord>(bad));
        check(!rc.has_value(), "non-monotonic batch rejected");
        check(fs.last_seq(id) == 3, "rejected batch appended nothing (atomic)");
        std::vector<EventRecord> good{EventRecord{4, cbytes("g4")}, EventRecord{5, cbytes("g5")}};
        check(fs.append_batch(id, t2, std::span<const EventRecord>(good)).has_value(), "good batch commits");
        check(fs.last_seq(id) == 5, "good batch atomically appended both");
    }
    {
        auto fs = make();  // reopen once more — batch + bumped owner durable.
        check(fs.current_owner(id) == FenceToken{2}, "bumped owner durable");
        check(fs.last_seq(id) == 5, "batch last_seq durable");
    }

    std::printf("%s: %s\n", label, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace quark::testkit
