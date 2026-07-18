// Implements 002-Scheduler §Sharding/§Priority scheduling + ADR-010 (K-band per-shard run-queue).
//
// The per-shard *run-queue* holds SCHEDULED ACTIVATIONS (not messages — that is the per-actor
// mailbox, 003). It is an intrusive Vyukov MPSC of `Schedulable` nodes: many producer threads push
// an activation on its Idle→Scheduled (or Parked→Scheduled) edge, a SINGLE draining worker pops
// them (single-consumer, arbitrated by the per-shard drain-owner CAS in the engine — a cold edge,
// ADR-010 §Work stealing). Emptiness is NON-LINEARIZABLE exactly like the mailbox, so `select`
// surfaces a distinct `Busy` the worker bounded-spins on (ADR-010 normative note).
//
// PRIORITY (ADR-010): the run-queue generalizes to K FIFO bands.
//   * `UniformFIFO` (K=1, DEFAULT) is a DISTINCT TYPE that lowers to a single per-shard MPSC with
//     ZERO added band-select/branch/atomic — proven byte-identical to a no-priority control. The
//     disable path MUST resolve to `UniformFIFO`, never `PriorityBands<1>` (005 Validation rule).
//   * `PriorityBands<K, Anti>` (1<K≤8): enqueue picks the band by a compile-time-known subscript on
//     the SAME single `tail_.exchange` (0 added cross-core RMW); select does an O(K) relaxed
//     non-empty probe + `countr_zero` (TZCNT) to pick the top non-empty band, then the `Anti`
//     policy injects anti-starvation (RotatingReserve<M> default — bound (d+1)·K·M select turns).
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "quark/core/config.hpp"
#include "pal/pal.hpp"

namespace quark {

class Activation;  // fwd — the run-queue only moves Activation* by reference through a Schedulable.

// A schedulable unit: an Activation plus its intrusive run-queue link + resolved placement.
// The engine owns exactly one Schedulable per registered activation; the exec-state machine
// guarantees an activation is Scheduled in exactly ONE band of ONE shard at a time, so the single
// `rq_next` link is never contended for membership.
struct Schedulable {
    std::atomic<Schedulable*> rq_next{nullptr};  // intrusive Vyukov link — MUST be the first member
    Activation* activation = nullptr;            // the borrowed activation (lane, not owner)
    std::uint32_t shard = 0;                     // stable ActorId→shard (002 §Sharding)
    std::uint32_t budget = 0;                     // resolved per-actor DrainBudget (0 ⇒ engine default)
    std::uint16_t band = 0;                       // priority band, resolved at registration (ADR-010)
    std::uint16_t reserved = 0;
};

// Result of one consumer select step. Emptiness is non-linearizable (ADR-002/010): the transient
// publish window is surfaced as `Busy`, never folded into `Empty`.
enum class RunStatus : std::uint8_t { Item, Empty, Busy };
struct RunResult {
    Schedulable* item = nullptr;  // valid iff status == Item
    RunStatus status = RunStatus::Empty;
};

// The per-band intrusive Vyukov MPSC of Schedulables (single consumer). Mirrors the proven 003
// mailbox structure (ADR-002/003/004) but threads Schedulable* instead of Descriptor*.
class ActivationMpsc {
public:
    ActivationMpsc() noexcept : head_(&stub_), tail_(&stub_) {
        stub_.rq_next.store(nullptr, std::memory_order_relaxed);
    }
    ActivationMpsc(const ActivationMpsc&) = delete;
    ActivationMpsc& operator=(const ActivationMpsc&) = delete;
    ActivationMpsc(ActivationMpsc&&) = delete;
    ActivationMpsc& operator=(ActivationMpsc&&) = delete;

    // Producer (many threads): wait-free push. One acq_rel exchange + one release link store — a
    // full StoreLoad barrier on x86-TSO, so the engine's subsequent Dekker load needs no extra
    // fence there (producer-side elision, ADR-004/015).
    QUARK_ALWAYS_INLINE void enqueue(Schedulable* s) noexcept { link_push(s); }

