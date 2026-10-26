// Tests 027-Reminders / 012-Persistence hardening of FileReminderStore + ReminderService
// (review B7/B9/B10, REVIEW-BACKLOG.md):
//   * B7  a CRC-bad record with intact data AFTER it is corruption: the log is NOT truncated and the
//         store refuses to serve/append, while a bad record ending at EOF (torn tail) is still truncated;
//   * B7  a partial append (disk-full / EFBIG mid-record) is rolled back, so a later append survives
//         the next replay instead of hiding behind a torn record (Linux: provoked with RLIMIT_FSIZE);
//   * B9  a failed compact() leaves the store writable on the OLD file (fd_ is not left closed) and
//         the PAL exposes the directory-sync barrier;
//   * B10 re-registering a reminder drops the stale due-index entry (no early fire with the new due
//         time, no fire at the old time for a rescheduled one-shot); a failing store does not lose a
//         reminder or mark it fired.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#endif

#include "quark/core/reminder_service.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

std::filesystem::path unique_path(const char* tag) {
    const auto n = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string("quark_rem_hard_") + tag + "_" + std::to_string(static_cast<long long>(n)));
}

ReminderRow row(std::uint64_t key, const char* name, std::int64_t due, std::int64_t period = 0) {
    return ReminderRow{ActorId{TypeKey{0x7E}, key}, name, due, period, std::vector<std::byte>(40, std::byte{9})};
}

std::vector<char> slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void spit(const std::filesystem::path& p, const std::vector<char>& v) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(v.data(), static_cast<std::streamsize>(v.size()));
}

// ---- B7: mid-file corruption vs torn tail ------------------------------------------------------
bool test_midfile_corruption(bool& ok) {
    const auto path = unique_path("mid");
    std::uintmax_t size_after_two = 0;
    {
        FileReminderStore s(path.string());
        check(s.put(row(1, "a", 100)).has_value(), "put a", ok);
        check(s.put(row(2, "b", 200)).has_value(), "put b", ok);
        size_after_two = std::filesystem::file_size(path);
        check(s.put(row(3, "c", 300)).has_value(), "put c", ok);
    }
    // Flip a byte inside record b's body (record a is first; b starts at its end). Record c stays intact.
    auto bytes = slurp(path);
    const std::size_t full = bytes.size();
    const std::size_t a_end = size_after_two / 2;  // a and b are the same size (same shape rows)
    bytes[a_end + 8 + 12] = static_cast<char>(bytes[a_end + 8 + 12] ^ 0x5A);
    spit(path, bytes);
    {
        FileReminderStore s(path.string());
        check(std::filesystem::file_size(path) == full, "B7: mid-file CRC failure does NOT truncate the log", ok);
        check(!s.load_all().has_value(), "B7: a corrupt log refuses load_all", ok);
        check(!s.put(row(4, "d", 400)).has_value(), "B7: a corrupt log refuses put", ok);
        check(!s.remove(ReminderKey{ActorId{TypeKey{0x7E}, 1}, reminder_name_hash("a")}).has_value(),
              "B7: a corrupt log refuses remove", ok);
        check(!s.checkpoint(5).has_value(), "B7: a corrupt log refuses checkpoint", ok);
        check(std::filesystem::file_size(path) == full, "B7: refused ops leave the file untouched", ok);
    }
    std::filesystem::remove(path);
    return ok;
}

bool test_torn_tail_still_truncated(bool& ok) {
    const auto path = unique_path("tail");
    {
        FileReminderStore s(path.string());
        check(s.put(row(1, "a", 100)).has_value(), "put a", ok);
        check(s.put(row(2, "b", 200)).has_value(), "put b", ok);
    }
    auto bytes = slurp(path);
    const std::size_t good = bytes.size();
    bytes.resize(good - 7);  // tear the last record: it now ends exactly at EOF
    spit(path, bytes);
    {
        FileReminderStore s(path.string());
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == 1, "torn tail: only the intact record survives", ok);
        check(s.put(row(5, "e", 500)).has_value(), "torn tail: the store is writable", ok);
    }
    {
        FileReminderStore s(path.string());
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == 2, "torn tail: the post-repair append survives replay", ok);
    }
    std::filesystem::remove(path);
    return ok;
}

