// Implements 006-Messaging-and-Addressing §ask + ADR-007 §5 (reply routing) / §Reply ordering —
// the shard-pooled, monotonic-generation ReplyCell with the win-arbitration handshake.
//
// An `ask` routes its one-shot reply through a ReplyCell (NOT the caller frame — ADR-007), so the
// UAF/ABA/lost-wakeup class the naive deposit-then-CAS sketch had is closed:
//   * ONE atomic `st_` arbitrates the awaiter (await_suspend) against the responder (resolve): the
//     first to CAS Empty→{Waiter|Resolved} wins; the loser observes the winner and completes the
//     rendezvous. Exactly one of them resumes/observes — no double, no lost wakeup.
//   * a monotonic `gen_` (bumped on pool release) fences a STALE responder (from a cancelled/prior
//     ask whose cell was recycled) — resolve() no-ops on a generation mismatch (the descriptor
//     gen-gate mirror, ADR-003/004).
//   * the AWAITER is the sole cell-release point (ADR-007 §5): the responder never frees the cell.
//
// SCOPE (006): local delivery. Timeout/deadline-driven resolution is a documented seam to 011
// (a deadline fires → resolve(gen, timeout)); the cell lifecycle here is complete and correct.
#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "quark/core/error.hpp"

namespace quark::detail {

// A one-shot reply channel for a single `ask`. Pooled and generation-fenced (never the caller
// frame). Delivers `result<R>` = std::expected<R, error> so failure is a value, not an exception
// across the boundary (006 §Reply type and errors).
template <class R>
class ReplyCell {
public:
    ReplyCell() noexcept = default;
    ReplyCell(const ReplyCell&) = delete;
    ReplyCell& operator=(const ReplyCell&) = delete;
    ~ReplyCell() {
        if (has_value_) value_ptr()->~result<R>();
    }

    // Current generation the responder must present. Captured by the Responder at ask time.
    [[nodiscard]] std::uint64_t gen() const noexcept { return gen_.load(std::memory_order_acquire); }

    // --- Pool lifecycle (single-threaded: pool mutex held) --------------------------------
    // arm: ready the cell for a fresh ask; returns the generation to hand the responder.
    std::uint64_t arm() noexcept {
        cont_ = {};
        st_.store(kEmpty, std::memory_order_release);
        return gen_.load(std::memory_order_relaxed);
    }
    // disarm: bump the generation (fences any late/stale resolve) and drop a leftover value.
    void disarm() noexcept {
        if (has_value_) {
            value_ptr()->~result<R>();
            has_value_ = false;
        }
        gen_.fetch_add(1, std::memory_order_acq_rel);
        st_.store(kEmpty, std::memory_order_release);
    }

    // --- Responder side (single resolver; gen-fenced) -------------------------------------
    // Deposit the reply and complete the rendezvous. A stale responder (recycled cell) no-ops.
    void resolve(std::uint64_t gen, result<R> v) noexcept {
        if (gen_.load(std::memory_order_acquire) != gen) return;  // recycled — stale, drop
        ::new (store_) result<R>(std::move(v));
        has_value_ = true;
        // Publish the value, then win-arbitrate on st_. release-store publishes `store_`/has_value_.
        const std::uint32_t prev = st_.exchange(kResolved, std::memory_order_acq_rel);
        if (prev == kWaiter) {
            std::coroutine_handle<> h = cont_;  // published by suspend()'s release-CAS
            st_.notify_all();
            if (h) h.resume();  // 015 SEAM: block_on never sets a waiter; the co_await path's
                                // re-admit-through-the-015-gate is future work — here the awaiter
                                // is driven off-lane (block_on) or on a standalone test coroutine.
            return;
        }
        st_.notify_all();  // resolver won the race; a later suspend() will observe kResolved
    }

