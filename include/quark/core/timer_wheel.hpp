// Implements 011-Timers-and-Scheduled-Work §Data structure — a hierarchical (cascading) timing
// wheel (Varghese & Lauck), the O(1)-insert / O(1)-per-tick expiry structure timers ride on. This
// header is deliberately transport-agnostic: it knows only about `TimerEntry` (a pooled, type-
// erased fire cell) and monotonic *ticks*. Binding an entry to an `ActorRef`/`tell` and to real
// (`pal`) time lives one layer up in `timer_service.hpp`, so the wheel itself is reusable both for
// the standalone `TimerService` and — as a documented seam — for a per-shard scheduler-integrated
// wheel (011 §Advancing the clock, single-writer, no locks).
//
// STRUCTURE (011): `kLevels` levels of `kWheelSize` buckets each, addressed by successive
// `kWheelBits`-wide digits of the absolute expiry tick. Level 0 covers the next `kWheelSize` ticks,
// level 1 the next `kWheelSize^2`, etc. On each level-0 rollover the wheel CASCADES one bucket down
// from the next level (Linux-kernel style), which is what guarantees "no timer lost across wheel
// rollover" while keeping insert O(1) and per-tick expiry O(1) amortized. Far-future timers that
// exceed the wheel span sit in a small binary-heap OVERFLOW tier and are promoted as time advances.
//
// FIRE/CANCEL DISCIPLINE (011 §Cancellation): cancellation is LAZY — `TimerEntry::cancelled` is a
// tombstone the wheel skips when the bucket fires; the wheel never scans to remove. Pooled entries
// carry a monotonic `gen` so a cancel racing a fire is gen-gated (see timer_service.hpp) — this
// header only distributes/expires entries; it owns no threading policy.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace quark::detail {

// --- TimerEntry: a pooled, type-erased fire cell -------------------------------------------------
// Intrusive `next` threads it through a bucket list (and, while free, the pool free-list). The
// fire/destroy thunks + inline `storage` hold the bound `(ActorRef<A>, Msg)` closure the service
// installs, so firing a timer is a plain indirect call with 0 heap allocation (011 §API — a timer
// firing is a `tell`, never an off-lane lambda). `gen` is the ABA/reuse guard for cancellation.
struct TimerEntry {
    static constexpr std::size_t kInlineCap = 96;  // holds ActorRef<A> (24 B) + a small message

    TimerEntry* next = nullptr;          // intrusive: bucket list while armed, free-list while pooled
    std::uint64_t expiry_ticks = 0;      // absolute expiry, in wheel ticks
    std::uint64_t period_ticks = 0;      // 0 = one-shot; >0 = periodic re-arm stride
    std::uint64_t gen = 0;               // reuse generation (gen-gated cancel — 011 §Cancellation)
    std::int64_t deadline_ns = 0;        // 018 seam: local steady-clock instant this entry targets
    bool cancelled = false;              // lazy tombstone: skipped when its bucket fires
    bool active = false;                 // armed (in a bucket/heap) vs recycled (in the free-list)
    void (*fire_fn)(void*) noexcept = nullptr;    // reconstruct closure + `tell` (service-installed)
    void (*destroy_fn)(void*) noexcept = nullptr;  // destroy the bound closure on recycle
    alignas(16) std::byte storage[kInlineCap]{};
};

// --- TimerEntryPool: stable-address free-list of entries -----------------------------------------
// Backs entries in chunks so their addresses stay stable for the lifetime of a live TimerHandle
// (a handle holds `TimerEntry*` + the `gen` it saw). `acquire`/`release` are O(1); growth (a new
// chunk) is a COLD event on the schedule path only — never on the tick/fire path. `release` bumps
// `gen`, so a stale handle to a recycled slot fails the gen-gate cleanly (no use-after-free: the
// memory is owned by the pool for its whole lifetime, only recycled, never freed under a handle).
class TimerEntryPool {
public:
    explicit TimerEntryPool(std::size_t chunk = 256) : chunk_(chunk == 0 ? 1 : chunk) {}

    TimerEntryPool(const TimerEntryPool&) = delete;
    TimerEntryPool& operator=(const TimerEntryPool&) = delete;

    [[nodiscard]] TimerEntry* acquire() {
        if (free_ == nullptr) grow();  // cold: pre-size the service to avoid this on schedule
        TimerEntry* e = free_;
        free_ = e->next;
        e->next = nullptr;
        e->cancelled = false;
        e->active = true;
        return e;
    }

    // Recycle a fired/cancelled entry. Bumps `gen` FIRST so any handle still holding the old gen
    // no-ops (gen-gate), then threads the cell back onto the free-list.
    void release(TimerEntry* e) noexcept {
        e->active = false;
        e->cancelled = false;
        e->fire_fn = nullptr;
        e->destroy_fn = nullptr;
        ++e->gen;
        e->next = free_;
        free_ = e;
    }

private:
    void grow() {
        auto block = std::make_unique<TimerEntry[]>(chunk_);
        for (std::size_t i = 0; i < chunk_; ++i) {
            block[i].next = free_;
            free_ = &block[i];
        }
        chunks_.push_back(std::move(block));
    }

    std::size_t chunk_;
    std::vector<std::unique_ptr<TimerEntry[]>> chunks_;
    TimerEntry* free_ = nullptr;
};

