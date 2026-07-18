// Implements 011-Timers-and-Scheduled-Work §API + §Advancing the clock + §Cancellation + §Deadlines
// — the standalone TimerService: it owns a hierarchical timing wheel (timer_wheel.hpp), holds the
// `ActorRef`s to fire, and turns a delayed/periodic schedule into a plain `tell` on expiry. A timer
// firing NEVER runs user code off-lane — it enqueues a message on the target actor's mailbox through
// the public 006 `tell` seam, so ordering, placement, and the single-executor invariant all hold
// (011 §API: "timer callbacks are messages, never arbitrary lambdas run on the timer lane").
//
// SCOPE (faithful first cut). 011 places a wheel *per shard*, advanced by the shard's worker between
// drains, with a single node-level timekeeper backstop for idle shards (targeted wake, 002). That
// per-shard integration is a documented seam (see notes at end): it needs the engine's drain-edge
// and targeted-wakeup hooks, owned elsewhere this session. This standalone service is the spec-
// faithful standalone realization — one wheel, one optional driver thread — built PURELY on the
// public `ActorRef`/`tell`/`pal` seams. Its correctness (fire/cancel/periodic/rollover/deadline) is
// exactly what the per-shard version reuses; only the "who advances the clock" wrapper differs.
//
// THREADING & THE FIRE-vs-CANCEL RACE. The standalone service is multi-writer (arbitrary threads
// `schedule`/`cancel`; a driver thread ticks), so it serializes wheel access under one mutex. Every
// operation on an entry — insert, cancel-flag, fire, recycle (which bumps `gen`) — happens under
// that mutex, so there is no data race, and the pooled-entry `gen` gate makes a `cancel` that races
// a fire resolve to EXACTLY ONE outcome with no use-after-free (see cancel_entry / on_expire). The
// per-shard seam replaces the mutex with the single-writer + gen-gated lazy-cancel discipline (011
// §Cancellation) — same gen-gate, no lock. // TODO(seam-002): per-shard single-writer wheel.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "quark/core/actor_ref.hpp"
#include "quark/core/timer_wheel.hpp"
#include "pal/pal.hpp"

namespace quark {

class TimerService;

// --- TimerHandle: a cancellation token over a scheduled timer --------------------------------
// A value handle (service ptr + entry ptr + the `gen` it was minted at). `cancel()` is lazy and
// gen-gated: it flips the entry's tombstone only if the entry still carries the same generation
// (i.e., has neither fired-and-recycled nor been reused for a later timer). Copyable/movable; a
// default-constructed handle is inert. The handle must not outlive its TimerService.
class TimerHandle {
public:
    TimerHandle() noexcept = default;
    TimerHandle(TimerService* svc, detail::TimerEntry* entry, std::uint64_t gen) noexcept
        : svc_(svc), entry_(entry), gen_(gen) {}

    void cancel() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return svc_ != nullptr && entry_ != nullptr; }

private:
    friend class TimerService;
    TimerService* svc_ = nullptr;
    detail::TimerEntry* entry_ = nullptr;
    std::uint64_t gen_ = 0;
};

// --- TimerService ----------------------------------------------------------------------------
class TimerService {
public:
    struct Config {
        std::chrono::nanoseconds tick = std::chrono::milliseconds(1);  // wheel granularity (011 OQ)
        std::size_t entry_capacity = 1024;  // pre-sized entry pool (cold growth only past this)
    };

    TimerService() : TimerService(Config{}) {}

    explicit TimerService(Config cfg)
        : tick_ns_(cfg.tick.count() <= 0 ? 1 : static_cast<std::uint64_t>(cfg.tick.count())),
          pool_(cfg.entry_capacity),
          start_tp_(pal::now()) {}

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    ~TimerService() { stop(); }

    // --- Scheduling (011 §API) --------------------------------------------------------------
    // One-shot: `tell` `msg` to `ref` after `delay`. Periodic: re-arm every `period` (first fire
    // after `period`). Both return a cancellation handle. The message is stored by value in the
    // entry's inline slot and COPIED into the mailbox on each fire (so a periodic timer keeps
    // re-sending) — 0 heap allocation on the fire path.
    template <class A, class M>
    [[nodiscard]] TimerHandle schedule_after(const ActorRef<A>& ref, std::chrono::nanoseconds delay,
                                             M msg) {
        return arm<A, M>(ref, delay, /*period=*/std::chrono::nanoseconds::zero(), std::move(msg));
    }

