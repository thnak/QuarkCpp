// Proves the 012 `FileStore` adapter is CRASH-DURABLE and models the same `Store` seam as the
// reference `InMemoryStore` — i.e. that a real database/filesystem backend plugs in behind the
// persistence seam with the snapshot/event-log/recovery/fencing mechanism unchanged.
//
// "Crash" is modeled the only portable way a unit test can: DESTROY the FileStore (closing its fds,
// exactly as process exit does) and OPEN A FRESH ONE on the same directory. Everything an
// acknowledged write promised must be present after the reopen — owner token, last_seq, latest
// snapshot, and the full event log — because each durable op fdatasync'd before returning.
//
// Four invariants:
//   1. state survives a reopen (owner, last_seq, snapshot bytes, log bytes);
//   2. fencing is durable — a superseded (stale-token) writer stays fenced out ACROSS a reopen;
//   3. append_batch is all-or-none — a non-monotonic batch appends nothing, before AND after reopen;
//   4. the real EventLog + recover_event_sourced mechanism folds a DURABLE log across a restart.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "tmp_dir_util.hpp"

#include "quark/core/error.hpp"
#include "quark/core/event_log.hpp"
#include "quark/core/file_store.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

std::vector<std::byte> B(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
}

}  // namespace

// A Described event/state pair for the end-to-end recovery sub-test (sub-test 4). At global scope so
// the QUARK_SERIALIZE describe hook attaches (matches the sample/test convention).
struct Added {
    std::int64_t n;
};
struct Sum {
    std::int64_t total = 0;
};
QUARK_SERIALIZE(Added, (1, n))
QUARK_SERIALIZE(Sum, (1, total))

int main() {
    bool ok = true;

    const std::string root = quark::test::make_temp_dir("quark_filestore_");
    const ActorId id{TypeKey{0xF11E}, 7};

    // ---- 1) write, "crash" (destroy), reopen, verify everything is durable --------------------
    {
        FileStore fs(root);
        const FenceToken t = fs.acquire_fence(id);
        check(t == FenceToken{1}, "first fence token is 1", ok);
        check(fs.append(id, t, 1, B("event-one")).has_value(), "append seq 1", ok);
        check(fs.append(id, t, 2, B("event-two")).has_value(), "append seq 2", ok);
        check(fs.append(id, t, 3, B("event-three")).has_value(), "append seq 3", ok);
        SnapshotRecord snap;
        snap.through_seq = 2;
        snap.record = B("snap@2");
        check(fs.save_snapshot(id, t, snap).has_value(), "save snapshot through_seq=2", ok);
    }  // fds closed — the "crash"/exit

    {
        FileStore fs(root);  // reopen → replay from disk
        check(fs.current_owner(id) == FenceToken{1}, "owner token survived reopen", ok);
        check(fs.last_seq(id) == 3, "last_seq survived reopen", ok);

        auto s = fs.load_snapshot(id);
        check(s.has_value() && s->has_value(), "snapshot present after reopen", ok);
        check(s.has_value() && s->has_value() && (*s)->through_seq == 2, "snapshot through_seq durable", ok);
        check(s.has_value() && s->has_value() && (*s)->record == B("snap@2"), "snapshot bytes durable", ok);
        check(s.has_value() && s->has_value() && (*s)->fence == FenceToken{1}, "snapshot fence durable", ok);

        auto cur = fs.read_log(id, 1);
        check(cur.has_value() && cur->size() == 3, "all 3 events durable", ok);
        check(cur.has_value() && cur->size() == 3 && cur->entries[0].seq == 1 &&
                  cur->entries[0].record == B("event-one"), "event 1 bytes durable", ok);
        check(cur.has_value() && cur->size() == 3 && cur->entries[2].seq == 3 &&
                  cur->entries[2].record == B("event-three"), "event 3 bytes durable", ok);
        auto tail = fs.read_log(id, 3);
        check(tail.has_value() && tail->size() == 1 && tail->entries[0].record == B("event-three"),
              "read_log(from=3) tail", ok);

        // ---- 2) fencing is durable across the reopen -----------------------------------------
        const FenceToken t2 = fs.acquire_fence(id);
        check(t2 == FenceToken{2}, "new owner strictly greater than the durable token", ok);
        auto stale = fs.append(id, FenceToken{1}, 4, B("stale"));
        check(!stale.has_value() && stale.error().code == errc::unavailable,
              "stale (token-1) writer is fenced out with errc::unavailable", ok);
        check(fs.last_seq(id) == 3, "the stale append did NOT land", ok);

        // ---- 3) append_batch is all-or-none --------------------------------------------------
        std::vector<EventRecord> bad{EventRecord{4, B("ok")}, EventRecord{4, B("dup-seq")}};
        auto rc = fs.append_batch(id, t2, std::span<const EventRecord>(bad));
        check(!rc.has_value(), "non-monotonic batch rejected", ok);
        check(fs.log_size(id) == 3, "rejected batch appended NOTHING (atomic)", ok);
        std::vector<EventRecord> good{EventRecord{4, B("g4")}, EventRecord{5, B("g5")}};
        check(fs.append_batch(id, t2, std::span<const EventRecord>(good)).has_value(), "good batch commits", ok);
        check(fs.log_size(id) == 5, "good batch atomically appended both events", ok);
    }

    {
        FileStore fs(root);  // reopen once more — the batch + new owner must be durable
        check(fs.current_owner(id) == FenceToken{2}, "bumped owner durable", ok);
        check(fs.last_seq(id) == 5, "batch last_seq durable", ok);
        check(fs.log_size(id) == 5, "batch events durable", ok);
    }

    // ---- 4) end-to-end: EventLog commit, restart, recover_event_sourced folds the durable log --
    {
        const ActorId eid{TypeKey{0x5011}, 1};
        {
            FileStore fs(root);
            const FenceToken f = fs.acquire_fence(eid);
            EventLog<Added, FileStore> log(fs, eid, f, fs.last_seq(eid) + 1);
            log.stage(Added{10});
            log.stage(Added{5});
            check(log.commit().has_value(), "EventLog commit to FileStore", ok);
        }  // restart
        {
            FileStore fs(root);
            auto rec = recover_event_sourced<Sum, Added>(
                fs, eid, Sum{}, [](Sum& s, const Added& a) { s.total += a.n; });
            check(rec.has_value(), "recover_event_sourced from FileStore", ok);
            check(rec.has_value() && rec->state.total == 15, "recovered folded state == 15 (durable)", ok);
            check(rec.has_value() && rec->last_seq == 2, "recovered last_seq == 2", ok);
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // tidy the temp dir (best-effort)

    std::printf("persistence_filestore_durable_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