// ---- B7: a failed/partial append is rolled back --------------------------------------------------
bool test_partial_append_rolled_back(bool& ok) {
#if defined(__linux__)
    const auto path = unique_path("partial");
    {
        FileReminderStore s(path.string());
        check(s.put(row(1, "a", 100)).has_value(), "put a", ok);
        const auto sz = std::filesystem::file_size(path);
        // Cap the file size mid-way through the next record: write() lands a SHORT count, then EFBIG.
        struct rlimit old{};
        getrlimit(RLIMIT_FSIZE, &old);
        auto old_handler = signal(SIGXFSZ, SIG_IGN);
        struct rlimit lim = old;
        lim.rlim_cur = sz + 20;
        setrlimit(RLIMIT_FSIZE, &lim);
        const bool failed = !s.put(row(2, "b", 200)).has_value();
        setrlimit(RLIMIT_FSIZE, &old);
        signal(SIGXFSZ, old_handler);
        check(failed, "B7: the over-limit append reports failure", ok);
        check(std::filesystem::file_size(path) == sz, "B7: the partial record is truncated back off the file", ok);
        check(s.put(row(3, "c", 300)).has_value(), "B7: a later append succeeds after the rollback", ok);
    }
    {
        FileReminderStore s(path.string());
        auto rows = s.load_all();
        // a + c survive; b's put failed so (like InMemory on error) it is not durable.
        bool has_a = false, has_c = false;
        if (rows) for (const auto& r : *rows) { has_a |= r.name == "a"; has_c |= r.name == "c"; }
        check(has_a && has_c, "B7: replay keeps the append that FOLLOWED the failed one", ok);
    }
    std::filesystem::remove(path);
#endif
    return ok;
}

// ---- B9 -------------------------------------------------------------------------------------------
bool test_failed_compact_keeps_store_writable(bool& ok) {
    const auto path = unique_path("compact");
    const auto tmp = std::filesystem::path(path.string() + ".compact");
    std::filesystem::create_directory(tmp);  // a directory squats on the tmp name: open(tmp) fails
    {
        FileReminderStore s(path.string());
        check(s.put(row(1, "a", 100)).has_value(), "put a", ok);
        check(!s.compact().has_value(), "B9: compact() fails when the tmp file cannot be created", ok);
        check(s.put(row(2, "b", 200)).has_value(), "B9: the store is STILL writable after a failed compact", ok);
    }
    std::filesystem::remove(tmp);
    {
        FileReminderStore s(path.string());
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == 2, "B9: both rows are durable on the old file", ok);
        check(s.compact().has_value(), "B9: compact succeeds once the obstruction is gone", ok);
        check(s.put(row(3, "c", 300)).has_value(), "B9: the store is writable after a good compact", ok);
    }
    {
        FileReminderStore s(path.string());
        auto rows = s.load_all();
        check(rows.has_value() && rows->size() == 3, "B9: compacted + appended rows replay", ok);
    }
    std::filesystem::remove(path);
    return ok;
}

bool test_pal_dir_sync(bool& ok) {
    const auto dir = unique_path("dirsync");
    std::filesystem::create_directory(dir);
    check(pal::durable_sync_dir(dir.string()), "B9: durable_sync_dir succeeds on an existing directory", ok);
    const auto a = (dir / "a").string(), b = (dir / "b").string();
    { std::ofstream(a) << "x"; }
    check(pal::durable_rename(a, b), "B9: durable_rename moves the file", ok);
    check(!std::filesystem::exists(a) && std::filesystem::exists(b), "B9: rename took effect", ok);
#if defined(__linux__)
    check(!pal::durable_sync_dir((dir / "nope").string()), "B9: syncing a missing directory reports failure", ok);
#endif
    std::filesystem::remove_all(dir);
    return ok;
}

// ---- B10 -------------------------------------------------------------------------------------------
struct Fire { std::string name; std::int64_t due; std::int64_t at; };
constexpr std::int64_t kSec = 1'000'000'000;

bool test_reregister_drops_stale_index_entry(bool& ok) {
    InMemoryReminderStore store;
    std::vector<Fire> fires;
    ReminderService<InMemoryReminderStore> svc(
        store, [&](const FireEvent& e) { fires.push_back({std::string(e.name), e.scheduled_due_ns, e.fired_at_ns}); });
    const ActorId a{TypeKey{0x7E}, 1};

    // Postponed one-shot: registered for t=10s, then re-registered for t=50s.
    (void)svc.remind_at(a, "late", WallInstant{10 * kSec}, 0, {});
    (void)svc.remind_at(a, "late", WallInstant{50 * kSec}, 0, {});
    svc.tick(WallInstant{20 * kSec});
    check(fires.empty(), "B10: a postponed reminder does NOT fire at its old time (t=20s)", ok);
    svc.tick(WallInstant{60 * kSec});
    check(fires.size() == 1 && fires[0].due == 50 * kSec, "B10: it fires exactly once at the NEW due time", ok);
    svc.tick(WallInstant{120 * kSec});
    check(fires.size() == 1, "B10: and never again", ok);
    check(svc.pending() == 0, "B10: nothing left pending", ok);

    // Re-register EARLIER: old (later) entry must not fire a second time.
    fires.clear();
    (void)svc.remind_at(a, "early", WallInstant{500 * kSec}, 0, {});
    (void)svc.remind_at(a, "early", WallInstant{200 * kSec}, 0, {});
    svc.tick(WallInstant{250 * kSec});
    check(fires.size() == 1 && fires[0].due == 200 * kSec, "B10: pulled-earlier reminder fires once at the new time", ok);
    svc.tick(WallInstant{600 * kSec});
    check(fires.size() == 1, "B10: the stale later index entry does not fire it again", ok);

    // Re-register AFTER the stale entry was already pulled into the ready segment (rate-limited tick).
    fires.clear();
    ReminderService<InMemoryReminderStore> svc2(
        store, [&](const FireEvent& e) { fires.push_back({std::string(e.name), e.scheduled_due_ns, e.fired_at_ns}); },
        ReminderConfig{.bucket_ns = kSec, .fire_rate = 0, .credit = 1});
    (void)svc2.remind_at(a, "x", WallInstant{1 * kSec}, 0, {});
    (void)svc2.remind_at(a, "y", WallInstant{1 * kSec}, 0, {});
    svc2.tick(WallInstant{2 * kSec});                            // credit=1: one fires, one waits in ready_
    const std::size_t first = fires.size();
    (void)svc2.remind_at(a, "x", WallInstant{900 * kSec}, 0, {});  // postpone BOTH names
    (void)svc2.remind_at(a, "y", WallInstant{900 * kSec}, 0, {});
    svc2.tick(WallInstant{3 * kSec});
    svc2.tick(WallInstant{4 * kSec});
    check(first == 1 && fires.size() == 1, "B10: an entry already in the ready segment is dropped on re-register", ok);
    return ok;
}

