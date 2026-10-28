// Tests 011-Timers-and-Scheduled-Work §Advancing the clock + §Cancellation — review D2: the timer
// service must NEVER run user code under its mutex. A fired timer's `tell` copies the stored message
// and recycling destroys it; both can run user code (copy-ctor / destructor / lazy activation). Held
// under `mu_`, that (a) deadlocks if the code calls cancel()/schedule() on the same service and
// (b) stalls every schedule()/cancel() from other threads for the whole of a long catch-up.
//
//  (a) re-entrancy: a message copy-ctor/destructor that cancel()s and schedule()s on the same service
//      completes (a watchdog turns a deadlock into a FAIL instead of a hung CI job).
//  (b) a long catch-up (many slow fires) does not block concurrent schedule()/cancel() — measured with
//      a deliberately generous bound (stall would be ~the whole catch-up; the bound is a fraction).
//  (c) at-most-once under cancel races: deterministic in-flight cancel of one-shot/periodic, stale-
//      handle safety after slot reuse, and a threaded cancel-vs-advance race with per-id fire counts.
//
// Own sink actor (not timertest::Harness): the message type here carries the re-entrant hooks.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/timer_service.hpp"
#include "timer_test_util.hpp"

using namespace quark;
using namespace std::chrono_literals;
using timertest::check;

namespace {

struct Hooked;
using HookFn = void (*)(const Hooked&, bool is_copy);
std::atomic<HookFn> g_hook{nullptr};

// A message whose copy-ctor (the fire-time `tell` copy) and destructor (recycle / mailbox reclaim)
// call a test hook, like a user type that touches the timer service from its special members.
struct Hooked {
    int id = 0;
    int mode = 0;
    bool live = true;  // false once moved-from (a moved-from shell must not trigger hooks)

    Hooked(int i, int m) : id(i), mode(m) {}
    Hooked(const Hooked& o) : id(o.id), mode(o.mode) {
        if (HookFn h = g_hook.load(std::memory_order_acquire)) h(*this, true);
    }
    Hooked(Hooked&& o) noexcept : id(o.id), mode(o.mode) { o.live = false; }
    Hooked& operator=(const Hooked&) = delete;
    ~Hooked() {
        if (!live) return;
        if (HookFn h = g_hook.load(std::memory_order_acquire)) h(*this, false);
    }
};

constexpr int kMaxId = 4096;

struct Sink : Actor<Sink, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.test.timer_service_reentrancy_test.Sink";
    using protocol = Protocol<Hooked>;
    std::atomic<int> got[kMaxId]{};
    std::atomic<int> total{0};
    void handle(const Hooked& h) noexcept {
        got[h.id].fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_release);
    }
};

class Rig {
public:
    Rig()
        : pool_(8192),
          act_(std::make_unique<Activation>(&actor_, Sink::dispatch_table(), pool_.sink())),
          eng_(EngineConfig{1, 1, 256, 64}),
          router_(eng_.post_courier(), pool_) {
        eng_.register_activation(actor_id_of<Sink>(1), *act_);
        eng_.start();
    }
    ~Rig() { eng_.stop(); }
    [[nodiscard]] ActorRef<Sink> ref() { return router_.get<Sink>(1); }
    [[nodiscard]] Sink& sink() noexcept { return actor_; }
    [[nodiscard]] bool wait_total(int want) {
        const auto end = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < end) {
            if (actor_.total.load(std::memory_order_acquire) >= want) return true;
            std::this_thread::yield();
        }
        return actor_.total.load(std::memory_order_acquire) >= want;
    }
    [[nodiscard]] int got(int id) const { return actor_.got[id].load(std::memory_order_acquire); }

private:
    detail::MessagePool pool_;
    Sink actor_;
    std::unique_ptr<Activation> act_;
    Engine<> eng_;
    LocalRouter router_;
};

// Run `fn` on a thread; if it has not finished within `limit`, the service is deadlocked -> FAIL+exit.
template <class Fn>
void with_watchdog(const char* what, std::chrono::seconds limit, Fn fn) {
    std::promise<void> done;
    auto fut = done.get_future();
    std::thread t([&] {
        fn();
        done.set_value();
    });
    if (fut.wait_for(limit) != std::future_status::ready) {
        std::fprintf(stderr, "timer_service_reentrancy_test: FAIL  DEADLOCK/HANG in: %s\n", what);
        std::fflush(stderr);
        std::_Exit(1);
    }
    t.join();
}

