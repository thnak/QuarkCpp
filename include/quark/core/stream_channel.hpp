// Implements 024-Streaming-and-Inbound-Streams §StreamChannel — the pre-allocated, bounded,
// credit-controlled SPSC ring that carries a high-rate inbound stream to an actor WITHOUT fanning
// one discrete mailbox descriptor per frame. Pinned by ADR-005 (the credit-ring winner) and
// ADR-014 (the async-suspend real-scheduler exactly-once gate).
//
// This header owns the RING PRIMITIVE only — the three single-writer monotone cursors, try_push,
// the plain-load/plain-store drain, derived credit, hysteresis arming state, the producer-un-stall
// rendezvous, and the single-producer token. It sits OFF the mailbox: the ordinary Vyukov mailbox
// (003, mailbox.hpp) + discrete tell/ask path are UNCHANGED. The exec-state wakeup + seq_cst Dekker
// close-out that schedules the drain lives in stream_activation.hpp, composing the settled 002/015
// ExecStateCell VERBATIM (new buffer, old scheduler — 024 §"Off the mailbox, or a mailbox variant?").
//
// CREDIT IS DERIVED, NOT COUNTED (024 §Cursors): occupancy = head - disp, credit-available =
// window - (head - tail). Two single-writer cursors, so there is NO shared credit counter to
// double-grant or lose — GATE-5 is race-free BY CONSTRUCTION (ADR-005). The consumer inner loop is
// plain acquire-load + release-store ONLY; the ONLY cross-core RMW in the whole design is the
// producer's `armed.exchange` on the ring's empty->nonempty arm-edge (stream_activation.hpp), never
// per frame (the load-bearing 023 0-RMW gate; ADR-005 S1).
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (named downstream owner — NOT implemented here; compiled away in the default):
//   * StreamMode::ZeroCopyRetained / by-reference registered-RX / transport DMA into the arena +
//     the strict in-order-prefix byte-credit reclamation  ->  019 (PAL) / 003 (Memory). The inline
//     ≤56 B slot default (this file) is immune to the overwrite hazard (it copies into the slot);
//     the by-reference regime + byte-credit are declared (StreamMode) and stubbed, never wired.
//   * Outbound streaming replies (an `ask` returning a stream)  ->  006. This spec is INBOUND only.
//   * Multi-source fan-in into one stream  ->  stays on the mailbox (SPSC precondition; the
//     single-producer token below is the runtime enforcement — a second bind is a typed 007 error).
//   * Transport framing / wire format  ->  010 / 016 (they own the bytes; 024 owns the ring once
//     frames are in it).
//   * ARM64 weak-memory re-gate of the `armed.exchange`-as-Dekker-arm  ->  deferred (herd7/GenMC
//     before any ARM claim). x86-TSO only here; see the TODO(arm64) in stream_activation.hpp.
//   * Adaptive `credit_limit` narrowing stability + idle-density footprint  ->  022 open questions.
// ============================================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>

#include "quark/core/config.hpp"
#include "quark/core/error.hpp"