// --- TimingWheel: the cascading hierarchical wheel -----------------------------------------------
// Single-writer by contract (the owner serializes insert/tick — the standalone service does so with
// a mutex; the per-shard seam does so by construction). Time is measured in abstract ticks; the
// owner maps ticks<->`pal` time. `tick()` advances the wheel by one tick and invokes `fire_one` for
// each entry whose bucket comes due, in bucket order.
class TimingWheel {
public:
    static constexpr unsigned kWheelBits = 6;                     // 64 buckets per level
    static constexpr unsigned kWheelSize = 1u << kWheelBits;      // 64
    static constexpr std::uint64_t kWheelMask = kWheelSize - 1;
    static constexpr unsigned kLevels = 4;                        // span = 64^4 = 16,777,216 ticks

    [[nodiscard]] static constexpr std::uint64_t span_ticks() noexcept {
        return std::uint64_t{1} << (kWheelBits * kLevels);
    }

    [[nodiscard]] std::uint64_t now_ticks() const noexcept { return now_; }
    [[nodiscard]] std::size_t overflow_size() const noexcept { return overflow_.size(); }

    // Distribute an entry into the level whose digit selects its bucket, by absolute `expiry_ticks`.
    // Overdue entries (expiry <= now) land in the current level-0 bucket to fire on the next tick.
    // O(1); the only branch is the level pick. No allocation unless the (cold) overflow heap grows.
    void insert(TimerEntry* e) {
        const std::int64_t sdelta = static_cast<std::int64_t>(e->expiry_ticks - now_);
        if (sdelta <= 0) {                                  // due / overdue → fire next tick
            push_front(levels_[0][now_ & kWheelMask], e);
            return;
        }
        const std::uint64_t delta = static_cast<std::uint64_t>(sdelta);
        for (unsigned lvl = 0; lvl < kLevels; ++lvl) {
            if (delta < (std::uint64_t{1} << (kWheelBits * (lvl + 1)))) {
                const std::uint64_t slot = (e->expiry_ticks >> (kWheelBits * lvl)) & kWheelMask;
                push_front(levels_[lvl][slot], e);
                return;
            }
        }
        overflow_.push_back(e);                             // far future (cold, sparse)
        std::push_heap(overflow_.begin(), overflow_.end(), heap_greater);
    }

    // Advance one tick and fire the bucket that comes due. `fire_one` is invoked per expired entry
    // (the service tells + recycles/re-arms). 0 allocation on this path (buckets are intrusive; the
    // overflow heap only shrinks here).
    template <class FireOne>
    void tick(FireOne&& fire_one) {
        promote_overflow();
        const std::uint64_t idx = now_ & kWheelMask;
        if (idx == 0) cascade_chain();  // level-0 rollover → cascade higher levels down
        ++now_;
        // Detach the due bucket BEFORE firing so re-arms/cascades that re-insert can't corrupt the
        // list we are walking (a periodic re-arm targets a future bucket, never this one).
        TimerEntry* e = take(levels_[0][idx]);
        while (e != nullptr) {
            TimerEntry* nxt = e->next;
            e->next = nullptr;
            fire_one(e);
            e = nxt;
        }
    }

private:
    struct Bucket {
        TimerEntry* head = nullptr;
    };

    static void push_front(Bucket& b, TimerEntry* e) noexcept {
        e->next = b.head;
        b.head = e;
    }
    static TimerEntry* take(Bucket& b) noexcept {
        TimerEntry* h = b.head;
        b.head = nullptr;
        return h;
    }
    static bool heap_greater(const TimerEntry* a, const TimerEntry* b) noexcept {
        return a->expiry_ticks > b->expiry_ticks;  // min-heap by expiry (top = soonest)
    }

    // On a level-0 rollover, cascade each higher level whose digit also just rolled to 0: re-insert
    // that level's now-current bucket, which redistributes its entries downward (Linux __run_timers
    // discipline). Re-insertion of an entry whose expiry is within the wheel span always lands it in
    // a lower level or the current level-0 bucket — never back where it came from.
    void cascade_chain() {
        for (unsigned lvl = 1; lvl < kLevels; ++lvl) {
            const std::uint64_t cidx = (now_ >> (kWheelBits * lvl)) & kWheelMask;
            TimerEntry* e = take(levels_[lvl][cidx]);
            while (e != nullptr) {
                TimerEntry* nxt = e->next;
                e->next = nullptr;
                insert(e);
                e = nxt;
            }
            if (cidx != 0) break;  // this level didn't roll over → chain stops (the && short-circuit)
        }
    }

    // Pull far-future entries that now fit the wheel span down into it. Sparse and cold: the heap is
    // only touched here (pop) and on an overflow insert, never on the common short-timer fire path.
    void promote_overflow() {
        while (!overflow_.empty()) {
            TimerEntry* top = overflow_.front();
            if (static_cast<std::int64_t>(top->expiry_ticks - now_) >=
                static_cast<std::int64_t>(span_ticks())) {
                break;  // still beyond the span; heap is ordered so nothing else qualifies either
            }
            std::pop_heap(overflow_.begin(), overflow_.end(), heap_greater);
            overflow_.pop_back();
            insert(top);
        }
    }

    Bucket levels_[kLevels][kWheelSize]{};
    std::vector<TimerEntry*> overflow_{};  // binary min-heap by expiry_ticks (011 overflow tier)
    std::uint64_t now_ = 0;
};

}  // namespace quark::detail