    // Consumer (exactly one worker at a time): pop one activation with plain head_ loads.
    [[nodiscard]] RunResult try_pop() noexcept {
        Schedulable* front = head_;
        Schedulable* next = front->rq_next.load(std::memory_order_acquire);

        if (front == &stub_) {
            if (next == nullptr) {
                if (tail_.load(std::memory_order_acquire) == &stub_)
                    return {nullptr, RunStatus::Empty};
                return {nullptr, RunStatus::Busy};  // producer mid-publish
            }
            head_ = next;
            front = next;
            next = front->rq_next.load(std::memory_order_acquire);
        }

        if (next != nullptr) {
            head_ = next;
            return {front, RunStatus::Item};
        }

        Schedulable* tail = tail_.load(std::memory_order_acquire);
        if (front != tail) return {nullptr, RunStatus::Busy};

        link_push(&stub_);  // re-arm the stub (occupancy-1 boundary only)
        next = front->rq_next.load(std::memory_order_acquire);
        if (next != nullptr) {
            head_ = next;
            return {front, RunStatus::Item};
        }
        return {nullptr, RunStatus::Busy};
    }

    // Relaxed non-empty hint for the O(K) band-select bitmap (ADR-010 F2). A hint only — the pop
    // does the proper ordered disambiguation and Busy detection.
    [[nodiscard]] QUARK_ALWAYS_INLINE bool nonempty_hint() const noexcept {
        return tail_.load(std::memory_order_relaxed) != &stub_;
    }

    // Acquire-ordered work probe for the drain-owner close-out re-check AND the cross-shard park
    // rescan (engine). It compares the shared tail_ to the CONSTANT &stub_ and NEVER touches the
    // consumer-private head_ (ADR-010 normative): a worker that has released drain-ownership — or one
    // probing a shard another lane is draining — must not read head_, which the owner is mutating in
    // try_pop(). `tail_ != &stub_` is a complete emptiness test: the consumer re-arms the stub
    // (tail_ = &stub_) exactly when it drains the last node, so tail_ points off the stub iff a
    // producer has published work (linked or mid-publish — both mean "do not park").
    [[nodiscard]] bool has_work() const noexcept {
        return tail_.load(std::memory_order_acquire) != &stub_;
    }

private:
    QUARK_ALWAYS_INLINE void link_push(Schedulable* s) noexcept {
        s->rq_next.store(nullptr, std::memory_order_relaxed);
        Schedulable* prev = tail_.exchange(s, std::memory_order_acq_rel);
        prev->rq_next.store(s, std::memory_order_release);
    }

    Schedulable* head_;                             // consumer-private drain cursor (plain loads)
    QUARK_CACHE_ALIGNED std::atomic<Schedulable*> tail_;  // producer-shared tail (the contended atomic)
    QUARK_CACHE_ALIGNED Schedulable stub_;          // sentinel on its own line (off head_)
};

// ============================================================================================
// Anti-starvation policies (ADR-010 — consumer-local, NON-atomic; only the drain-owner touches it).
// ============================================================================================

// RotatingReserve<M> (DEFAULT): strict top-band-first, except every M-th SELECT TURN (a turn = a
// select that found work) it services the next non-empty band under a round-robin cursor. Bound:
// any band at priority-distance d from the top is serviced within (d+1)·K·M select turns — proven
// tight for EVERY band incl. middles (ADR-010 C2). Tunable via M, independent of priority spread.
template <std::size_t M>
struct RotatingReserve {
    static_assert(M >= 1, "RotatingReserve<M> needs M >= 1");
    std::size_t turn = 0;
    std::size_t cursor = 0;

    // `mask` has bit b set iff band b is (hint-)non-empty; guaranteed non-zero by the caller.
    template <std::size_t K>
    [[nodiscard]] QUARK_ALWAYS_INLINE std::size_t pick(std::uint32_t mask) noexcept {
        ++turn;
        if (turn % M == 0) {
            for (std::size_t i = 0; i < K; ++i) {
                const std::size_t b = (cursor + i) % K;
                if (mask & (1u << b)) {
                    cursor = (b + 1) % K;
                    return b;
                }
            }
        }
        return static_cast<std::size_t>(std::countr_zero(mask));  // strict top band (0 = highest)
    }
};

// WeightedDRR<W...> — proportional deficit-round-robin (ADR-010 alternative; share = w_i/Σw). Kept
// for completeness; RotatingReserve is the safer default (WeightedDRR stretches suspend-heavy tails).
template <std::uint16_t... W>
struct WeightedDRR {
    static constexpr std::array<std::uint16_t, sizeof...(W)> weights{W...};
    std::array<std::int32_t, sizeof...(W)> deficit{};
    std::size_t rr = 0;

