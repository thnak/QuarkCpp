// Shared 012 `Store` FAULT-handling harness for the durable adapters (SqliteStore/RocksStore), review
// B3-B6/B11 (REVIEW-BACKLOG.md). Where store_conformance.hpp proves the happy path survives a restart,
// this proves the adapters never report success or "no data" when they actually FAILED:
//   * an unusable store (failed open) errors on every operation — a failed read is never "no snapshot"
//     (recovery would seed default state over real data) and a failed acquire_fence hands out no token;
//   * a write under the invalid token 0 is refused, and a snapshot older than the stored one is refused;
//   * the store is safe under concurrent use: concurrent acquire_fence on one actor never returns a
//     duplicate token, and concurrent appends to different actors all commit (run under TSan).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark::testkit {

template <class S, class Make, class MakeBroken>
int run_store_fault_checks(const char* label, Make make, MakeBroken make_broken) {
    bool ok = true;
    auto check = [&](bool c, const char* what) {
        if (!c) { std::fprintf(stderr, "  [%s] CHECK FAILED: %s\n", label, what); ok = false; }
    };
    auto bytes = [](std::uint8_t v) { return std::vector<std::byte>(8, static_cast<std::byte>(v)); };
    const ActorId id{TypeKey{0xFA17}, 1};

    // ---- an unusable store errors on EVERYTHING; a failed read is not "no data" ---------------
    {
        S broken = make_broken();
        check(!broken.healthy(), "the broken store reports unhealthy");
        check(!broken.acquire_fence(id).has_value(), "acquire_fence fails (no token handed out)");
        check(!broken.load_snapshot(id).has_value(), "load_snapshot is an ERROR, not 'no snapshot'");
        check(!broken.read_log(id, 0).has_value(), "read_log is an ERROR, not an empty log");
        check(!broken.append(id, FenceToken{1}, 1, std::span<const std::byte>(bytes(1))).has_value(),
              "append fails");
        check(!broken.save_snapshot(id, FenceToken{1}, SnapshotRecord{FenceToken{1}, 0, bytes(2)})
                   .has_value(),
              "save_snapshot fails");
    }

    // ---- token 0 and stale snapshots -----------------------------------------------------------
    {
        S s = make();
        check(s.healthy(), "the working store is healthy");
        const FenceToken none{0};
        check(!s.append(id, none, 1, std::span<const std::byte>(bytes(1))).has_value(),
              "append under token 0 is refused");
        check(!s.save_snapshot(id, none, SnapshotRecord{none, 0, bytes(2)}).has_value(),
              "save_snapshot under token 0 is refused");
        const auto f = s.acquire_fence(id);
        check(f.has_value() && f->value >= 1, "a real fence token is >= 1");
        if (f) {
            check(s.append(id, *f, 1, std::span<const std::byte>(bytes(1))).has_value(), "append 1");
            check(s.append(id, *f, 2, std::span<const std::byte>(bytes(2))).has_value(), "append 2");
            check(s.save_snapshot(id, *f, SnapshotRecord{*f, 2, bytes(3)}).has_value(), "snapshot @2");
            check(!s.save_snapshot(id, *f, SnapshotRecord{*f, 1, bytes(4)}).has_value(),
                  "an OLDER snapshot (through_seq=1) is refused");
            check(s.save_snapshot(id, *f, SnapshotRecord{*f, 2, bytes(5)}).has_value(),
                  "an equal through_seq snapshot is allowed");
            auto snap = s.load_snapshot(id);
            check(snap.has_value() && snap->has_value() && (*snap)->through_seq == 2 &&
                      (*snap)->record == bytes(5),
                  "the stored snapshot is the latest accepted one");
            // A fenced-out (stale) writer is refused.
            const auto g = s.acquire_fence(id);
            check(g.has_value() && g->value > f->value, "a second acquire hands out a greater token");
            auto stale = s.append(id, *f, 3, std::span<const std::byte>(bytes(3)));
            check(!stale.has_value() && stale.error().code == errc::unavailable,
                  "the superseded token is fenced out");
        }
    }

    // ---- concurrency: unique tokens, no lost appends -------------------------------------------
    {
        S s = make();
        constexpr int kThreads = 8;
        constexpr int kEvents = 100;
        std::vector<std::uint64_t> toks(kThreads, 0);
        std::vector<int> failures(kThreads, 0);
        const ActorId shared{TypeKey{0xFA17}, 999};
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t] {
                const auto fs = s.acquire_fence(shared);  // hammered by every thread
                toks[static_cast<std::size_t>(t)] = fs ? fs->value : 0;
                const ActorId mine{TypeKey{0xFA18}, static_cast<std::uint64_t>(t + 1)};
                const auto f = s.acquire_fence(mine);
                if (!f) { ++failures[static_cast<std::size_t>(t)]; return; }
                for (int i = 1; i <= kEvents; ++i)
                    if (!s.append(mine, *f, static_cast<SeqNo>(i), std::span<const std::byte>(bytes(7)))
                             .has_value())
                        ++failures[static_cast<std::size_t>(t)];
            });
        }
        for (auto& th : ts) th.join();
        std::sort(toks.begin(), toks.end());
        check(toks.front() >= 1 && std::adjacent_find(toks.begin(), toks.end()) == toks.end(),
              "concurrent acquire_fence on ONE actor never hands out a duplicate token");
        check(std::all_of(failures.begin(), failures.end(), [](int f) { return f == 0; }),
              "every concurrent per-actor append succeeds");
        for (int t = 0; t < kThreads; ++t)
            check(s.last_seq(ActorId{TypeKey{0xFA18}, static_cast<std::uint64_t>(t + 1)}) == kEvents,
                  "every concurrently appended event is committed");
    }

    std::printf("%s: %s\n", label, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace quark::testkit
