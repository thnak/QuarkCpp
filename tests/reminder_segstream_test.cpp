// Proves the 027 durable-reminder subsystem (the SEGSTREAM design, decisions/ADR-017) upholds its
// load-bearing invariants — the same properties the design→prove gate graded, re-verified here as a
// standing regression test built from scratch on this machine (not trusting the debate's transcript).
//
// "Crash" is modeled the only portable way a unit test can (as persistence_filestore_durable_test
// does): DESTROY the service + FileReminderStore (closing fds, exactly as process exit does) and OPEN
// A FRESH store on the same file. Everything an acknowledged register promised must survive.
//
// Invariants:
//   1. MASS-DUE FLATTENS (ADR-017 C4p): N reminders due at ONE instant fire at peak == fire_rate per
//      bucket, NOT N — and the smear is deterministic (identical across two runs).
//   2. O(due-now) scan (ADR-017 F2): a huge DORMANT population does not fire and does not get pulled
//      — a tick with 10 due among 100k dormant fires exactly 10, leaves 100k pending.
//   3. DURABLE + AT-LEAST-ONCE, ZERO LOSS across a crash (ADR-017 S2p): register on a FileReminder
//      store, "crash" mid-wave, reopen, drain — every committed reminder fires, none lost.
//   4. IDEMPOTENT re-fire (ADR-017 C1/S2p): a duplicate fire (at-least-once) carries the SAME
//      (name, scheduled_due) dedup key, so an idempotent handler records the effect exactly once.
//   5. ONE-SHOT vs PERIODIC: a one-shot row is removed after firing; a periodic row re-arms at
//      scheduled+period (no drift) and fires again on the next period.
//   6. WALL-CLOCK domain is civil time (ADR-017 C1): pal::wall_now() is a distinct type from the
//      monotonic pal::now(), and reads real civil time (post-2020), not boot-relative time.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "tmp_dir_util.hpp"

#include "pal/pal.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

constexpr std::int64_t kSec = 1'000'000'000LL;

ActorId actor(std::uint64_t k) { return ActorId{TypeKey{7}, k}; }

// Identifies one reminder occurrence across actors for the test's loss accounting. The per-actor
// idempotency dedup key is (name, scheduled_due) — the handler runs in the actor's context, so the
// actor is implicit there — but to COUNT distinct reminders across many actors in a test we must
// include the actor key too (else N reminders sharing a name+due collapse to one).
struct Dedup {
    std::uint64_t actor_key;
    std::uint64_t name_hash;
    std::int64_t scheduled_due_ns;
    friend bool operator<(const Dedup& a, const Dedup& b) noexcept {
        if (a.actor_key != b.actor_key) return a.actor_key < b.actor_key;
        if (a.name_hash != b.name_hash) return a.name_hash < b.name_hash;
        return a.scheduled_due_ns < b.scheduled_due_ns;
    }
};

}  // namespace