namespace quark {

// The two slot regimes (024 §Buffer ownership and zero-copy). Inline is the DEFAULT and the only
// one wired here; ZeroCopyRetained is a declared 019/003 seam (compiled away in the inline default).
enum class StreamMode : std::uint8_t {
    Inline = 0,            // ≤56 B frame lives in the slot (copied in); immune to the overwrite hazard
    ZeroCopyRetained = 1,  // SEAM (019/003): by-reference span + in-order-prefix byte-credit — NOT wired
};

// The 003 inline-slot ceiling: a frame this size or smaller lives directly in the ring slot.
inline constexpr std::size_t kStreamInlineMax = 56;

// ============================================================================================
// MonotoneCursor — a single-writer, 64-bit monotone cursor. It EXPOSES ONLY load + store and has
// deliberately NO fetch_add / exchange / compare_exchange: a consumer therefore CANNOT issue a
// cross-core atomic RMW on a cursor even by accident. The 023 0-RMW-on-drain gate is thus enforced
// STRUCTURALLY (by the type's absent API), not merely measured after the fact (ADR-005 S1).
// ============================================================================================
class MonotoneCursor {
public:
    MonotoneCursor() noexcept = default;
    MonotoneCursor(const MonotoneCursor&) = delete;
    MonotoneCursor& operator=(const MonotoneCursor&) = delete;

    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t acquire() const noexcept {
        return v_.load(std::memory_order_acquire);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t relaxed() const noexcept {
        return v_.load(std::memory_order_relaxed);
    }
    QUARK_ALWAYS_INLINE void store_release(std::uint64_t x) noexcept {
        v_.store(x, std::memory_order_release);
    }
    QUARK_ALWAYS_INLINE void store_relaxed(std::uint64_t x) noexcept {
        v_.store(x, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> v_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "a cursor must be a lock-free 64-bit atomic (no wrap for any process lifetime)");
};

// ============================================================================================
// StreamChannel<F> — one pre-allocated, bounded, credit-controlled SPSC ring per stream. Owned by
// the activation (stream_activation.hpp); sits OFF the mailbox. F is the inline frame type.
//
// Cursors (024 §Cursors — credit is derived, not counted):
//   head (producer): next slot to fill.        Occupancy       = head - disp.
//   disp (consumer): next frame to dispatch.    Credit-available = window - (head - tail).
//   tail (consumer): oldest slot still owed credit (credit-return).
//
// The split of `disp` and `tail` is the async-suspend exactly-once heart: on dispatch of frame k
// `disp`->k+1 but `tail` stays, so a SUSPENDED handler pins the window at the parked frame — the
// producer's derived credit provably cannot cover the parked slot (no overwrite). `tail` advances
// only when the frame COMPLETES (024 §Why disp and tail are split; ADR-014 CONTROL-4 proves a
// collapsed single cursor tears/loses and wrongly credits parked frames).
// ============================================================================================
template <class F>
class StreamChannel {
    static_assert(std::is_trivially_copyable_v<F>,
                  "inline-slot stream frame must be trivially copyable (024 §Inline slot regime); "
                  "a by-reference / ZeroCopyRetained frame is a declared 019/003 seam");
    static_assert(sizeof(F) <= 4096, "frame is implausibly large for a ring slot — use by-reference (019 seam)");

public:
    struct Config {
        std::uint32_t capacity = 256;     // the credit window (rounded UP to a power of two)
        std::uint32_t low_watermark = 0;  // hysteresis re-arm threshold; 0 => capacity/2
        StreamMode mode = StreamMode::Inline;
    };

    // COLD (stream-open): pre-allocate the ring from the shard pmr resource. 0 per-frame hot-path
    // alloc thereafter (024 §Buffer ownership; ADR-005 S2). Freed at close (dtor).
    StreamChannel(const Config& cfg, std::pmr::memory_resource* mr)
        : mr_(mr),
          capacity_(round_up_pow2(cfg.capacity == 0 ? 1u : cfg.capacity)),
          mask_(capacity_ - 1),
          low_watermark_(cfg.low_watermark != 0 ? cfg.low_watermark : capacity_ / 2),
          mode_(cfg.mode) {
        // Inline default: a flat array of trivially-copyable slots, pre-allocated once (cold).
        slots_ = static_cast<F*>(mr_->allocate(sizeof(F) * capacity_, alignof(F) < alignof(std::max_align_t)
                                                                           ? alignof(std::max_align_t)
                                                                           : alignof(F)));
    }

    ~StreamChannel() {
        if (slots_)
            mr_->deallocate(slots_, sizeof(F) * capacity_,
                            alignof(F) < alignof(std::max_align_t) ? alignof(std::max_align_t) : alignof(F));
    }

    StreamChannel(const StreamChannel&) = delete;
    StreamChannel& operator=(const StreamChannel&) = delete;
    StreamChannel(StreamChannel&&) = delete;
    StreamChannel& operator=(StreamChannel&&) = delete;

    // ---- Producer side (single writer; enforced by the stream-open token, stream_activation.hpp) --
    // Lossless backpressure: FAILS (returns false) when credit is depleted, so the producer STALLS
    // (a lost frame N would corrupt the stream — 024 §Backpressure). Never sheds. FIFO-per-stream is
    // the monotone order of `head`. Plain load + copy + release-store — NO cross-core RMW.
    [[nodiscard]] QUARK_ALWAYS_INLINE bool try_push(const F& frame) noexcept {
        const std::uint64_t h = head_.relaxed();   // single-writer self-read
        const std::uint64_t t = tail_.acquire();   // consumer's credit-return cursor
        if (h - t >= capacity_) return false;       // derived credit depleted -> stall (lossless)
        slots_[h & mask_] = frame;   // copy the ≤56 B frame INTO the slot (inline regime)
        head_.store_release(h + 1);  // publish (release pairs with the consumer's head acquire)
        return true;
    }

    // Blocking, lossless push: on credit depletion, arm the reverse-Dekker stall edge and WAIT on the
    // credit-return wake (024 §Producer un-stall). Never sheds. Off the hot path (only under sustained
    // overproduce). The producer half of the reverse Dekker: arm `stalled_`, seq_cst fence, capture the
    // credit generation AFTER arming, re-check credit; if still depleted, sleep on the generation —
    // paired with the consumer's `poll_unstall` (advance tail, fence, observe `stalled_`, bump+notify),
    // so a stalled producer is never left asleep against available credit (no lost wakeup).
    void push_blocking(const F& frame) noexcept {
        for (;;) {
            if (try_push(frame)) return;
            stalls_.fetch_add(1, std::memory_order_relaxed);            // observability: a real stall
            // CAPTURE THE CREDIT GENERATION *BEFORE* ARMING (load-bearing ordering — see below). Any
            // credit-return that observes our arm bumps the generation strictly PAST this captured `g`
            // (its `stalled_.exchange`+`fetch_add` happen-after our `stalled_.store(true)`, which
            // happens-after this load), so `wait(g)` provably cannot sleep on an already-signalled
            // generation. Capturing AFTER arming reintroduced a LOST WAKEUP that hangs under sustained
            // overproduce: a `poll_unstall` slipping between the arm and the capture consumed the arm
            // (exchange->false, bumped the gen — that WAS this stall's wakeup) and the producer then
            // slept on that already-bumped value with `stalled_` clear, so no future poll_unstall
            // re-signalled it (empirically: asleep in wait, stalled_==false, gen frozen, full credit).
            const std::uint32_t g = credit_gen_.load(std::memory_order_acquire);  // capture BEFORE arming
            stalled_.store(true, std::memory_order_release);           // arm the un-stall edge
            std::atomic_thread_fence(std::memory_order_seq_cst);       // StoreLoad (producer Dekker half)
            if (credit_available() > 0) {                              // credit raced back in
                stalled_.store(false, std::memory_order_relaxed);
                continue;
            }
            credit_gen_.wait(g, std::memory_order_acquire);            // sleep until a credit-return wake
            stalled_.store(false, std::memory_order_relaxed);         // woke; retry the push
        }
    }

    // ---- Consumer side (single drainer; the exec-state CAS guarantees at-most-one — 001/002) ------
    // Every operation here is plain acquire-load / release-store / relaxed self-read. NO RMW.

    // Frames waiting to be dispatched.
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t occupancy() const noexcept {
        return head_.acquire() - disp_.relaxed();
    }
    // Credit the producer may still consume (window - resident-owed).
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t credit_available() const noexcept {
        return capacity_ - (head_.acquire() - tail_.acquire());
    }

    // Peek the next frame to dispatch (at `disp`) WITHOUT advancing — valid iff occupancy() > 0.
    [[nodiscard]] QUARK_ALWAYS_INLINE const F& peek() const noexcept { return slots_[disp_.relaxed() & mask_]; }
    // The frame at an arbitrary already-dispatched-but-not-yet-credited index (the pinned window):
    // valid iff tail <= idx < head. Used to re-read a PARKED frame's slot on resume (it is pinned by
    // the split cursor, so this read races nothing — the producer's credit cannot cover it).
    [[nodiscard]] QUARK_ALWAYS_INLINE const F& slot_at(std::uint64_t idx) const noexcept {
        return slots_[idx & mask_];
    }

    // DISPATCH edge: advance `disp` (frame at the old `disp` is now dispatched). Release store only.
    QUARK_ALWAYS_INLINE std::uint64_t advance_dispatch() noexcept {
        const std::uint64_t d = disp_.relaxed();
        disp_.store_release(d + 1);
        return d;  // the index that was just dispatched
    }
    // CREDIT-RETURN edge: advance `tail` (frame at the old `tail` has COMPLETED; its slot is now
    // reclaimable and its credit returns to the producer). Release store only — NO RMW on the drain.
    QUARK_ALWAYS_INLINE void advance_tail() noexcept {
        tail_.store_release(tail_.relaxed() + 1);
    }

    // ---- Hysteresis (024 §Arming — low-watermark) -------------------------------------------------
    // Re-arm the readiness edge only once occupancy has fallen to/under the low-watermark, so the
    // emergent batch is large. A budget-bounded drain that leaves occupancy above the watermark keeps
    // the descriptor armed (it will be re-scheduled for fairness, not re-armed per frame).
    [[nodiscard]] QUARK_ALWAYS_INLINE bool below_low_watermark() const noexcept {
        return occupancy() <= low_watermark_;
    }

    // ---- Producer un-stall (024 §Producer un-stall — reverse Dekker rendezvous) --------------------
    // Called by the drainer at a BATCH boundary (never per frame): if a credit-starved producer armed
    // `stalled_`, the seq_cst fence + the notify wake it on the next credit-return under edge-triggered
    // readiness, so a stalled producer never sleeps forever against available credit. Off the per-frame
    // path — the RMW/fence here is O(batches), not O(frames).
    void poll_unstall() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);  // consumer half of the reverse Dekker
        // Test-and-clear the stall edge (exchange is an RMW, but at a BATCH boundary — O(stalls), NOT
        // per frame). On the disarm edge, bump the generation + wake the sleeping producer.
        if (QUARK_UNLIKELY(stalled_.exchange(false, std::memory_order_acq_rel))) {
            credit_gen_.fetch_add(1, std::memory_order_acq_rel);
            credit_gen_.notify_all();
        }
    }

