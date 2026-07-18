// Quark sample 14 — Durable reminders (027 / SEGSTREAM, decisions/ADR-017).
//
// A TIMER (sample 04, spec 011) is in-memory, monotonic, and gone the instant the actor deactivates
// or the process restarts — right for a retry backoff. A REMINDER is the durable sibling: it is
// persisted, scheduled against WALL-CLOCK / civil time ("21:00"), survives a restart AND actor
// passivation, and is delivered at-least-once. Right for "charge this subscription at 9 PM",
// "expire this session tomorrow", "run end-of-day settlement".
//
// This sample drives the service DETERMINISTICALLY with `tick(WallInstant)` (no wall-clock sleeps),
// exactly as sample 04 drives the timer wheel with `advance_ticks` — so the output is identical every
// run. In production you call `svc.tick(quark::wall_now())` from the owner node's loop.
//
// What it shows:
//   1. a one-shot reminder firing as a `tell`-style delivery, then being durably resolved (removed);
//   2. a periodic reminder re-arming at scheduled+period (no drift);
//   3. DURABILITY — register on a crash-durable FileReminderStore, "restart" the process (reopen the
//      store), and the un-fired reminder still fires — nothing committed is lost;
//   4. the MASS-DUE / 9 PM problem in miniature — 10,000 reminders all due at one instant FLATTEN to
//      a peak of `fire_rate` per second, never a 10,000-wide spike (ADR-017 C4p).
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 14_durable_reminders
// Run  :  taskset -c 0-3 build/samples/14_durable_reminders
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

using namespace quark;

namespace {
constexpr std::int64_t kSec = 1'000'000'000LL;
ActorId user(std::uint64_t id) { return ActorId{TypeKey{1}, id}; }

// A civil instant helper: "day D at second S" — just illustrative arithmetic on a WallInstant.
WallInstant at(std::int64_t seconds_since_epoch) { return WallInstant{seconds_since_epoch * kSec}; }
}  // namespace

int main() {
    std::printf("Quark sample 14 — durable reminders (027 / SEGSTREAM)\n");
    std::printf("=====================================================\n\n");

    // --- 1 & 2: one-shot + periodic on the in-memory reference store ------------------------------
    {
        InMemoryReminderStore store;
        // The fire callback models the `tell` that lands on the actor's own lane (011 delivery). In a
        // real engine this reactivates a passivated actor (ADR-008) and delivers `payload` as a message.
        auto deliver = [](const FireEvent& e) {
            std::printf("   FIRE  actor=%llu  \"%.*s\"  scheduled@%llds  (dedup key = name+scheduled)\n",
                        static_cast<unsigned long long>(e.actor.key),
                        static_cast<int>(e.name.size()), e.name.data(),
                        static_cast<long long>(e.scheduled_due_ns / kSec));
        };
        ReminderConfig cfg;                 // fire_rate 0 ⇒ fire everything due immediately (low volume)
        ReminderService<InMemoryReminderStore> svc(store, deliver, cfg);
        svc.open();

        std::printf("1/2. one-shot + periodic:\n");
        (void)svc.remind_at(user(7), "expire-session", at(1000), /*period*/ 0, {});          // one-shot
        (void)svc.remind_at(user(7), "heartbeat", at(1000), /*period*/ 3 * kSec, {});         // every 3s

        svc.tick(at(1000));                 // both due now
        std::printf("   after first tick: pending=%zu (one-shot resolved, periodic re-armed)\n", svc.pending());
        svc.tick(at(1003));                 // periodic fires again, one-shot does NOT
        svc.tick(at(1006));
        std::printf("   periodic advanced by exactly its period each time; one-shot fired once.\n\n");
    }

    // --- 3: DURABILITY across a restart ----------------------------------------------------------
    {
        namespace fs = std::filesystem;
        char tmpl[] = "/tmp/quark_sample14_XXXXXX";
        const std::string dir = ::mkdtemp(tmpl);
        const std::string path = dir + "/reminders.qrem";

        std::printf("3. durability across a restart (FileReminderStore at %s):\n", path.c_str());
        // --- process instance #1: register a reminder due "tomorrow", then crash before it fires ---
        {
            FileReminderStore store(path);
            ReminderService<FileReminderStore> svc(store, [](const FireEvent&) {}, {});
            svc.open();
            (void)svc.remind_at(user(42), "charge-subscription", at(90'000), 0, {});
            std::printf("   instance #1: registered 'charge-subscription' (durable), then EXIT before it fires.\n");
        }  // store + service destroyed == process exit; the .qrem file (fdatasync'd) persists

        // --- process instance #2: fresh start, reopen the SAME store — the reminder is still there ---
        {
            bool fired = false;
            FileReminderStore store(path);
            ReminderService<FileReminderStore> svc(
                store, [&](const FireEvent& e) {
                    fired = true;
                    std::printf("   instance #2: reminder survived restart and FIRED — \"%.*s\"\n",
                                static_cast<int>(e.name.size()), e.name.data());
                }, {});
            svc.open();
            std::printf("   instance #2: reopened store, replayed %zu pending reminder(s).\n", svc.replayed());
            svc.tick(at(90'000));           // civil time reaches the due instant
            std::printf("   → nothing was lost across the restart: fired=%s\n\n", fired ? "yes" : "NO");
        }
        fs::remove_all(dir);
    }

    // --- 4: the 9 PM / mass-due problem in miniature ---------------------------------------------
    {
        constexpr std::size_t N = 10'000;
        constexpr std::uint32_t kFireRate = 500;    // reminders/sec drain ⇒ spread ≈ 20 s
        const std::int64_t nine_pm = 200'000;       // one civil instant (seconds)

        InMemoryReminderStore store;
        ReminderConfig cfg;
        cfg.bucket_ns = kSec;
        cfg.fire_rate = kFireRate;                  // THE spread lever: peak == fire_rate, not N
        cfg.credit = kFireRate;
        std::size_t this_second = 0, peak = 0;
        ReminderService<InMemoryReminderStore> svc(store, [&](const FireEvent&) { ++this_second; }, cfg);
        svc.open();

        std::printf("4. mass-due (the 9 PM problem), %zu reminders all due at one instant:\n", N);
        for (std::size_t i = 0; i < N; ++i) (void)svc.remind_at(user(i), "daily-report", at(nine_pm), 0, {});

        std::size_t buckets = 0;
        for (int b = 0; svc.pending() > 0 && b < 1000; ++b) {
            this_second = 0;
            svc.tick(at(nine_pm + b));
            peak = this_second > peak ? this_second : peak;
            ++buckets;
        }
        std::printf("   naive would be a %zu-wide spike in ONE second.\n", N);
        std::printf("   SEGSTREAM drained %zu fires over %zu seconds at PEAK %zu/s (== fire_rate %u).\n",
                    N, buckets, peak, kFireRate);
        std::printf("   → the wave FLATTENED %.0fx instead of melting the node.\n\n",
                    static_cast<double>(N) / static_cast<double>(peak));
    }

    std::printf("See 027-Reminders.md and decisions/ADR-017 for the full model + proof.\n");
    return 0;
}
