// 027 durable-reminder benchmark — re-measures the load-bearing ADR-017 scale claim from scratch on
// THIS machine (not trusting the debate transcript): the 10⁶-at-one-instant mass-due wave FLATTENS to
// a peak of `fire_rate` per bucket (not N), the per-tick idle scan is O(due-now) (flat across dormant
// population), and register/cancel is one cheap op (one fdatasync on the durable store).
//
// MACHINE SAFETY: the 10⁶ wave is DATA, driven SINGLE-THREADED. Run pinned to ONE core:
//     cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4 --target reminder_bench
//     taskset -c 0 build/bench/reminder_bench
// Never run unpinned / multi-core (ADR-017 machine-safety constraint).
//
// Timing uses pal::now() (monotonic BootClock) to measure elapsed REAL time of the ops; the reminders
// themselves are scheduled against WallInstant (civil time) and driven by an explicit per-bucket tick
// — the deterministic driver, no wall-clock sleeps (ADR-017 C1).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "pal/pal.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

using namespace quark;

namespace {
constexpr std::int64_t kSec = 1'000'000'000LL;

double ns_since(pal::clock::time_point t0) {
    return static_cast<double>((pal::now() - t0).count());
}
ActorId actor(std::uint64_t k) { return ActorId{TypeKey{7}, k}; }
}  // namespace

int main() {
    std::printf("== 027 reminder benchmark (SEGSTREAM / ADR-017) — single core, taskset -c 0 ==\n\n");

    // ---------------------------------------------------------------------------------------------
    // A. MASS-DUE FLATTEN — the 9 PM / one-million-reminders gate.
    //    N reminders all due at ONE civil instant; drained at fire_rate. Peak/bucket must be
    //    fire_rate (bounded), never N. spread = ceil(N / fire_rate) buckets (ADR-017 C4p).
    // ---------------------------------------------------------------------------------------------
    {
        constexpr std::size_t N = 1'000'000;
        constexpr std::uint32_t kFireRate = 3'333;   // ⇒ spread ≈ 300 s (ADR-017 reference numbers)
        const std::int64_t due = 1'000'000 * kSec;

        InMemoryReminderStore store;
        ReminderConfig cfg;
        cfg.bucket_ns = kSec;
        cfg.fire_rate = kFireRate;
        cfg.credit = 4096;                            // kCredit — per-tick / in-flight window
        std::size_t this_bucket = 0, peak = 0;
        ReminderService<InMemoryReminderStore> svc(store, [&](const FireEvent&) { ++this_bucket; }, cfg);
        svc.open();

        auto t0 = pal::now();
        for (std::size_t i = 0; i < N; ++i) (void)svc.remind_at(actor(i), "wave", WallInstant{due}, 0, {});
        double reg_ns = ns_since(t0);

        auto t1 = pal::now();
        std::size_t buckets = 0;
        for (int b = 0; svc.pending() > 0 && b < 100'000; ++b) {
            this_bucket = 0;
            svc.tick(WallInstant{due + static_cast<std::int64_t>(b) * kSec});
            peak = this_bucket > peak ? this_bucket : peak;
            ++buckets;
        }
        double drain_ns = ns_since(t1);

        std::printf("A. mass-due flatten:  N=%zu  fire_rate=%u/s  bucket=1s\n", N, kFireRate);
        std::printf("   register 10^6 rows : %.1f ms total  (%.0f ns/reminder, in-RAM index+store)\n",
                    reg_ns / 1e6, reg_ns / static_cast<double>(N));
        std::printf("   drain              : %zu buckets, %.1f ms wall\n", buckets, drain_ns / 1e6);
        std::printf("   PEAK fired / bucket : %zu   (== fire_rate %u ⇒ flattened %0.0fx vs naive spike N)\n",
                    peak, kFireRate, static_cast<double>(N) / static_cast<double>(peak));
        std::printf("   verdict            : %s (peak %s N)\n\n",
                    peak <= kFireRate + 1 ? "FLAT [goal]" : "SPIKE [MISS]",
                    peak <= kFireRate + 1 ? "<<" : "==");
    }

    // ---------------------------------------------------------------------------------------------
    // B. O(due-now) IDLE SCAN — a tick with NOTHING due must cost the same regardless of how many
    //    DORMANT reminders the node holds (ordered-map begin-peek is O(log S), not O(total)).
    // ---------------------------------------------------------------------------------------------
    {
        std::printf("B. O(due-now) idle-tick scan (nothing due; %s):\n", "cost must be flat vs dormant population");
        for (std::size_t pop : {std::size_t{1'000}, std::size_t{10'000}, std::size_t{100'000}, std::size_t{1'000'000}}) {
            InMemoryReminderStore store;
            ReminderConfig cfg;
            cfg.bucket_ns = kSec;
            cfg.fire_rate = 0;
            ReminderService<InMemoryReminderStore> svc(store, [](const FireEvent&) {}, cfg);
            svc.open();
            const std::int64_t future = 10'000'000 * kSec;
            for (std::size_t i = 0; i < pop; ++i) (void)svc.remind_at(actor(i), "d", WallInstant{future + static_cast<std::int64_t>(i) * kSec}, 0, {});
            // Time many idle ticks (now well before anything is due).
            constexpr int kTicks = 200'000;
            auto t0 = pal::now();
            for (int i = 0; i < kTicks; ++i) svc.tick(WallInstant{kSec + i});  // nothing due
            double per = ns_since(t0) / kTicks;
            std::printf("   dormant=%-9zu : %.1f ns/idle-tick  (fired 0, pending %zu)\n", pop, per, svc.pending());
        }
        std::printf("\n");
    }

    // ---------------------------------------------------------------------------------------------
    // C. DURABLE register/cancel — one fdatasync per op on the crash-durable FileReminderStore.
    // ---------------------------------------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        char tmpl[] = "/tmp/quark_rembench_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        const std::string path = std::string(dir) + "/r.qrem";
        constexpr std::size_t M = 20'000;

        FileReminderStore store(path);
        ReminderConfig cfg; cfg.bucket_ns = kSec; cfg.fire_rate = 0;
        ReminderService<FileReminderStore> svc(store, [](const FireEvent&) {}, cfg);
        svc.open();
        const std::int64_t due = 500'000 * kSec;

        auto t0 = pal::now();
        for (std::size_t i = 0; i < M; ++i) (void)svc.remind_at(actor(i), "charge", WallInstant{due}, 0, {});
        double put_ns = ns_since(t0) / M;

        auto t1 = pal::now();
        for (std::size_t i = 0; i < M; ++i) (void)svc.cancel(actor(i), "charge");
        double cancel_ns = ns_since(t1) / M;

        std::printf("C. durable FileReminderStore (%zu ops, on %s):\n", M, "/tmp — device-dependent");
        std::printf("   register (upsert)  : %.0f ns/op   (1 fdatasync/op)\n", put_ns);
        std::printf("   cancel (remove)    : %.0f ns/op   (1 fdatasync/op)\n", cancel_ns);
        std::printf("   note               : fsync cost is device-bound (tmpfs ~0.2us, ext4 ~2ms); the\n");
        std::printf("                        O(1) index work + exactly-one-sync-per-op is what's fixed.\n\n");
        fs::remove_all(dir);
    }

    std::printf("done. (numbers are tripwires on this VM, not the 023 reference stamp — ADR-017.)\n");
    return 0;
}
