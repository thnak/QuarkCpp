// Tests 012-Persistence hardening of the reference stores (review B3/B5/B7/B11, REVIEW-BACKLOG.md):
//   * a write under the invalid token 0 ("no fence") is refused — a failed acquire_fence can never be
//     mistaken for a valid owner;
//   * a snapshot older (lower through_seq) than the stored one is refused;
//   * FileStore: MID-FILE corruption (bad CRC with intact records after it) is NOT truncated and the
//     log is refused, while a torn TAIL is still repaired by truncation;
//   * InMemoryStore / FileStore are safe under concurrent use (run under TSan in CI).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/file_store.hpp"
#include "quark/core/persistence.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

std::vector<std::byte> bytes(std::uint8_t v, std::size_t n = 16) {
    return std::vector<std::byte>(n, static_cast<std::byte>(v));
}

std::filesystem::path unique_dir() {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("quark_store_hard_" + std::to_string(static_cast<long long>(n)));
}

std::filesystem::path wal_path(const std::filesystem::path& dir, ActorId id) {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx-%016llx.qwal",
                  static_cast<unsigned long long>(id.type.value),
                  static_cast<unsigned long long>(id.key));
    return dir / name;
}

template <class S>
void common_semantics(S& store, const char* label, bool& ok) {
    const ActorId id{TypeKey{0xB0B}, 1};
    const std::string l = label;

    // ---- B5: token 0 is "no fence": every write under it is refused ---------------------------
    const FenceToken none{0};
    check(!store.append(id, none, 1, std::span<const std::byte>(bytes(1))).has_value(),
          (l + ": append under token 0 is refused").c_str(), ok);
    EventRecord one{1, bytes(1)};
    check(!store.append_batch(id, none, std::span<const EventRecord>(&one, 1)).has_value(),
          (l + ": append_batch under token 0 is refused").c_str(), ok);
    check(!store.save_snapshot(id, none, SnapshotRecord{none, 0, bytes(2)}).has_value(),
          (l + ": save_snapshot under token 0 is refused").c_str(), ok);

    // ---- a real token still writes ------------------------------------------------------------
    const FenceToken f = store.acquire_fence(id);
    check(f.value >= 1, (l + ": a real fence token is >= 1").c_str(), ok);
    check(store.append(id, f, 1, std::span<const std::byte>(bytes(1))).has_value(),
          (l + ": append under a real token succeeds").c_str(), ok);
    check(store.append(id, f, 2, std::span<const std::byte>(bytes(2))).has_value(),
          (l + ": second append succeeds").c_str(), ok);

    // ---- B11: a snapshot may not roll the recovery point back ----------------------------------
    check(store.save_snapshot(id, f, SnapshotRecord{f, 2, bytes(3)}).has_value(),
          (l + ": snapshot through_seq=2 accepted").c_str(), ok);
    check(!store.save_snapshot(id, f, SnapshotRecord{f, 1, bytes(4)}).has_value(),
          (l + ": an OLDER snapshot (through_seq=1) is refused").c_str(), ok);
    check(store.save_snapshot(id, f, SnapshotRecord{f, 2, bytes(5)}).has_value(),
          (l + ": an equal through_seq snapshot is allowed (snapshot-model flush)").c_str(), ok);
    auto snap = store.load_snapshot(id);
    check(snap.has_value() && snap->has_value() && (*snap)->through_seq == 2 &&
              (*snap)->record == bytes(5),
          (l + ": the stored snapshot is the latest accepted one").c_str(), ok);
}

template <class S>
void concurrency(S& store, const char* label, bool& ok) {
    constexpr int kThreads = 8;
    constexpr int kEvents = 200;
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            const ActorId id{TypeKey{0xC0C}, static_cast<std::uint64_t>(t + 1)};
            const FenceToken f = store.acquire_fence(id);
            for (int i = 1; i <= kEvents; ++i)
                if (!store.append(id, f, static_cast<SeqNo>(i), std::span<const std::byte>(bytes(7)))
                         .has_value())
                    failures.fetch_add(1);
            if (!store.save_snapshot(id, f, SnapshotRecord{f, kEvents, bytes(9)}).has_value())
                failures.fetch_add(1);
        });
    }
    // A shared actor hammered by every thread: fences stay unique and strictly increasing.
    std::vector<std::uint64_t> toks(kThreads);
    std::vector<std::thread> ts2;
    const ActorId shared{TypeKey{0xC0C}, 999};
    for (int t = 0; t < kThreads; ++t)
        ts2.emplace_back([&, t] { toks[static_cast<std::size_t>(t)] = store.acquire_fence(shared).value; });
    for (auto& th : ts) th.join();
    for (auto& th : ts2) th.join();
    std::sort(toks.begin(), toks.end());
    check(failures.load() == 0, (std::string(label) + ": concurrent per-actor writes all succeed").c_str(), ok);
    check(std::adjacent_find(toks.begin(), toks.end()) == toks.end() && toks.front() >= 1,
          (std::string(label) + ": concurrent acquire_fence on one actor never hands out a duplicate").c_str(),
          ok);
    for (int t = 0; t < kThreads; ++t) {
        const ActorId id{TypeKey{0xC0C}, static_cast<std::uint64_t>(t + 1)};
        check(store.last_seq(id) == kEvents,
              (std::string(label) + ": every concurrently appended event is committed").c_str(), ok);
    }
}
}  // namespace

