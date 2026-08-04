// Tests 001-Actor-Execution-Model §Hybrid handler execution + ADR-015 §Parked seam — the
// park()-vs-complete_parked() lost-wakeup/UB race a red-team pass confirmed against the REAL
// Sequential drain_step()/complete_parked() (activation.hpp), and the fix (an acquire-gate in
// complete_parked() that blocks until it observes the exec-state release-store `park()` performs,
// establishing happens-before before touching the plain `parked_frame_`/`parked_desc_` members).
//
// Two threads drive a REAL `Activation` (no Engine/worker pool — exec_suspend_test's own style):
//   * Thread A ("the lane"): posts one message, acquires, and calls drain_step() on a handler whose
//     FIRST co_await commits to suspending — exactly like an ask()-driven handler that has already
//     won its own responder's win-arbitration before drain_step gets back to writing the parked
//     fields + calling exec_.park(). NO artificial wait/spin is added on this side; drain_step runs
//     exactly as production code does.
//   * Thread B ("the carrier"): the instant it observes the handler's suspend-commit signal (set at
//     the earliest legitimate moment — inside the awaiter's await_suspend, before drain_step's
//     suspend tail has necessarily run at all), calls Activation::complete_parked() with NO wait for
//     `state() == Parked` (that spin is exactly the test-harness workaround
//     tests/sched_readmit_test.cpp uses — deliberately NOT reproduced here).
//
// Timing is swept: most iterations run at native speed (the true race window is a handful of
// instructions); every Nth iteration widens the window with an injected microsecond delay
// (red-team's own technique — see park_race_probe.cpp) so the adversarial interleaving is exercised
// deterministically thousands of times, not left to timing luck.
//
// LIVENESS ORACLE (both builds): after both threads settle, the activation must never be left
// sealed in `Parked` with nobody signalled to re-admit it — either the resume genuinely completed
// AND the carrier's re-admit reported `wake == true` (state now `Scheduled`), or it did not.
//
// TEETH (this same file, compiled twice — see tests/CMakeLists.txt):
//   * activation_park_race_test        (this fix present): the oracle must hold on EVERY iteration,
//     including every widened-window one. A single violation is a hard FAIL.
//   * activation_park_race_control     (-DQUARK_ACTIVATION_PARK_NO_GATE): reverts complete_parked()
//     to the pre-fix shape (activation.hpp's `#ifndef QUARK_ACTIVATION_PARK_NO_GATE` guard) and asks
//     drain_step() to call a test-provided hook between the plain parked-field writes and
//     `exec_.park()` (also gated behind the SAME macro, zero cost otherwise) so the control can widen
//     the SAFE half of the window (fields already validly written, `park()`'s store not yet visible)
//     instead of the guaranteed-crash half (resuming a still-default-constructed, i.e. null,
//     coroutine_handle) — a deliberate choice so the control reports a clean, deterministic exit
//     instead of an uncontrolled SEGV. The pure-UB half (reading the parked fields before ANY write
//     landed) is already demonstrated by red-team's standalone `park_race_probe.cpp`
//     (429/429 stuck under a 5us injected delay against the real ExecStateCell) — cited as
//     corroborating evidence, not reproduced here with a real coroutine for exactly that reason.
//   The control must reproduce the permanent-stranding failure (silently dropped re-admit, activation
//   sealed in Parked forever) at least once, and confirm each stranded activation can truly never be
//   re-acquired again (a fresh message posted afterward is never admitted) — returns 0 iff it does
//   (same "must strand to prove the harness has teeth" convention as sched_no_lost_wakeup_test).
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <thread>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {

struct Ping {
    int tag;
};

// Fires the instant the handler's async op commits to suspending — the earliest legitimate moment a
// real carrier (e.g. an ask()'s responder) could race in. `delay_us`, when set, widens the window
// AFTER this signal and BEFORE control unwinds back into drain_step's suspend tail (redteam's
// technique) — used only by the (fixed) positive build, where it is safe because complete_parked()'s
// acquire-gate blocks a carrier that arrives this early.
struct CommitAwaiter {
    std::atomic<bool>* handoff_ready;
    int delay_us;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<>) const noexcept {
        handoff_ready->store(true, std::memory_order_relaxed);
        if (delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        return true;  // always suspend
    }
    void await_resume() const noexcept {}
};

struct Handler : Actor<Handler, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<bool> handoff_ready{false};
    std::atomic<int> completed{0};
    int awaiter_delay_us = 0;

    task<> handle(const Ping&) {
        co_await CommitAwaiter{&handoff_ready, awaiter_delay_us};
        completed.fetch_add(1, std::memory_order_release);
        co_return;
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

constexpr std::uint64_t kStallSpins = 2'000'000'000ULL;

// Spin until `flag` observes true (acquire), or bail out past a generous stall bound so a genuine
// regression reports a clean failure instead of an infinite hang. Returns false on stall.
bool spin_until(std::atomic<bool>& flag) {
    std::uint64_t spins = 0;
    while (!flag.load(std::memory_order_acquire)) {
        if (++spins > kStallSpins) return false;
    }
    return true;
}

}  // namespace

#if defined(QUARK_ACTIVATION_PARK_NO_GATE)
// CONTROL: the test-provided widen hook activation.hpp declares under this same macro. Runs on
// Thread A (the lane), sequenced right after the plain `parked_frame_`/`parked_desc_` writes and
// right before `exec_.park()` — see the file banner for why the control widens THIS half of the
// window (safe) rather than the pre-write half (a guaranteed null-coroutine-handle crash).
namespace quark {
namespace {
std::atomic<bool> g_writes_done{false};
// Only ever written by Thread A (the same thread that calls drain_step()) between iterations —
// never concurrently written, so a plain int is fine; only its EFFECT (the sleep) needs to be
// observed by Thread B, and that's mediated by g_writes_done above, not by this value directly.
int g_hook_delay_us = 0;
}  // namespace

void quark_activation_park_race_test_hook() {
    g_writes_done.store(true, std::memory_order_release);
    if (g_hook_delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(g_hook_delay_us));
}
}  // namespace quark
#endif

int main() {
    bool ok = true;

#if defined(QUARK_ACTIVATION_PARK_NO_GATE)
    constexpr bool kControl = true;
    constexpr int kIters = 3000;
    constexpr int kDelayPeriod = 5;   // widen every 5th iteration
    constexpr int kDelayUs = 5;       // same magnitude red-team's probe used
#else
    constexpr bool kControl = false;
    constexpr int kIters = 3000;
    constexpr int kDelayPeriod = 5;
    constexpr int kDelayUs = 5;
#endif

    int widened = 0, stuck = 0, resumed_ok = 0;

    for (int i = 0; i < kIters; ++i) {
        Handler actor;
        Activation act{&actor, Handler::dispatch_table()};

        Ping ping{i};
        Descriptor d;
        d.payload = &ping;
        stamp<Handler, Ping>(d);

        const bool widen = (i % kDelayPeriod) == 0;
        if (widen) ++widened;

#if defined(QUARK_ACTIVATION_PARK_NO_GATE)
        actor.awaiter_delay_us = 0;  // control: never delay pre-write (see banner) — the hook does it
        quark::g_writes_done.store(false, std::memory_order_relaxed);
        quark::g_hook_delay_us = widen ? kDelayUs : 0;
#else
        actor.awaiter_delay_us = widen ? kDelayUs : 0;  // fixed build: widen the FULL pre-write window
#endif

        act.post(&d);
        check(act.try_acquire(), "acquire Scheduled->Running", ok);

        bool carrier_wake = false;
        std::atomic<bool> carrier_done{false};
        bool carrier_spin_ok = true;

        std::thread carrier([&] {
#if defined(QUARK_ACTIVATION_PARK_NO_GATE)
            // Control: wait for the SAFE go-signal (plain fields already validly written) — see the
            // file banner for why the pre-write half isn't exercised here.
            if (!spin_until(quark::g_writes_done)) carrier_spin_ok = false;
#else
            // Fixed build: wait for the EARLIEST legitimate signal (suspend commit). No wait for
            // Parked here — that would defeat the point of this test.
            if (!spin_until(actor.handoff_ready)) carrier_spin_ok = false;
#endif
            carrier_wake = act.complete_parked();
            carrier_done.store(true, std::memory_order_release);
        });

        const auto out = act.drain_step(64);
        carrier.join();

        check(carrier_spin_ok, "carrier observed the go-signal (no stall)", ok);
        check(out == Activation::DrainOutcome::Suspended, "drain suspends on the async handler", ok);
        check(carrier_done.load(std::memory_order_acquire), "carrier completed complete_parked()", ok);

        const bool activation_stuck = act.state() == ExecState::Parked && !carrier_wake;

        if constexpr (kControl) {
            if (activation_stuck) {
                ++stuck;
                // Confirm PERMANENCE: post a brand-new message and show it can never be admitted —
                // "nothing left to ever re-admit it" (the bug report's own framing), not merely "this
                // one message was lost".
                Ping follow{-1};
                Descriptor d2;
                d2.payload = &follow;
                stamp<Handler, Ping>(d2);
                act.post(&d2);
                check(!act.try_acquire(), "a stuck activation can never be re-acquired (permanent seal)", ok);
            } else if (carrier_wake) {
                ++resumed_ok;
                check(act.state() == ExecState::Scheduled, "non-stuck iteration re-admitted cleanly", ok);
            }
        } else {
            // Fixed build: the oracle holds unconditionally, every iteration.
            check(!activation_stuck, "activation never sealed in Parked with a dropped re-admit", ok);
            check(carrier_wake, "re-admit signalled a wake (no silent lost wakeup)", ok);
            check(act.state() == ExecState::Scheduled, "re-admitted Parked->Scheduled", ok);
            check(!act.is_parked(), "parked frame cleared after completion", ok);
            check(actor.completed.load(std::memory_order_acquire) == 1, "handler resumed exactly once", ok);
            if (carrier_wake) ++resumed_ok;
        }
    }

    if constexpr (kControl) {
        const bool teeth_ok = stuck > 0;
        check(teeth_ok, "control reproduced the permanent-stranding lost wakeup at least once", ok);
        std::printf(
            "activation_park_race_test [CONTROL -DQUARK_ACTIVATION_PARK_NO_GATE]: %s  "
            "(iters=%d widened=%d resumed_ok=%d STUCK=%d)\n",
            (ok && teeth_ok) ? "OK (correctly reproduced the race — teeth confirmed)" : "FAIL", kIters,
            widened, resumed_ok, stuck);
        return (ok && teeth_ok) ? 0 : 1;
    }
    std::printf(
        "activation_park_race_test: %s  (iters=%d widened=%d resumed_ok=%d STUCK=%d)\n",
        ok ? "OK" : "FAIL", kIters, widened, resumed_ok, stuck);
    return ok ? 0 : 1;
}