// Shared hook state (set before the hook is armed; read from whichever thread runs the hook).
std::mutex g_mu;
TimerService* g_svc = nullptr;
ActorRef<Sink>* g_ref = nullptr;
TimerHandle g_victim;
TimerHandle g_self;
std::atomic<int> g_hook_calls{0};
std::atomic<bool> g_in_slow_fire{false};

void reenter_hook(const Hooked& h, bool is_copy) {
    if (h.mode != 1) return;
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    TimerHandle v;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        v = g_victim;
    }
    v.cancel();  // re-enters cancel_entry from inside a fire (copy) / recycle (destructor)
    if (is_copy)  // re-enters arm from inside a fire
        (void)g_svc->schedule_after(*g_ref, 1ms, Hooked{2, 0});
}

void slow_hook(const Hooked& h, bool is_copy) {
    if (h.mode != 2 || !is_copy) return;
    g_in_slow_fire.store(true, std::memory_order_release);
    std::this_thread::sleep_for(2ms);  // a slow tell (lazy activation / user copy-ctor)
}

void self_cancel_hook(const Hooked& h, bool is_copy) {
    if (h.mode != 3 || !is_copy) return;
    TimerHandle s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_self;
    }
    s.cancel();  // cancel my own timer while it is being fired (in flight)
}

}  // namespace