    [[nodiscard]] std::uint32_t credit_generation() const noexcept {
        return credit_gen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t stall_events() const noexcept { return stalls_.load(std::memory_order_relaxed); }

    // ---- Observers (tests / 009 observability) ----------------------------------------------------
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t low_watermark() const noexcept { return low_watermark_; }
    [[nodiscard]] StreamMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint64_t head() const noexcept { return head_.acquire(); }
    [[nodiscard]] std::uint64_t disp() const noexcept { return disp_.acquire(); }
    [[nodiscard]] std::uint64_t tail() const noexcept { return tail_.acquire(); }

    static constexpr std::uint32_t round_up_pow2(std::uint32_t x) noexcept {
        if (x <= 1) return 1;
        --x;
        x |= x >> 1;  x |= x >> 2;  x |= x >> 4;  x |= x >> 8;  x |= x >> 16;
        return x + 1;
    }

private:
    std::pmr::memory_resource* mr_;
    F* slots_ = nullptr;
    std::uint32_t capacity_;
    std::uint64_t mask_;
    std::uint32_t low_watermark_;
    StreamMode mode_;

    // Producer-owned cursor on its own line; consumer-owned cursors on another — head is written by
    // the producer, disp/tail by the consumer, so isolate the producer line from the consumer lines
    // to keep the drain's plain loads off the contended producer store (ADR-005 F1 layout fix).
    QUARK_CACHE_ALIGNED MonotoneCursor head_;   // producer single-writer
    QUARK_CACHE_ALIGNED MonotoneCursor disp_;   // consumer single-writer
    MonotoneCursor tail_;                        // consumer single-writer (same line as disp_ — both consumer)

    // Reverse-Dekker un-stall word (off the per-frame path). `stalled_` armed by the producer,
    // observed+cleared by the consumer at a batch boundary; `credit_gen_` is the futex wake word.
    QUARK_CACHE_ALIGNED std::atomic<bool> stalled_{false};
    std::atomic<std::uint32_t> credit_gen_{0};
    std::atomic<std::uint64_t> stalls_{0};  // observability: producer stall episodes (off the hot path)
};

}  // namespace quark