int main() {
    bool ok = true;

    // === 1. MASS-DUE FLATTENS + determinism ===================================================
    {
        constexpr std::size_t N = 200'000;
        constexpr std::uint32_t kFireRate = 10'000;                 // reminders/sec
        const std::int64_t due = 100'000 * kSec;                    // one civil instant

        auto run_wave = [&](std::vector<std::size_t>& hist, std::vector<std::uint64_t>& order) {
            InMemoryReminderStore store;
            ReminderConfig cfg;
            cfg.bucket_ns = kSec;
            cfg.fire_rate = kFireRate;
            cfg.credit = kFireRate;                                 // per-tick cap == rate ⇒ peak==rate
            std::size_t this_bucket = 0;
            ReminderService<InMemoryReminderStore> svc(
                store, [&](const FireEvent& e) { ++this_bucket; order.push_back(e.name_hash); }, cfg);
            svc.open();
            for (std::size_t i = 0; i < N; ++i)
                (void)svc.remind_at(actor(i), "wave", WallInstant{due}, 0, {});
            // Tick once per bucket until the whole segment has drained.
            for (int b = 0; b < 100 && svc.pending() > 0; ++b) {
                this_bucket = 0;
                svc.tick(WallInstant{due + static_cast<std::int64_t>(b) * kSec});
                hist.push_back(this_bucket);
            }
            return svc.fired_total();
        };

        std::vector<std::size_t> hist1, hist2;
        std::vector<std::uint64_t> order1, order2;
        std::uint64_t fired1 = run_wave(hist1, order1);
        std::uint64_t fired2 = run_wave(hist2, order2);

        std::size_t peak = 0, total = 0;
        for (std::size_t c : hist1) { peak = c > peak ? c : peak; total += c; }
        check(fired1 == N && total == N, "mass-due: all N fired exactly once", ok);
        // Peak per bucket must be the fire_rate (bounded drain), NOT the whole wave.
        check(peak <= kFireRate, "mass-due: peak per bucket <= fire_rate (flattened, not N)", ok);
        check(peak >= kFireRate * 9 / 10, "mass-due: drain runs at ~fire_rate (not trickling)", ok);
        check(peak * 10 <= N, "mass-due: peak is >=10x smaller than the naive spike N (flattened)", ok);
        // Deterministic smear: identical fire order + histogram across two independent runs.
        check(fired1 == fired2 && order1 == order2 && hist1 == hist2,
              "mass-due: deterministic smear (identical order + histogram across runs)", ok);
        std::fprintf(stderr,
                     "  [1] mass-due N=%zu fire_rate=%u -> peak/bucket=%zu (== fire_rate), "
                     "buckets=%zu, deterministic=%s\n",
                     N, kFireRate, peak, hist1.size(), (order1 == order2 ? "yes" : "NO"));
    }

    // === 2. O(due-now): dormant population is not fired / not scanned =========================
    {
        InMemoryReminderStore store;
        ReminderConfig cfg;
        cfg.bucket_ns = kSec;
        cfg.fire_rate = 0;  // fast path: fire everything due now
        std::size_t fired = 0;
        ReminderService<InMemoryReminderStore> svc(store, [&](const FireEvent&) { ++fired; }, cfg);
        svc.open();
        const std::int64_t now = 50'000 * kSec;
        for (std::size_t i = 0; i < 100'000; ++i)                 // dormant: due FAR in the future
            (void)svc.remind_at(actor(i), "later", WallInstant{now + 1'000'000 * kSec}, 0, {});
        for (std::size_t i = 0; i < 10; ++i)                      // 10 due now
            (void)svc.remind_at(actor(1'000'000 + i), "now", WallInstant{now}, 0, {});
        std::size_t f = svc.tick(WallInstant{now});
        check(f == 10 && fired == 10, "O(due-now): only the 10 due fire, 100k dormant untouched", ok);
        check(svc.pending() == 100'000, "O(due-now): 100k dormant remain pending", ok);
        std::fprintf(stderr, "  [2] due-now fired=%zu, dormant pending=%zu\n", f, svc.pending());
    }

    // === 3. DURABLE + AT-LEAST-ONCE, ZERO LOSS across a crash =================================
    {
        namespace fs = std::filesystem;
        const std::string dir = quark::test::make_temp_dir("quark_reminder_");
        const std::string path = dir + "/reminders.qrem";

        constexpr std::size_t N = 2'000;
        const std::int64_t due = 100'000 * kSec;

        // Register N reminders durably, then tick PARTWAY (drain some), then "crash".
        std::set<Dedup> fired_before;
        {
            FileReminderStore store(path);
            ReminderConfig cfg;
            cfg.bucket_ns = kSec;
            cfg.fire_rate = 200;  // drains over ~10 buckets
            cfg.credit = 200;
            ReminderService<FileReminderStore> svc(
                store, [&](const FireEvent& e) { fired_before.insert({e.actor.key, e.name_hash, e.scheduled_due_ns}); }, cfg);
            svc.open();
            for (std::size_t i = 0; i < N; ++i)
                (void)svc.remind_at(actor(i), "charge", WallInstant{due}, 0, {});
            // Drain only ~3 buckets, then drop everything (crash) with the wave unfinished.
            for (int b = 0; b < 3; ++b) svc.tick(WallInstant{due + static_cast<std::int64_t>(b) * kSec});
            check(svc.pending() > 0, "durable: crash mid-wave (work remained)", ok);
        }
        check(fired_before.size() < N, "durable: not all fired before the crash", ok);

        // REOPEN from the same file. Nothing committed may be lost — the remaining reminders must
        // still be durable and fire. Handler is idempotent on (name, scheduled_due).
        std::set<Dedup> effects;
        std::uint64_t total_fires = 0;
        {
            FileReminderStore store(path);
            ReminderConfig cfg;
            cfg.bucket_ns = kSec;
            cfg.fire_rate = 500;
            cfg.credit = 500;
            ReminderService<FileReminderStore> svc(
                store, [&](const FireEvent& e) { effects.insert({e.actor.key, e.name_hash, e.scheduled_due_ns}); ++total_fires; }, cfg);
            svc.open();
            check(svc.replayed() == N - fired_before.size(),
                  "durable: reopen replays exactly the un-resolved rows", ok);
            for (int b = 0; b < 20 && svc.pending() > 0; ++b)
                svc.tick(WallInstant{due + static_cast<std::int64_t>(100 + b) * kSec});
            check(svc.pending() == 0, "durable: drained to empty after reopen", ok);
        }

        // Union of pre-crash and post-crash effects must cover ALL N distinct reminders — zero loss.
        std::set<Dedup> all = fired_before;
        all.insert(effects.begin(), effects.end());
        check(all.size() == N, "durable: ZERO committed reminders lost across the crash", ok);
        std::fprintf(stderr,
                     "  [3] durable: N=%zu, fired_before_crash=%zu, replayed+fired_after=%zu, "
                     "union=%zu (== N ⇒ 0 lost)\n",
                     N, fired_before.size(), effects.size(), all.size());
        fs::remove_all(dir);
    }

    // === 4. IDEMPOTENT re-fire — a duplicate delivery yields exactly one effect ================
    {
        InMemoryReminderStore store;
        ReminderConfig cfg;
        cfg.bucket_ns = kSec;
        cfg.fire_rate = 0;
        std::unordered_map<std::uint64_t, int> effect_count;  // keyed by the dedup (name_hash^due)
        std::uint64_t deliveries = 0;
        auto handler = [&](const FireEvent& e) {
            ++deliveries;
            // Idempotent handler (017): apply the effect at most once per (name, scheduled_due).
            std::uint64_t dk = e.name_hash ^ static_cast<std::uint64_t>(e.scheduled_due_ns);
            effect_count[dk] += (effect_count.count(dk) ? 0 : 1);
        };
        ReminderService<InMemoryReminderStore> svc(store, handler, cfg);
        svc.open();
        const std::int64_t due = 10 * kSec;
        (void)svc.remind_at(actor(1), "dup", WallInstant{due}, 0, {});
        svc.tick(WallInstant{due});                  // first (normal) fire
        // Simulate the at-least-once re-fire a crash-before-confirm would cause: re-register the SAME
        // (actor,name,scheduled_due) and fire again. The dedup key is identical ⇒ one effect.
        (void)svc.remind_at(actor(1), "dup", WallInstant{due}, 0, {});
        svc.tick(WallInstant{due});                  // duplicate fire
        check(deliveries == 2, "idempotent: reminder was delivered twice (at-least-once)", ok);
        check(effect_count.size() == 1, "idempotent: exactly one distinct dedup key", ok);
        int applied = 0;
        for (auto& [k, v] : effect_count) applied += v;
        check(applied == 1, "idempotent: the effect applied exactly once despite two deliveries", ok);
        std::fprintf(stderr, "  [4] idempotent: deliveries=%llu, distinct effects=%d\n",
                     static_cast<unsigned long long>(deliveries), applied);
    }

    // === 5. one-shot removed; periodic re-arms at scheduled+period (no drift) ==================
    {
        InMemoryReminderStore store;
        ReminderConfig cfg;
        cfg.bucket_ns = kSec;
        cfg.fire_rate = 0;
        std::vector<std::int64_t> periodic_dues;
        ReminderService<InMemoryReminderStore> svc(
            store, [&](const FireEvent& e) { if (e.name == "tick") periodic_dues.push_back(e.scheduled_due_ns); }, cfg);
        svc.open();
        const std::int64_t due = 5 * kSec;
        (void)svc.remind_at(actor(1), "oneshot", WallInstant{due}, 0, {});
        (void)svc.remind_at(actor(2), "tick", WallInstant{due}, 2 * kSec, {});  // period 2s
        svc.tick(WallInstant{due});
        check(svc.pending() == 1, "lifecycle: one-shot removed, periodic remains", ok);
        // Fire the periodic across three more periods; scheduled dues advance by exactly the period.
        svc.tick(WallInstant{due + 2 * kSec});
        svc.tick(WallInstant{due + 4 * kSec});
        bool advanced = periodic_dues.size() >= 3 && periodic_dues[0] == due &&
                        periodic_dues[1] == due + 2 * kSec && periodic_dues[2] == due + 4 * kSec;
        check(advanced, "lifecycle: periodic re-arms at scheduled+period with no drift", ok);
        std::fprintf(stderr, "  [5] one-shot removed; periodic dues advanced by exact period\n");
    }

    // === 6. wall-clock domain is civil time, distinct from the monotonic deadline clock ========
    {
        static_assert(!std::is_same_v<pal::clock, pal::wall_clock>,
                      "reminder wall clock must differ from the monotonic deadline clock (ADR-017 C1)");
        static_assert(!pal::wall_clock::is_steady, "wall clock follows civil steps (not steady)");
        static_assert(pal::clock::is_steady, "deadline clock is monotonic (steady)");
        const std::int64_t civil = wall_now().ns;
        // CLOCK_REALTIME now is > 2020 (~1.6e18 ns since epoch); CLOCK_BOOTTIME (uptime) is far smaller.
        check(civil > 1'600'000'000LL * kSec, "wall-clock: reads real civil time (post-2020)", ok);
        check(static_cast<std::int64_t>(pal::now().time_since_epoch().count()) < civil,
              "wall-clock: boot-relative monotonic time is far below civil time", ok);
        std::fprintf(stderr, "  [6] wall civil ns=%lld, boot ns=%lld (distinct domains)\n",
                     static_cast<long long>(civil),
                     static_cast<long long>(pal::now().time_since_epoch().count()));
    }

    std::fprintf(stderr, "reminder_segstream_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
