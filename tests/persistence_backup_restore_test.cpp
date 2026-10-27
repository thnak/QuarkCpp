// Tests ADR-055 (#23) online backup / restore with a fencing epoch, for InMemoryStore and FileStore:
//  1. round trip (through the byte codec + image file): recovered actor state is identical;
//  2. a backup taken WHILE a writer thread commits is a consistent cut: per-actor contiguous prefix,
//     and across two actors written in lockstep the cut never shows B ahead of A or more than 1 behind;
//  3. after restore, a writer holding any pre-restore token is rejected (errc::unavailable), the
//     restore epoch exceeds every token in the image, and acquire_fence continues above it;
//  4. restore into a FRESH FileStore directory survives a reopen and EventLog recovery + new commits
//     work; restore into a non-empty store is refused; corrupt image bytes are rejected.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "tmp_dir_util.hpp"

#include "quark/core/backup.hpp"
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
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
const auto apply = [](Sum& s, const Added& a) { s.total += a.n; };
const ActorId kA{TypeKey{7}, 1};
const ActorId kB{TypeKey{7}, 2};

template <class S>
std::int64_t recovered_total(S& st, ActorId id, SeqNo* last = nullptr) {
    auto rec = recover_event_sourced<Sum, Added>(st, id, Sum{}, apply);
    if (!rec) return -1;
    if (last) *last = rec->last_seq;
    return rec->state.total;
}

// Populate A with 12 events and a snapshot at seq 10 (so image = snapshot + tail), B with 3 events.
template <class S>
FenceToken populate(S& st) {
    auto fa = st.acquire_fence(kA);
    EventLog<Added, S> la(st, kA, *fa, 1);
    Sum s;
    for (int i = 1; i <= 12; ++i) {
        la.stage(Added{i});
        check(la.commit().has_value(), "commit A");
        if (i <= 10) s.total += i;
        if (i == 10) check(la.template compact_if_due<CompactEveryNEvents<10>>(s).has_value(), "compact");
    }
    auto fb = st.acquire_fence(kB);
    EventLog<Added, S> lb(st, kB, *fb, 1);
    for (int i = 1; i <= 3; ++i) {
        lb.stage(Added{100 * i});
        check(lb.commit().has_value(), "commit B");
    }
    return *fa;
}

template <class Src, class Dst>
void round_trip_and_fence(Src& src, Dst& dst, const std::string& file) {
    const FenceToken old_a = populate(src);
    const std::int64_t want_a = recovered_total(src, kA), want_b = recovered_total(src, kB);
    check(want_a == 78 && want_b == 600, "source totals");

    check(snapshot_to_file(src, file).has_value(), "snapshot_to_file");
    auto ep = restore_from_file(dst, file);
    check(ep.has_value(), "restore_from_file");
    if (!ep) return;
    check(recovered_total(dst, kA) == want_a, "round trip: A state equal");
    check(recovered_total(dst, kB) == want_b, "round trip: B state equal");
    check(dst.last_seq(kA) == src.last_seq(kA) && dst.last_seq(kB) == src.last_seq(kB), "last_seq equal");

    // Fencing epoch: strictly above every token in the image; old owner rejected; new owner works.
    check(old_a < *ep, "epoch > old owner token");
    check(dst.current_owner(kA) == *ep && dst.current_owner(kB) == *ep, "owner == epoch");
    const std::byte payload[1]{};
    auto stale = dst.append(kA, old_a, dst.last_seq(kA) + 1, payload);
    check(!stale && stale.error().code == errc::unavailable, "old owner write rejected after restore");
    auto stale_b = dst.append_batch(kA, FenceToken{ep->value - 1}, {});
    check(!stale_b && stale_b.error().code == errc::unavailable, "token epoch-1 rejected");
    auto fresh = dst.acquire_fence(kA);
    check(fresh && *ep < *fresh, "acquire_fence continues above the epoch");
    if (fresh) {
        EventLog<Added, Dst> l(dst, kA, *fresh, dst.last_seq(kA) + 1);
        l.stage(Added{1000});
        check(l.commit().has_value(), "new owner commits");
        check(recovered_total(dst, kA) == want_a + 1000, "recovery sees post-restore commit");
    }
    // The source (old cluster) is untouched by the backup and still writable by its owner.
    EventLog<Added, Src> ls(src, kA, old_a, src.last_seq(kA) + 1);
    ls.stage(Added{1});
    check(ls.commit().has_value(), "source still writable after backup");

    // min_epoch: an owner that acquired AFTER the cut is only fenced if the operator supplies a floor.
    auto late = src.acquire_fence(kA);  // token > image epoch
    check(late.has_value(), "late acquire");
}