    template <class A, class M>
    [[nodiscard]] TimerHandle schedule_every(const ActorRef<A>& ref,
                                             std::chrono::nanoseconds period, M msg) {
        return arm<A, M>(ref, period, period, std::move(msg));
    }

    // --- Deterministic driving (tests / the per-shard active-advance seam) ------------------
    // Advance the wheel by exactly one tick and fire everything that comes due. Use these for
    // deterministic tests; scheduling is relative to the wheel's LOGICAL now, so no wall-clock.
    void tick() {
        std::lock_guard<std::mutex> g(mu_);
        tick_locked();
    }
    void advance_ticks(std::uint64_t n) {
        std::lock_guard<std::mutex> g(mu_);
        for (std::uint64_t i = 0; i < n; ++i) tick_locked();
    }
    void advance(std::chrono::nanoseconds by) {
        advance_ticks(static_cast<std::uint64_t>(by.count() <= 0 ? 0 : by.count()) / tick_ns_);
    }

    // Advance the wheel to track real (`pal`) time — the worker-driven "active advance" (011).
    void poll() {
        std::lock_guard<std::mutex> g(mu_);
        poll_locked();
    }

    [[nodiscard]] std::uint64_t now_ticks() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return wheel_.now_ticks();
    }

    // --- Driver thread (011 §Advancing the clock — the timekeeper backstop, standalone) -----
    // Spawns exactly ONE thread (machine-safety: ≤1 timer thread) that polls real time at
    // `resolution` and fires due timers. Idempotent; `stop()` joins it. Tests that want
    // determinism use tick()/advance() and never start the driver.
    void start(std::chrono::nanoseconds resolution) {
        if (running_.exchange(true, std::memory_order_acq_rel)) return;
        const auto res = resolution.count() <= 0 ? std::chrono::nanoseconds(tick_ns_) : resolution;
        driver_ = std::jthread([this, res](std::stop_token st) { driver_loop(st, res); });
    }
    void start() { start(std::chrono::nanoseconds(tick_ns_)); }

    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        driver_.request_stop();
        if (driver_.joinable()) driver_.join();
    }

    // --- Deadline helper (011 §Deadlines / 018 seam) ----------------------------------------
    // The local monotonic instant `now + delay`, as steady-clock ns since the epoch — matching
    // `MessageContext::deadline_ns`. A wheel entry is ALWAYS a local instant; when a deadline
    // crosses a node boundary the transport ships the *remaining duration* and the receiver
    // reconstructs a local instant. That cross-node translation is defined in 018 and is a seam
    // here (010 transport / 018 clocks). This is the local-instant half both sides agree on.
    [[nodiscard]] static std::int64_t deadline_from_delay(std::chrono::nanoseconds delay) noexcept {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             pal::now().time_since_epoch())
                             .count();
        return static_cast<std::int64_t>(now) + delay.count();
    }