// A store whose writes can be made to fail.
struct FlakyStore {
    InMemoryReminderStore inner;
    bool fail_put = false, fail_remove = false;
    [[nodiscard]] result<void> put(const ReminderRow& r) {
        if (fail_put) return fail(errc::internal, "flaky put");
        return inner.put(r);
    }
    [[nodiscard]] result<void> remove(ReminderKey k) {
        if (fail_remove) return fail(errc::internal, "flaky remove");
        return inner.remove(k);
    }
    [[nodiscard]] result<std::vector<ReminderRow>> load_all() const { return inner.load_all(); }
    [[nodiscard]] result<void> checkpoint(std::int64_t b) { return inner.checkpoint(b); }
    [[nodiscard]] std::int64_t checkpoint_bucket() const noexcept { return inner.checkpoint_bucket(); }
};
static_assert(ReminderStore<FlakyStore>);

bool test_tick_survives_store_errors(bool& ok) {
    FlakyStore store;
    int fired = 0;
    ReminderService<FlakyStore> svc(store, [&](const FireEvent&) { ++fired; });
    const ActorId a{TypeKey{0x7E}, 1};

    (void)svc.remind_at(a, "once", WallInstant{1 * kSec}, 0, {});
    store.fail_remove = true;
    svc.tick(WallInstant{2 * kSec});
    check(fired == 1, "B10: fired once", ok);
    check(svc.pending() == 1, "B10: a failed durable remove keeps the reminder (not silently dropped)", ok);
    check(store.inner.live_rows() == 1, "B10: the durable row is still there", ok);
    store.fail_remove = false;
    svc.tick(WallInstant{3 * kSec});
    check(svc.pending() == 0 && store.inner.live_rows() == 0,
          "B10: once the store recovers, the retry confirms + removes it", ok);

    fired = 0;
    (void)svc.remind_at(a, "per", WallInstant{10 * kSec}, 5 * kSec, {});
    store.fail_put = true;
    svc.tick(WallInstant{11 * kSec});
    check(fired == 1, "B10: periodic fired", ok);
    store.fail_put = false;
    svc.tick(WallInstant{12 * kSec});
    check(fired == 2, "B10: a failed advance is retried (same occurrence re-fires: at-least-once)", ok);
    svc.tick(WallInstant{13 * kSec});
    check(fired == 2, "B10: after the advance succeeds, the next occurrence is 5s later", ok);
    svc.tick(WallInstant{16 * kSec});
    check(fired == 3, "B10: periodic continues from scheduled + period", ok);
    return ok;
}
}  // namespace

int main() {
    bool ok = true;
    struct { const char* n; bool (*f)(bool&); } tests[] = {
        {"midfile_corruption", test_midfile_corruption},
        {"torn_tail_still_truncated", test_torn_tail_still_truncated},
        {"partial_append_rolled_back", test_partial_append_rolled_back},
        {"failed_compact_keeps_store_writable", test_failed_compact_keeps_store_writable},
        {"pal_dir_sync", test_pal_dir_sync},
        {"reregister_drops_stale_index_entry", test_reregister_drops_stale_index_entry},
        {"tick_survives_store_errors", test_tick_survives_store_errors},
    };
    for (auto& t : tests) {
        bool tok = true;
        t.f(tok);
        std::printf("%s: %s\n", t.n, tok ? "ok" : "FAILED");
        ok = ok && tok;
    }
    std::printf(ok ? "persistence_reminder_store_hardening: PASS\n"
                   : "persistence_reminder_store_hardening: FAIL\n");
    return ok ? 0 : 1;
}
