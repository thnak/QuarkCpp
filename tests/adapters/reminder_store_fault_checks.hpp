// Shared 027 `ReminderStore` FAULT/edge harness for the durable adapters (SqliteReminderStore /
// RocksReminderStore), review B13 (REVIEW-BACKLOG.md). Where reminder_store_conformance.hpp proves the
// happy path survives a restart, this proves:
//   * due times round-trip EXACTLY across a reopen, including negative and extreme values (the Rocks
//     adapter biases the due in its key and once returned it with the high bit flipped);
//   * re-arming a reminder across the sign boundary replaces the row and never leaves a duplicate;
//   * an unusable (failed-open) store errors on every op — `load_all` must not look like "no
//     reminders", or a cold start would silently forget every durable reminder;
//   * a failed checkpoint does not advance the in-RAM bucket; concurrent use is safe (run under TSan).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

namespace quark::testkit {

template <class S, class Make, class MakeBroken>
int run_reminder_store_fault_checks(const char* label, Make make, MakeBroken make_broken) {
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) { std::fprintf(stderr, "  [%s] CHECK FAILED: %s\n", label, what); ok = false; }
    };
    const std::vector<std::byte> payload(3, std::byte{0x5A});
    const ActorId actor{TypeKey{0xBEEF}, 1};

    // ---- an unusable store errors on everything (a failed read is not "no reminders") -----------
    {
        S broken = make_broken();
        check(!broken.healthy(), "the broken store reports unhealthy");
        check(!broken.load_all().has_value(), "load_all is an ERROR, not an empty list");
        check(!broken.put(ReminderRow{actor, "x", 1, 0, payload}).has_value(), "put fails");
        check(!broken.remove(ReminderRow{actor, "x", 1, 0, payload}.key()).has_value(), "remove fails");
        check(!broken.checkpoint(7).has_value(), "checkpoint fails");
        check(broken.checkpoint_bucket() == -1, "a failed checkpoint does not advance the in-RAM bucket");
    }

    // ---- exact due round-trip across a reopen, incl. negative / extreme values -----------------
    const std::int64_t dues[] = {std::numeric_limits<std::int64_t>::min() + 1, -5'000'000'000LL, -1, 0, 1,
                                 1'700'000'000'000'000'000LL, std::numeric_limits<std::int64_t>::max() - 1};
    {
        S s = make();
        check(s.healthy(), "the working store is healthy");
        std::uint64_t n = 0;
        for (const std::int64_t d : dues)
            check(s.put(ReminderRow{actor, "r" + std::to_string(n++), d, 0, payload}).has_value(), "put");
    }
    {
        S s = make();  // "restart"
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == std::size(dues), "every reminder reloaded after a reopen");
        if (rows) {
            std::vector<std::int64_t> got;
            for (const ReminderRow& r : *rows) got.push_back(r.scheduled_due_ns);
            std::vector<std::int64_t> want(std::begin(dues), std::end(dues));
            std::sort(want.begin(), want.end());
            check(std::is_sorted(got.begin(), got.end()), "load_all returns rows in due order (incl. negative)");
            std::sort(got.begin(), got.end());
            check(got == want, "every due time round-trips EXACTLY (negative and extreme values included)");
        }
    }

    // ---- re-arm across the sign boundary replaces, never duplicates ----------------------------
    {
        S s = make();
        const ReminderRow neg{actor, "rearm", -100, 0, payload};
        check(s.put(neg).has_value(), "put at a negative due");
        check(s.put(ReminderRow{actor, "rearm", 100, 50, payload}).has_value(), "re-arm to a positive due");
        check(s.put(ReminderRow{actor, "rearm", -7, 50, payload}).has_value(), "re-arm back to a negative due");
        auto rows = s.load_all();
        int n = 0;
        std::int64_t due = 0;
        if (rows)
            for (const ReminderRow& r : *rows)
                if (r.name == "rearm") { ++n; due = r.scheduled_due_ns; }
        check(n == 1, "exactly ONE row after repeated re-arms (no stale due-keyed duplicate)");
        check(n == 1 && due == -7, "the surviving row carries the latest due");
        check(s.remove(neg.key()).has_value(), "remove");
        auto after = s.load_all();
        bool gone = after.has_value();
        if (after)
            for (const ReminderRow& r : *after) gone = gone && r.name != "rearm";
        check(gone, "removed reminder is gone (no row left under any of its past dues)");
    }

    // ---- checkpoint advances only on success; durable across reopen ----------------------------
    {
        S s = make();
        check(s.checkpoint(12345).has_value() && s.checkpoint_bucket() == 12345, "checkpoint advances");
    }
    {
        S s = make();
        check(s.checkpoint_bucket() == 12345, "checkpoint durable across a reopen");
    }

    // ---- concurrency: many writers on distinct keys, a reader, no lost rows --------------------
    {
        S s = make();
        constexpr int kThreads = 6;
        constexpr int kPer = 60;
        std::vector<int> fails(kThreads, 0);
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t)
            ts.emplace_back([&, t] {
                const ActorId a{TypeKey{0xC0DE}, static_cast<std::uint64_t>(t + 1)};
                for (int i = 0; i < kPer; ++i) {
                    if (!s.put(ReminderRow{a, "k" + std::to_string(i), 1000 + i, 0, payload}).has_value())
                        ++fails[static_cast<std::size_t>(t)];
                    if (i % 20 == 0 && !s.load_all().has_value()) ++fails[static_cast<std::size_t>(t)];
                }
            });
        for (auto& th : ts) th.join();
        check(std::all_of(fails.begin(), fails.end(), [](int f) { return f == 0; }),
              "concurrent put/load_all all succeed");
        auto rows = s.load_all();
        std::size_t mine = 0;
        if (rows)
            for (const ReminderRow& r : *rows)
                if (r.actor.type.value == 0xC0DE) ++mine;
        check(mine == static_cast<std::size_t>(kThreads) * kPer, "no concurrent write was lost");
    }

    std::printf("%s: %s\n", label, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace quark::testkit
