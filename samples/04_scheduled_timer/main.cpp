// Quark sample 04 — Scheduled & periodic messages (011 timers).
//
// The TimerService turns time into messages: `schedule_after` sends one `tell` after a delay,
// `schedule_every` re-sends it on a period, and a returned handle cancels future fires. It delivers
// through the same `ActorRef`/`tell` seam as everything else — a timer is just a deferred send.
//
// This sample drives the wheel DETERMINISTICALLY with `advance_ticks(n)` (no wall-clock sleeps), so
// the output is identical every run. In production the engine advances the wheel from real `pal`
// time; the deterministic driver is what the 014 sim and these demos use.
//
// What it shows:
//   * a one-shot timer that does NOT fire before its delay, and DOES fire after it
//   * a periodic timer firing repeatedly, then a cancel() that stops all future fires
//   * reading the actor's state back with an `ask` (which FIFO-follows the fired tells)
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 04_scheduled_timer
// Run  :  taskset -c 0-3 build/samples/04_scheduled_timer
#include <chrono>
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/timer_service.hpp"

using namespace quark;
using namespace std::chrono_literals;

struct Tick {};
struct Count {};

// Counts how many timer fires it has received; answers the count on demand.
struct Alarm : Actor<Alarm, Sequential> {
    using protocol = Protocol<Tick, Ask<Count, int>>;

    void handle(const Tick&) noexcept { ++fires_; }
    void handle(const Ask<Count, int>& m) noexcept { m.respond(fires_); }

private:
    int fires_ = 0;
};

// Small helper: after driving the wheel we read the actor's fire count via an ask (which serializes
// after all fired tells — the mailbox is FIFO).
static int read_count(ActorRef<Alarm>& ref) {
    result<int> r = block_on(ref.ask<int>(Count{}));
    return r.has_value() ? r.value() : -1;
}

int main() {
    detail::MessagePool pool(1024);
    Alarm alarm;
    auto activation = std::make_unique<Activation>(&alarm, Alarm::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Alarm>(1), *activation);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Alarm> ref = router.get<Alarm>(1);
    eng.start();

    // A 1 ns tick makes "ticks" == "nanoseconds of delay", so the numbers read directly.
    TimerService timers(TimerService::Config{/*tick*/ 1ns, /*entry_capacity*/ 256});

    bool ok = true;

    // --- One-shot: fire a single Tick after a 50-tick delay. ------------------------------------
    auto oneshot = timers.schedule_after(ref, 50ns, Tick{});
    (void)oneshot;

    timers.advance_ticks(40);                 // short of the delay
    ok &= (read_count(ref) == 0);
    std::printf("one-shot at t=40 (delay 50): fires=%d  (expected 0, not yet due)\n", read_count(ref));

    timers.advance_ticks(15);                 // past the delay (+ wheel fencepost slack)
    ok &= (read_count(ref) == 1);
    std::printf("one-shot at t=55:            fires=%d  (expected 1, fired once)\n", read_count(ref));

    // --- Periodic: fire every 10 ticks, then cancel and prove future fires stop. ----------------
    auto periodic = timers.schedule_every(ref, 10ns, Tick{});

    timers.advance_ticks(35);                 // ~3 more fires (at +10, +20, +30)
    const int after_periodic = read_count(ref);
    std::printf("periodic every 10, +35 ticks: fires=%d  (1 one-shot + 3 periodic = 4)\n",
                after_periodic);
    ok &= (after_periodic == 4);

    periodic.cancel();                        // lazy tombstone: no more fires scheduled
    timers.advance_ticks(100);                // plenty of time — but the timer is cancelled
    const int after_cancel = read_count(ref);
    std::printf("after cancel, +100 ticks:     fires=%d  (unchanged — cancel stopped it)\n",
                after_cancel);
    ok &= (after_cancel == after_periodic);

    eng.stop();
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
