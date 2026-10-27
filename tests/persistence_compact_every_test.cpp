// Tests ADR-052 (#24b) CompactEveryNEvents<N>: after N committed events past the last snapshot the
// EventLog writes a snapshot and the store truncates the subsumed prefix. Proves, for InMemoryStore
// and FileStore: the log stays bounded, recovery (snapshot + tail) still yields the exact state, the
// FileStore file shrinks and survives a reopen (including a fully-truncated log), and truncate_log
// refuses to drop events no snapshot subsumes and honours the fence.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "tmp_dir_util.hpp"

#include "quark/core/event_log.hpp"
#include "quark/core/file_store.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/serialize.hpp"
#include "quark/core/snapshot.hpp"

using namespace quark;

struct Added {
    std::int64_t n;
};
struct Sum {
    std::int64_t total = 0;
};
QUARK_SERIALIZE(Added, (1, n))
QUARK_SERIALIZE(Sum, (1, total))

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
const auto apply = [](Sum& s, const Added& a) { s.total += a.n; };

template <class S, class SizeFn>
void run(S& store, SizeFn log_size, bool& ok) {
    const ActorId id{TypeKey{7}, 1};
    auto fence = store.acquire_fence(id);
    check(fence.has_value(), "fence", ok);
    EventLog<Added, S> log(store, id, *fence, store.last_seq(id) + 1);
    Sum state;
    std::int64_t expect = 0;
    std::size_t max_log = 0;
    int compactions = 0;
    for (int i = 1; i <= 25; ++i) {
        log.stage(Added{i});
        check(log.commit().has_value(), "commit", ok);
        state.total += i;
        expect += i;
        auto c = log.template compact_if_due<CompactEveryNEvents<10>>(state);
        check(c.has_value(), "compact_if_due ok", ok);
        if (c && *c) ++compactions;
        max_log = std::max(max_log, log_size(id));
    }
    check(compactions == 2, "compacted at seq 10 and 20 only", ok);
    check(max_log <= 10, "log never exceeds N events", ok);
    check(log_size(id) == 5, "tail holds the 5 events past the last snapshot", ok);
    auto rec = recover_event_sourced<Sum, Added>(store, id, Sum{}, apply);
    check(rec.has_value() && rec->state.total == expect && rec->last_seq == 25,
          "recovery from snapshot+tail reproduces the exact state", ok);

    // Exactly N more events -> compaction with an EMPTY tail (log fully truncated).
    for (int i = 26; i <= 30; ++i) {
        log.stage(Added{i});
        (void)log.commit();
        state.total += i;
    }
    auto c = log.template compact_if_due<CompactEveryNEvents<10>>(state);
    check(c.has_value() && *c, "third compaction", ok);
    check(log_size(id) == 0, "fully truncated log", ok);

    // Safety: truncation without a covering snapshot is refused; an unfenced token is refused.
    check(!store.truncate_log(id, *fence, 31).has_value(), "truncate beyond snapshot refused", ok);
    check(!store.truncate_log(id, FenceToken{0}, 1).has_value(), "unfenced truncate refused", ok);
}
}  // namespace

int main() {
    bool ok = true;
    {
        InMemoryStore st;
        run(st, [&](ActorId id) { return st.log_size(id); }, ok);
    }
    {
        const std::string dir = quark::test::make_temp_dir("quark_compact_");
        const ActorId id{TypeKey{7}, 1};
        std::uintmax_t size_after = 0;
        {
            FileStore st(dir);
            run(st, [&](ActorId i) { return st.log_size(i); }, ok);
            for (auto& f : std::filesystem::directory_iterator(dir)) size_after = f.file_size();
        }
        {  // reopen: fully-truncated log + snapshot recovers state and continues the sequence
            FileStore st(dir);
            check(st.last_seq(id) == 30, "last_seq survives reopen of a compacted log", ok);
            auto rec = recover_event_sourced<Sum, Added>(st, id, Sum{}, apply);
            check(rec.has_value() && rec->state.total == 465 && rec->last_seq == 30,
                  "FileStore recovery after reopen is exact", ok);
            auto fence = st.acquire_fence(id);
            EventLog<Added, FileStore> log(st, id, *fence, st.last_seq(id) + 1);
            log.stage(Added{1});
            check(log.commit().has_value(), "append after compaction+reopen", ok);
            auto rec2 = recover_event_sourced<Sum, Added>(st, id, Sum{}, apply);
            check(rec2.has_value() && rec2->state.total == 466 && rec2->last_seq == 31,
                  "recovery sees post-compaction append", ok);
        }
        check(size_after < 1500, "compacted WAL file stays small (bounded on disk)", ok);
        std::filesystem::remove_all(dir);
    }
    std::printf("persistence_compact_every_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