int main() {
    bool ok = true;

    // ---- (a) re-entrancy: copy-ctor + destructor call cancel()/schedule() on the same service ------
    {
        Rig rig;
        TimerService svc(TimerService::Config{1ms, 1024});
        ActorRef<Sink> ref = rig.ref();
        g_svc = &svc;
        g_ref = &ref;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_victim = svc.schedule_after(ref, 100ms, Hooked{900, 0});
        }
        (void)svc.schedule_after(ref, 1ms, Hooked{1, 1});
        g_hook.store(&reenter_hook, std::memory_order_release);
        with_watchdog("(a) fire/recycle re-enters cancel()/schedule()", 20s, [&] {
            svc.advance_ticks(5);    // fires id1: its copy + destructor re-enter the service
            svc.advance_ticks(200);  // would fire the victim (id 900) if the cancel had been lost
        });
        g_hook.store(nullptr, std::memory_order_release);
        check(rig.wait_total(2), "(a) id1 and the timer scheduled from inside its fire both delivered", ok);
        std::this_thread::sleep_for(50ms);
        check(rig.got(1) == 1, "(a) re-entrant message delivered exactly once", ok);
        check(rig.got(2) == 1, "(a) timer scheduled from inside a fire delivered exactly once", ok);
        check(rig.got(900) == 0, "(a) victim cancelled from inside a fire never fired", ok);
        check(g_hook_calls.load() >= 2, "(a) hook ran in both copy-ctor and destructor", ok);
    }

    // ---- (b) a long catch-up must not block schedule()/cancel() from other threads ----------------
    {
        Rig rig;
        TimerService svc(TimerService::Config{1ms, 2048});
        ActorRef<Sink> ref = rig.ref();
        constexpr int kSlow = 400;  // 400 ticks * ~2 ms per slow fire ~= 0.8 s+ of catch-up
        for (int i = 0; i < kSlow; ++i)
            (void)svc.schedule_after(ref, std::chrono::milliseconds(i + 1), Hooked{10 + i, 2});
        g_hook.store(&slow_hook, std::memory_order_release);
        g_in_slow_fire.store(false);

        std::atomic<bool> catchup_done{false};
        std::chrono::nanoseconds max_stall{0};
        std::thread contender([&] {
            while (!g_in_slow_fire.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < 20 && !catchup_done.load(std::memory_order_acquire); ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                TimerHandle h = svc.schedule_after(ref, 10s, Hooked{3000, 0});
                h.cancel();
                const auto dt = std::chrono::steady_clock::now() - t0;
                if (dt > max_stall) max_stall = std::chrono::duration_cast<std::chrono::nanoseconds>(dt);
                std::this_thread::sleep_for(5ms);
            }
        });
        const auto t0 = std::chrono::steady_clock::now();
        with_watchdog("(b) long catch-up", 60s, [&] { svc.advance_ticks(kSlow + 5); });
        const auto catchup = std::chrono::steady_clock::now() - t0;
        catchup_done.store(true, std::memory_order_release);
        contender.join();
        g_hook.store(nullptr, std::memory_order_release);
        check(rig.wait_total(kSlow), "(b) every catch-up timer fired", ok);

        const auto stall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_stall).count();
        const auto catch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(catchup).count();
        std::printf("  (b) catch-up %lld ms, worst schedule+cancel stall %lld ms\n",
                    static_cast<long long>(catch_ms), static_cast<long long>(stall_ms));
        // Held across the catch-up the stall would be ~catch_ms; allow a quarter of it (and >=100 ms).
        check(catch_ms >= 400, "(b) the catch-up really was long (precondition)", ok);
        check(stall_ms < catch_ms / 4 && stall_ms < 400, "(b) schedule/cancel not blocked by catch-up", ok);
    }

    // ---- (c) at-most-once under cancel races ------------------------------------------------------
    {
        Rig rig;
        TimerService svc(TimerService::Config{1ms, 1024});
        ActorRef<Sink> ref = rig.ref();

        // c1: a one-shot cancelled while in flight (from its own copy-ctor) fires exactly once.
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_self = svc.schedule_after(ref, 1ms, Hooked{20, 3});
        }
        g_hook.store(&self_cancel_hook, std::memory_order_release);
        with_watchdog("(c1)", 20s, [&] { svc.advance_ticks(10); });
        g_hook.store(nullptr, std::memory_order_release);
        check(rig.wait_total(1), "(c1) in-flight one-shot delivered", ok);
        check(rig.got(20) == 1, "(c1) one-shot cancelled in flight fires at most once", ok);

        // c2: a periodic cancelled while in flight fires once more at most, then never re-arms.
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_self = svc.schedule_every(ref, 2ms, Hooked{21, 3});
        }
        g_hook.store(&self_cancel_hook, std::memory_order_release);
        with_watchdog("(c2)", 20s, [&] { svc.advance_ticks(50); });
        g_hook.store(nullptr, std::memory_order_release);
        check(rig.wait_total(2), "(c2) periodic's in-flight fire delivered", ok);
        std::this_thread::sleep_for(20ms);
        check(rig.got(21) == 1, "(c2) periodic cancelled in flight: one fire, no re-arm", ok);

        // c3: a stale handle must not cancel a newer timer that reused the recycled slot.
        TimerHandle old = svc.schedule_after(ref, 1ms, Hooked{22, 0});
        svc.advance_ticks(3);  // fires + recycles; slot returns to the pool
        TimerHandle fresh = svc.schedule_after(ref, 1ms, Hooked{23, 0});
        old.cancel();          // stale gen: must be a no-op even if `fresh` reused the slot
        svc.advance_ticks(3);
        check(rig.wait_total(4), "(c3) both timers delivered", ok);
        check(rig.got(22) == 1 && rig.got(23) == 1, "(c3) stale cancel did not kill the reused slot", ok);
        (void)fresh;

        // c4: threaded cancel-vs-advance race (one-shot + periodic): no id ever delivered twice for
        // a one-shot; a cancelled periodic stops once cancel() has returned AND the in-flight tick drained.
        constexpr int kRounds = 100, kPer = 16;
        int base = 100;
        for (int r = 0; r < kRounds; ++r) {
            std::vector<TimerHandle> hs;
            for (int i = 0; i < kPer; ++i)
                hs.push_back(svc.schedule_after(ref, 1ms, Hooked{base + i, 0}));
            std::atomic<bool> go{false};
            std::thread canceller([&] {
                while (!go.load(std::memory_order_acquire)) {}
                for (auto& h : hs) h.cancel();
            });
            go.store(true, std::memory_order_release);
            with_watchdog("(c4)", 20s, [&] { svc.advance_ticks(3); });
            canceller.join();
            base += kPer;
            if (r % 50 == 49) {
                for (int id = 100; id < kMaxId; ++id)
                    if (rig.got(id) > 1) { check(false, "(c4) one-shot delivered twice", ok); break; }
            }
        }
        TimerHandle per = svc.schedule_every(ref, 1ms, Hooked{4000, 0});
        svc.advance_ticks(5);
        per.cancel();
        svc.advance_ticks(1);  // drains any in-flight batch that was popped before the cancel landed
        for (auto end = std::chrono::steady_clock::now() + 10s;
             rig.got(4000) < 4 && std::chrono::steady_clock::now() < end;)
            std::this_thread::yield();  // the fires were popped before the cancel; let them land
        std::this_thread::sleep_for(20ms);
        const int after_cancel = rig.got(4000);
        svc.advance_ticks(20);
        std::this_thread::sleep_for(20ms);
        check(after_cancel >= 4 && after_cancel <= 5, "(c4) periodic fired once per tick until cancelled", ok);
        check(rig.got(4000) == after_cancel, "(c4) cancelled periodic never fires again", ok);
    }

    std::printf("timer_service_reentrancy_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