int main() {
    bool ok = true;

    {
        InMemoryStore store;
        common_semantics(store, "InMemoryStore", ok);
    }
    {
        InMemoryStore store;
        concurrency(store, "InMemoryStore", ok);
    }

    const auto dir = unique_dir();
    {
        FileStore store(dir.string());
        common_semantics(store, "FileStore", ok);
    }
    {
        FileStore store((dir / "conc").string());
        concurrency(store, "FileStore", ok);
    }

    // ---- B7: mid-file corruption must not truncate; a torn tail still must ----------------------
    {
        const ActorId id{TypeKey{0xD0D}, 1};
        const auto wal = wal_path(dir, id);
        {
            FileStore store(dir.string());
            const FenceToken f = store.acquire_fence(id);  // record 1: FENCE
            for (SeqNo s = 1; s <= 3; ++s)                 // records 2..4: one batch each
                check(store.append(id, f, s, std::span<const std::byte>(bytes(static_cast<std::uint8_t>(s))))
                          .has_value(),
                      "seed append", ok);
        }
        const auto good_size = std::filesystem::file_size(wal);

        // Flip a byte inside the FIRST record's body: its CRC fails but 3 intact records follow.
        auto flip = [&](std::uint64_t off) {
            std::fstream f(wal, std::ios::in | std::ios::out | std::ios::binary);
            f.seekg(static_cast<std::streamoff>(off));
            char c = 0;
            f.get(c);
            c = static_cast<char>(c ^ 0x5A);
            f.seekp(static_cast<std::streamoff>(off));
            f.put(c);
        };
        flip(9);  // header is 8 bytes (len, crc); byte 8 is the kind, 9 starts the fence value
        {
            FileStore store(dir.string());
            check(store.corrupted(id), "mid-file CRC failure is flagged as corruption", ok);
            check(!store.load_snapshot(id).has_value(), "a corrupt log refuses load_snapshot", ok);
            check(!store.read_log(id, 0).has_value(), "a corrupt log refuses read_log", ok);
            check(store.acquire_fence(id).value == 0, "a corrupt log hands out no fence token", ok);
            check(!store.append(id, FenceToken{1}, 4, std::span<const std::byte>(bytes(4))).has_value(),
                  "a corrupt log refuses appends", ok);
        }
        check(std::filesystem::file_size(wal) == good_size,
              "the corrupt file is NOT truncated (later acknowledged records are preserved on disk)", ok);

        // Torn TAIL: damage only the last record (it ends at EOF) — repaired by truncation as before.
        const ActorId id2{TypeKey{0xD0D}, 2};
        const auto wal2 = wal_path(dir, id2);
        {
            FileStore store(dir.string());
            const FenceToken f = store.acquire_fence(id2);
            for (SeqNo s = 1; s <= 3; ++s)
                check(store.append(id2, f, s, std::span<const std::byte>(bytes(static_cast<std::uint8_t>(s))))
                          .has_value(),
                      "seed append (torn-tail actor)", ok);
        }
        const auto size2 = std::filesystem::file_size(wal2);
        {
            std::fstream f(wal2, std::ios::in | std::ios::out | std::ios::binary);
            f.seekg(static_cast<std::streamoff>(size2 - 1));
            char c = 0;
            f.get(c);
            c = static_cast<char>(c ^ 0x5A);
            f.seekp(static_cast<std::streamoff>(size2 - 1));
            f.put(c);
        }
        {
            FileStore store(dir.string());
            check(!store.corrupted(id2), "a damaged TAIL record is a torn write, not corruption", ok);
            check(store.last_seq(id2) == 2, "the torn tail record is dropped, earlier records survive", ok);
            check(store.read_log(id2, 0).has_value() && store.read_log(id2, 0)->size() == 2,
                  "two intact events remain readable", ok);
        }
        check(std::filesystem::file_size(wal2) < size2, "the torn tail is truncated away", ok);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::printf("persistence_store_hardening_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