    template <std::size_t K>
    [[nodiscard]] std::size_t pick(std::uint32_t mask) noexcept {
        static_assert(sizeof...(W) == K, "WeightedDRR needs one weight per band");
        for (std::size_t i = 0; i < K; ++i) {
            const std::size_t b = (rr + i) % K;
            if (!(mask & (1u << b))) continue;
            deficit[b] += weights[b];
            if (deficit[b] > 0) {
                --deficit[b];
                rr = b;  // stay on this band until its deficit is spent
                return b;
            }
        }
        // All non-empty bands are deficit-starved this round — replenish and take the top band.
        const std::size_t top = static_cast<std::size_t>(std::countr_zero(mask));
        deficit[top] = weights[top] - 1;
        rr = top;
        return top;
    }
};

// ============================================================================================
// Scheduling policies (005 CRTP surface, resolved at compile time — no virtual on the hot path).
// ============================================================================================

// DEFAULT — K==1. A DISTINCT TYPE (not PriorityBands<1>) so the uniform case pays nothing.
struct UniformFIFO {
    static constexpr std::size_t bands = 1;
};

// Opt-in K-band priority run-queue, 1 < K ≤ 8 (ADR-010 cap).
template <std::size_t K, class Anti = RotatingReserve<8>>
struct PriorityBands {
    static_assert(K >= 1 && K <= 8, "K must be in [1,8] (ADR-010: K>8 breaches the local-tell budget)");
    static constexpr std::size_t bands = K;
    using anti = Anti;
};

// --- The per-shard run-queue, specialized on the scheduling policy ---------------------------

template <class Policy>
class RunQueue;  // primary left undefined — only the two policy shapes below are valid.

// UniformFIFO: a single MPSC, forwarded verbatim. No band array, no bitmap, no anti-starvation
// state — objdump-byte-identical to using ActivationMpsc directly (the ADR-010 F1 zero-cost gate).
template <>
class RunQueue<UniformFIFO> {
public:
    QUARK_ALWAYS_INLINE void enqueue(Schedulable* s) noexcept { q_.enqueue(s); }
    [[nodiscard]] QUARK_ALWAYS_INLINE RunResult select() noexcept { return q_.try_pop(); }
    [[nodiscard]] bool has_work() const noexcept { return q_.has_work(); }
    static constexpr std::size_t bands = 1;

private:
    ActivationMpsc q_;
};

// PriorityBands: K FIFO bands + a compile-time-subscript enqueue + an O(K) relaxed probe select.
template <std::size_t K, class Anti>
class RunQueue<PriorityBands<K, Anti>> {
public:
    // Enqueue: pick the band by subscript on the SAME single exchange — 0 added cross-core RMW.
    QUARK_ALWAYS_INLINE void enqueue(Schedulable* s) noexcept {
        bands_[s->band < K ? s->band : K - 1].enqueue(s);
    }

    // Select: relaxed O(K) non-empty probe → bitmap; anti-starvation picks the band; pop it with a
    // bounded Busy spin (a single-pass non-spinning probe can strand a lone-wakeup activation).
    [[nodiscard]] RunResult select() noexcept {
        std::uint32_t mask = 0;
        for (std::size_t b = 0; b < K; ++b)
            mask |= static_cast<std::uint32_t>(bands_[b].nonempty_hint()) << b;
        if (mask == 0) return {nullptr, RunStatus::Empty};

        bool saw_busy = false;
        // The hint can be stale (relaxed). Try the anti-starvation pick first, then fall through
        // the remaining non-empty bands in strict priority order so a spent hint never wins.
        const std::size_t first = anti_.template pick<K>(mask);
        for (std::size_t attempt = 0; attempt < K; ++attempt) {
            const std::size_t b = (attempt == 0)
                                      ? first
                                      : static_cast<std::size_t>(std::countr_zero(mask));
            if (!(mask & (1u << b))) continue;
            const RunResult r = bands_[b].try_pop();
            if (r.status == RunStatus::Item) return r;
            if (r.status == RunStatus::Busy) saw_busy = true;
            mask &= ~(1u << b);  // this band drained out from under us; drop it and try the next
            if (mask == 0) break;
        }
        return {nullptr, saw_busy ? RunStatus::Busy : RunStatus::Empty};
    }

    [[nodiscard]] bool has_work() const noexcept {
        for (std::size_t b = 0; b < K; ++b)
            if (bands_[b].has_work()) return true;
        return false;
    }
    static constexpr std::size_t bands = K;

private:
    std::array<ActivationMpsc, K> bands_{};
    [[no_unique_address]] Anti anti_{};
};

}  // namespace quark