private:
    // The bound closure stored inline in an entry: a copy of the ref and the message. Firing copies
    // the message into the mailbox via `tell` (leaving `msg` intact for a periodic re-fire).
    template <class A, class M>
    struct BoundSend {
        ActorRef<A> ref;
        M msg;
    };

    template <class A, class M>
    static void fire_bound(void* p) noexcept {
        auto* b = static_cast<BoundSend<A, M>*>(p);
        b->ref.tell(b->msg);  // 006 tell: enqueue on the target mailbox (single-executor preserved)
    }
    template <class A, class M>
    static void destroy_bound(void* p) noexcept {
        static_cast<BoundSend<A, M>*>(p)->~BoundSend<A, M>();
    }

    template <class A, class M>
    TimerHandle arm(const ActorRef<A>& ref, std::chrono::nanoseconds delay,
                    std::chrono::nanoseconds period, M msg) {
        using Bound = BoundSend<A, std::remove_cvref_t<M>>;
        static_assert(sizeof(Bound) <= detail::TimerEntry::kInlineCap,
                      "timer payload too large for the inline entry slot (shrink the message)");
        static_assert(alignof(Bound) <= alignof(std::max_align_t),
                      "timer payload over-aligned for the inline entry slot");

        std::lock_guard<std::mutex> g(mu_);
        detail::TimerEntry* e = pool_.acquire();
        const std::uint64_t delay_ticks = ticks_ceil(delay);
        e->expiry_ticks = wheel_.now_ticks() + delay_ticks;
        e->period_ticks = period > std::chrono::nanoseconds::zero() ? ticks_ceil(period) : 0;
        e->deadline_ns = deadline_from_delay(delay);  // 018 seam: local instant this fire targets
        ::new (e->storage) Bound{ref, std::move(msg)};
        e->fire_fn = &fire_bound<A, std::remove_cvref_t<M>>;
        e->destroy_fn = &destroy_bound<A, std::remove_cvref_t<M>>;
        wheel_.insert(e);
        return TimerHandle{this, e, e->gen};
    }

    // Ceil a duration to whole ticks, floored at 1 so a timer always fires strictly in the future
    // (a sub-tick delay fires on the next tick — never "now", which could double-fire on re-arm).
    [[nodiscard]] std::uint64_t ticks_ceil(std::chrono::nanoseconds d) const noexcept {
        const std::int64_t ns = d.count() <= 0 ? 0 : d.count();
        const std::uint64_t t = (static_cast<std::uint64_t>(ns) + tick_ns_ - 1) / tick_ns_;
        return t == 0 ? 1 : t;
    }

    void tick_locked() {
        wheel_.tick([this](detail::TimerEntry* e) noexcept { on_expire(e); });
    }

    void poll_locked() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 pal::now() - start_tp_)
                                 .count();
        const std::uint64_t target = static_cast<std::uint64_t>(elapsed < 0 ? 0 : elapsed) / tick_ns_;
        while (wheel_.now_ticks() < target) tick_locked();
    }

    // Called (under mu_) for each entry whose bucket came due. Lazy-cancel is honored here: a
    // tombstoned entry is dropped, never told. A live one-shot fires once and recycles; a live
    // periodic fires and re-arms (same entry, new expiry — `gen` unchanged so its handle stays
    // valid until it is actually cancelled + recycled).
    void on_expire(detail::TimerEntry* e) noexcept {
        if (e->cancelled) {
            recycle(e);
            return;
        }
        e->fire_fn(e->storage);
        if (e->period_ticks != 0) {
            e->expiry_ticks += e->period_ticks;
            e->deadline_ns += static_cast<std::int64_t>(e->period_ticks * tick_ns_);
            wheel_.insert(e);  // re-arm (targets a future bucket — never the one being drained)
        } else {
            recycle(e);
        }
    }

    void recycle(detail::TimerEntry* e) noexcept {
        if (e->destroy_fn != nullptr) e->destroy_fn(e->storage);
        pool_.release(e);  // bumps gen → any stale handle's cancel() no-ops (gen-gate)
    }

    // Gen-gated lazy cancel. Under mu_, so it is serialized against fire/recycle: it either flips the
    // tombstone before the bucket fires (→ the entry is dropped, 0 fires) or observes a bumped gen
    // after recycle (→ no-op). Exactly one outcome; the entry memory is pool-owned, never freed under
    // a handle, so no use-after-free even if the slot was already reused for a newer timer.
    void cancel_entry(detail::TimerEntry* e, std::uint64_t gen) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        if (e->active && e->gen == gen) e->cancelled = true;
    }

    void driver_loop(std::stop_token st, std::chrono::nanoseconds res) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(res);
            poll();
        }
        poll();  // final catch-up so timers due at stop still fire
    }

    friend class TimerHandle;

    mutable std::mutex mu_;
    std::uint64_t tick_ns_;
    detail::TimerEntryPool pool_;
    detail::TimingWheel wheel_;
    pal::clock::time_point start_tp_;
    std::jthread driver_;
    std::atomic<bool> running_{false};
};

inline void TimerHandle::cancel() const noexcept {
    if (svc_ != nullptr && entry_ != nullptr) svc_->cancel_entry(entry_, gen_);
}

}  // namespace quark
