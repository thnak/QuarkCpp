// Shared 027 `ReminderStore`-conformance harness for the durable reminder adapters
// (FileReminderStore / SqliteReminderStore / RocksReminderStore). Like the 012 Store harness it models
// a "crash" by asking the caller's factory to REOPEN the store on the SAME path — so an acknowledged
// put/remove/checkpoint must still be there. Raw ReminderStore ops only (put/remove/load_all/
// checkpoint), so a single header is ODR-safe across the per-backend check TUs.
//
// `Make` is a callable returning a fresh store bound to the SAME durable path each time it is invoked
// (so the next call is the "restart"). Returns 0 on success, 1 on any failed invariant.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

namespace quark::testkit {

inline std::vector<std::byte> rem_bytes(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
}

inline const quark::ReminderRow* rem_find(const std::vector<quark::ReminderRow>& rows,
                                          quark::ActorId actor, const char* name) {
    for (const auto& r : rows)
        if (r.actor == actor && r.name == name) return &r;
    return nullptr;
}

template <class Make>
int run_reminder_store_conformance(const char* label, Make make) {
    using namespace quark;
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) { std::fprintf(stderr, "  [%s] CHECK FAILED: %s\n", label, what); ok = false; }
    };

    const ActorId a1{TypeKey{0xA1CE}, 1};
    const ActorId a2{TypeKey{0xA1CE}, 2};  // same type as a1, different instance
    const auto p1 = rem_bytes("charge-one");
    const auto p2 = rem_bytes("beat-two");
    const auto p3 = rem_bytes("charge-three");

    // 1) put three rows (two share the name "wake" across DIFFERENT actors — the key includes the
    //    actor, so both must survive), checkpoint(42), then "crash" (destroy).
    {
        auto s = make();
        check(s.put(ReminderRow{a1, "wake", 1000, 0,   p1}).has_value(), "put a1/wake (one-shot)");
        check(s.put(ReminderRow{a1, "beat", 2000, 500, p2}).has_value(), "put a1/beat (periodic)");
        check(s.put(ReminderRow{a2, "wake", 3000, 0,   p3}).has_value(), "put a2/wake (one-shot)");
        check(s.checkpoint(42).has_value(), "checkpoint(42)");
    }
    // 2) reopen — all three rows + checkpoint durable; fields intact; the two "wake" keys are distinct.
    {
        auto s = make();
        check(s.checkpoint_bucket() == 42, "checkpoint bucket durable across reopen");
        auto rows = s.load_all();
        check(rows.has_value(), "load_all after reopen");
        check(rows.has_value() && rows->size() == 3, "all 3 rows durable");
        if (rows) {
            const ReminderRow* r1 = rem_find(*rows, a1, "wake");
            const ReminderRow* rb = rem_find(*rows, a1, "beat");
            const ReminderRow* r2 = rem_find(*rows, a2, "wake");
            check(r1 && r1->scheduled_due_ns == 1000 && r1->period_ns == 0 && r1->payload == p1,
                  "a1/wake fields durable");
            check(rb && rb->scheduled_due_ns == 2000 && rb->period_ns == 500 && rb->payload == p2,
                  "a1/beat fields durable (periodic)");
            check(r2 && r2->scheduled_due_ns == 3000 && r2->payload == p3,
                  "a2/wake distinct from a1/wake (key includes actor)");
        }

        // 3) remove one, UPSERT another to a new due (must replace, not duplicate). No re-checkpoint.
        check(s.remove(ReminderKey{a1, reminder_name_hash("wake")}).has_value(), "remove a1/wake");
        check(s.put(ReminderRow{a1, "beat", 9999, 500, p2}).has_value(), "upsert a1/beat -> due 9999");
    }
    // 4) reopen — removed row gone, upsert replaced (not duplicated), untouched row intact, ckpt still 42.
    {
        auto s = make();
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == 2, "after remove+upsert: exactly 2 rows (no dup)");
        if (rows) {
            check(rem_find(*rows, a1, "wake") == nullptr, "removed a1/wake is gone");
            const ReminderRow* rb = rem_find(*rows, a1, "beat");
            check(rb && rb->scheduled_due_ns == 9999, "a1/beat upsert advanced durably");
            const ReminderRow* r2 = rem_find(*rows, a2, "wake");
            check(r2 && r2->payload == p3, "untouched a2/wake intact");
        }
        check(s.checkpoint_bucket() == 42, "checkpoint unchanged by remove/upsert");

        // 5) advance the checkpoint, verify it survives the final reopen.
        check(s.checkpoint(100).has_value(), "checkpoint(100)");
    }
    {
        auto s = make();
        check(s.checkpoint_bucket() == 100, "advanced checkpoint durable");
    }

    std::printf("%s: %s\n", label, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace quark::testkit