    // --- Awaiter side ---------------------------------------------------------------------
    [[nodiscard]] bool ready() const noexcept {
        return st_.load(std::memory_order_acquire) == kResolved;
    }
    // Returns true iff the awaiter should stay suspended (responder will resume it); false iff the
    // value already landed (do not suspend). This CAS is the awaiter half of the win-arbitration.
    [[nodiscard]] bool suspend(std::coroutine_handle<> h) noexcept {
        cont_ = h;
        std::uint32_t expected = kEmpty;
        if (st_.compare_exchange_strong(expected, kWaiter, std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            return true;   // suspended — resolve() will resume us
        return false;      // already kResolved — carry on, take() has the value
    }
    // Off-lane blocking wait (block_on): park the calling thread on st_ until resolved.
    void block_wait() noexcept {
        std::uint32_t s = st_.load(std::memory_order_acquire);
        while (s != kResolved) {
            st_.wait(s, std::memory_order_acquire);
            s = st_.load(std::memory_order_acquire);
        }
    }
    // Move the resolved value out (called once, by the awaiter, before releasing the cell).
    [[nodiscard]] result<R> take() noexcept {
        result<R>* p = value_ptr();
        result<R> r = std::move(*p);
        p->~result<R>();
        has_value_ = false;
        return r;
    }

private:
    // Typed view over the aligned value storage. Route through void* (not reinterpret_cast) so the
    // aligned access carries no -Wcast-align noise; std::launder for the placement-new'd object.
    [[nodiscard]] result<R>* value_ptr() noexcept {
        return std::launder(static_cast<result<R>*>(static_cast<void*>(store_)));
    }

    static constexpr std::uint32_t kEmpty = 0;
    static constexpr std::uint32_t kWaiter = 1;
    static constexpr std::uint32_t kResolved = 2;

    std::atomic<std::uint32_t> st_{kEmpty};
    std::atomic<std::uint64_t> gen_{0};
    std::coroutine_handle<> cont_{};
    alignas(result<R>) unsigned char store_[sizeof(result<R>)];
    bool has_value_ = false;
};

// A shard-pooled source of ReplyCells (ADR-007: "shard-pooled monotonic-generation ReplyCell").
// SCOPE (006): one pool per (router, R). Per-shard typed pools are the production placement
// (008/003 seam); the win-arbitration + gen-fence lifecycle is identical either way. Mutex-guarded
// (acquire is off many ask lanes, release off the drain lane) — a lock, not a heap allocation, so
// steady-state ask is 0 pool-upstream allocation (ADR-007 F1) after warmup.
template <class R>
class ReplyCellPool {
public:
    struct Lease {
        ReplyCell<R>* cell = nullptr;
        std::uint64_t gen = 0;
    };

    explicit ReplyCellPool(std::size_t capacity) {
        storage_.reserve(capacity);
        free_.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            auto c = std::make_unique<ReplyCell<R>>();
            free_.push_back(c.get());
            storage_.push_back(std::move(c));
        }
    }
    ReplyCellPool(const ReplyCellPool&) = delete;
    ReplyCellPool& operator=(const ReplyCellPool&) = delete;

    [[nodiscard]] Lease acquire() {
        std::lock_guard<std::mutex> g(mu_);
        ReplyCell<R>* c;
        if (free_.empty()) {  // cold growth — pre-size to avoid it on the ask path
            auto n = std::make_unique<ReplyCell<R>>();
            c = n.get();
            storage_.push_back(std::move(n));
        } else {
            c = free_.back();
            free_.pop_back();
        }
        return Lease{c, c->arm()};
    }

    void release(ReplyCell<R>* c) {
        c->disarm();  // bump generation BEFORE handing back (fences a stale resolve into reuse)
        std::lock_guard<std::mutex> g(mu_);
        free_.push_back(c);
    }

private:
    std::mutex mu_;
    std::vector<std::unique_ptr<ReplyCell<R>>> storage_;
    std::vector<ReplyCell<R>*> free_;
};

// The handler-facing reply handle carried INSIDE the ask envelope (the query payload). ADR-007's
// dispatch table is locked this session (handlers return void/task<>, not R), so the reply channel
// travels in the message rather than as a handler return value — a documented seam to the
// "handler-returns-R" sugar. `respond(r)` resolves the cell exactly once.
//
// Move-only + single-armed: `ask` builds one armed Responder into the message; the caller-side
// temporary is left disarmed by the move, so exactly one Responder governs the cell. If the handler
// never replies, the Responder's destructor (run when the payload is reclaimed — on completion,
// tombstone-cancel, or teardown) fails the cell so NO caller hangs (reply-before-teardown).
template <class R>
class Responder {
public:
    Responder() noexcept = default;
    Responder(ReplyCell<R>* cell, std::uint64_t gen) noexcept
        : cell_(cell), gen_(gen), armed_(true) {}

    Responder(const Responder&) = delete;
    Responder& operator=(const Responder&) = delete;
    Responder(Responder&& o) noexcept : cell_(o.cell_), gen_(o.gen_), armed_(o.armed_) {
        o.armed_ = false;
        o.cell_ = nullptr;
    }
    Responder& operator=(Responder&& o) noexcept {
        if (this != &o) {
            fire_default();
            cell_ = o.cell_;
            gen_ = o.gen_;
            armed_ = o.armed_;
            o.armed_ = false;
            o.cell_ = nullptr;
        }
        return *this;
    }
    ~Responder() { fire_default(); }

    // Resolve the reply. `armed_` is local + touched only on the handler lane, so it is read BEFORE
    // the cell — after this, the awaiter may release the cell, but the destructor never touches it.
    void operator()(R value) const noexcept {
        if (!armed_) return;
        armed_ = false;
        cell_->resolve(gen_, result<R>(std::move(value)));
    }
    void fail(error e) const noexcept {
        if (!armed_) return;
        armed_ = false;
        cell_->resolve(gen_, std::unexpected<error>(e));
    }
    [[nodiscard]] bool armed() const noexcept { return armed_; }

private:
    void fire_default() noexcept {
        if (armed_ && cell_) {
            armed_ = false;
            // No reply produced before teardown/reclaim — resolve with a failure so the caller's
            // block_on/co_await returns an error value instead of hanging forever (007 seam: a
            // supervisor mass-fail of pending cells reuses this exact path).
            cell_->resolve(gen_, std::unexpected<error>(error{errc::supervised_stop, "no_reply"}));
        }
    }

    ReplyCell<R>* cell_ = nullptr;
    std::uint64_t gen_ = 0;
    mutable bool armed_ = false;
};

}  // namespace quark::detail