template <class S>
void concurrent_cut(S& st, const std::string& dir_tag) {
    auto fa = st.acquire_fence(kA);
    auto fb = st.acquire_fence(kB);
    EventLog<Added, S> la(st, kA, *fa, 1), lb(st, kB, *fb, 1);
    std::atomic<bool> stop{false};
    std::atomic<int> written{0};
    std::thread w([&] {
        for (int i = 1; i <= 400 && !stop.load(); ++i) {
            la.stage(Added{i});
            if (!la.commit()) break;
            lb.stage(Added{i});
            if (!lb.commit()) break;
            written.store(i);
        }
    });
    int images = 0;
    while (written.load() < 400 && images < 200) {
        BackupImage img;
        check(st.snapshot_to(img).has_value(), "concurrent snapshot_to");
        // Decode round trip of every image too (codec under load).
        auto dec = decode_backup(encode_backup(img));
        check(dec.has_value(), "image decodes");
        std::uint64_t la_seq = 0, lb_seq = 0;
        for (const ActorBackup& a : img.actors) {
            // Per-actor: contiguous committed prefix 1..last_seq with event value == seq.
            check(a.log.size() == a.last_seq, "log is the full prefix (no snapshot in this test)");
            for (std::size_t i = 0; i < a.log.size(); ++i)
                check(a.log[i].seq == i + 1, "contiguous seqs");
            (a.id == kA ? la_seq : lb_seq) = a.last_seq;
        }
        check(la_seq >= lb_seq && la_seq - lb_seq <= 1, "cross-actor cut: B lags A by at most one commit");
        ++images;
        if (images == 1 && written.load() == 0) std::this_thread::yield();
    }
    stop = true;
    w.join();
    check(images > 0, "took at least one image");
    // Final image restores to a store whose A recovery is a triangular-number prefix.
    BackupImage img;
    check(st.snapshot_to(img).has_value(), "final snapshot_to");
    InMemoryStore mem;
    check(mem.restore_from(img).has_value(), "restore final image");
    SeqNo last = 0;
    const std::int64_t tot = recovered_total(mem, kA, &last);
    check(tot == static_cast<std::int64_t>(last) * (static_cast<std::int64_t>(last) + 1) / 2,
          "restored actor state is exactly a prefix");
    (void)dir_tag;
}
}  // namespace

int main() {
    const std::string dir = quark::test::make_temp_dir("quark_backup_");
    const std::string img_file = dir + "/image.qbak";
    {  // InMemory -> InMemory
        InMemoryStore a, b;
        round_trip_and_fence(a, b, img_file);
    }
    {  // FileStore -> FileStore (restored into a fresh dir)
        const std::string d1 = dir + "/src", d2 = dir + "/dst";
        std::filesystem::create_directories(d1);
        std::filesystem::create_directories(d2);
        {
            FileStore a(d1), b(d2);
            round_trip_and_fence(a, b, img_file);
        }
        {  // reopen the restored directory: epoch durable, recovery works, stale token still fenced
            FileStore b(d2);
            check(recovered_total(b, kB) == 600, "restored FileStore recovers after reopen");
            check(b.current_owner(kB).value >= 1, "restored owner durable across reopen");
            const std::byte p[1]{};
            check(!b.append(kB, FenceToken{1}, b.last_seq(kB) + 1, p).has_value(),
                  "pre-restore token still fenced after reopen");
        }
        {  // refuse a non-empty target
            FileStore b(d2);
            BackupImage img;
            check(b.snapshot_to(img).has_value() && img.actors.size() == 2, "FileStore enumerates actors");
            check(!b.restore_from(img).has_value(), "restore into non-empty FileStore refused");
        }
    }
    {  // min_epoch floors the epoch above a token issued after the cut
        InMemoryStore a, b;
        populate(a);
        BackupImage img;
        check(a.snapshot_to(img).has_value(), "snapshot");
        auto late = a.acquire_fence(kA);
        auto ep = b.restore_from(img, *late);
        check(ep && *late < *ep, "min_epoch raises the epoch above a post-cut token");
        const std::byte p[1]{};
        check(!b.append(kA, *late, 100, p).has_value(), "post-cut token fenced when min_epoch supplied");
    }
    {  // corrupt / truncated images are rejected, non-empty InMemory refused
        InMemoryStore a;
        populate(a);
        BackupImage img;
        (void)a.snapshot_to(img);
        auto bytes = encode_backup(img);
        auto flipped = bytes;
        flipped[20] = static_cast<std::byte>(~static_cast<unsigned>(flipped[20]));
        check(!decode_backup(flipped).has_value(), "bit-flipped image rejected");
        bytes.resize(bytes.size() - 7);
        check(!decode_backup(bytes).has_value(), "truncated image rejected");
        check(!a.restore_from(img).has_value(), "restore into non-empty InMemoryStore refused");
    }
    {
        InMemoryStore st;
        concurrent_cut(st, "mem");
    }
    {
        const std::string d = dir + "/live";
        std::filesystem::create_directories(d);
        FileStore st(d);
        concurrent_cut(st, "file");
    }
    std::filesystem::remove_all(dir);
    std::printf("persistence_backup_restore_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
