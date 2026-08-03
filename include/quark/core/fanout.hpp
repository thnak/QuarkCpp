// Implements 006-Messaging-and-Addressing §Publish/Subscribe (ordered + reliable fan-out) as decided
// by ADR-039 (winner: `FanOut<M, Policy>` — N independent per-subscriber SPSC lanes). A single
// producer publishes an ORDERED stream to N dynamically attaching/detaching subscribers with
// PER-(publisher,subscriber) FIFO and no silent loss while attached — the middle ground between
// `ReplyStream<F>` (ADR-018, ordered but single-consumer) and `Topic<M>` (ADR-019, N-subscriber but
// best-effort/silent-drop).
//
// MECHANISM (ADR-039 winner, reusing two already-proven mechanisms verbatim):
//   * MEMBERSHIP is `Topic<M>`'s exact `std::atomic<std::shared_ptr<const SubVec>>` COW snapshot +
//     bounded-quiescence unsubscribe (ADR-019 GATE 6) — subscribe/unsubscribe rebuild under a mutex
//     (COLD), publish() only loads the snapshot (HOT).
//   * PAYLOAD is `Topic<M>`'s exact `SharedPayload<M>` / `SharedPayloadPool<M>` (ADR-019/003) — one
//     shared refcounted payload per publish, reclaimed exactly once regardless of how many/which
//     subscribers consumed, dropped, or departed.
//   * Each subscriber owns one `StreamChannel<FanOutEnvelope<M>>` (ADR-018's credit ring, reused
//     verbatim) as its lane: genuinely SPSC because this problem is single-producer (the same
//     precondition `StreamChannel` already assumes). A lane holds thin 16 B
//     `FanOutEnvelope<M>{payload*, id}` descriptors, never M itself.
//   * `OnSlowSubscriber<EvictAfter<N>>`: `try_push` failing on a full lane IS the eviction signal — a
//     per-message drop with an exactly-once, non-silent counter bump at the FanOut boundary
//     (`LaneEntry::evicted()`); the subscriber stays attached (this is bounded-lag, not ejection).
//   * `OnSlowSubscriber<Block>`: `push_blocking_while` (stream_channel.hpp) parks the producer on that
//     one lane until it drains or the subscriber departs; `unsubscribe()` flips the lane's `active_`
//     flag AND calls `notify_departure()` on it so a producer already asleep is released (without the
//     explicit wake this deadlocks — ADR-039 S1's firing control).
//   * Both policies are `if constexpr`-dispatched off `Policy::mode` — no runtime discriminator field
//     exists anywhere in the type (ADR-039 F2: zero cross-policy symbols).
//   * RECLAIM of a departed/evicted lane's still-queued envelopes happens in `~LaneEntry()` ONLY, once
//     every `shared_ptr<LaneEntry>` reference (the membership snapshot's + the subscriber's own drain
//     handle) has dropped — mirroring `BoundedInbox<M>::~BoundedInbox` (topic.hpp). The original design
//     ran this reclaim on the PRODUCER thread instead and that was a confirmed cross-subscriber UAF
//     (second-writer violation of `StreamChannel`'s consumer-only cursor API) — ADR-039 S2r's fix.
//
// PRECONDITION (load-bearing, not incidental): `publish()` is single-producer — exactly one
// thread/actor calls it per `FanOut` instance. Multi-producer fan-in needs an external serializing
// actor; `FanOut` does not arbitrate producers (ADR-039 residual risks).
//
// x86-TSO ONLY, same basis as `Topic<M>`/`StreamChannel<F>` — AArch64 weak-memory re-gate deferred.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "quark/core/descriptor.hpp"      // quark::MessageId
#include "quark/core/ids.hpp"             // quark::ActorId
#include "quark/core/stream_channel.hpp"  // quark::StreamChannel
#include "quark/core/topic.hpp"           // quark::SharedPayload, quark::SharedPayloadPool

namespace quark {

// --- OnSlowSubscriber<Mode> policy tags (CRTP, compile-time only) -----------------------------
// Same flat-tag idiom as `OnRestartAsk<Mode>` / `OnResourceFailure<Mode>` (supervision.hpp).
template <std::size_t N>
struct EvictAfter {
    static_assert(N > 0, "EvictAfter<N> needs a nonzero lane capacity");
    static constexpr std::size_t capacity = N;
};
struct Block {};
template <class Mode = Block>
struct OnSlowSubscriber {
    using mode = Mode;
};

namespace detail {
template <class T>
concept FanOutHasEvictCapacity = requires { { T::capacity } -> std::convertible_to<std::size_t>; };
}  // namespace detail

// The result of one publish (006 §PublishReceipt analogue for the reliable primitive). Under
// `EvictAfter`, `evicted` is the exactly-once FanOut-boundary drop signal for this publish; under
// `Block`, `departed` counts lanes that unsubscribed while this publish was parked on them. The other
// field is always 0 for a given policy instantiation.
struct FanOutReceipt {
    std::uint32_t delivered = 0;
    std::uint32_t evicted = 0;
    std::uint32_t departed = 0;
};

// A lane slot: a reference to the shared payload + its (per-publisher) sequence id. 16 B, trivially
// copyable — required by `StreamChannel<F>`'s inline-slot regime.
template <class M>
struct FanOutEnvelope {
    SharedPayload<M>* payload = nullptr;
    MessageId id{};
};

template <class M, class Policy = OnSlowSubscriber<Block>>
class FanOut {
    using Mode = typename Policy::mode;
    using Channel = StreamChannel<FanOutEnvelope<M>>;

    static constexpr std::size_t kDefaultLaneCapacity =
        detail::FanOutHasEvictCapacity<Mode> ? Mode::capacity : std::size_t{256};

public:
    // ============================================================================================
    // LaneEntry — the per-subscriber handle. Returned by subscribe(); the subscriber drains it
    // directly. Reclaimed automatically (drains any still-queued envelopes' payload refs) once its
    // last shared_ptr reference drops (ADR-039 S2r) — never touched by the producer after departure.
    // ============================================================================================
    class LaneEntry {
    public:
        LaneEntry(const typename Channel::Config& cfg, std::pmr::memory_resource* mr) : channel_(cfg, mr) {}
        LaneEntry(const LaneEntry&) = delete;
        LaneEntry& operator=(const LaneEntry&) = delete;

        ~LaneEntry() {
            while (channel_.occupancy() > 0) {
                const std::uint64_t idx = channel_.advance_dispatch();
                const FanOutEnvelope<M> e = channel_.slot_at(idx);
                if (e.payload) e.payload->release();
                channel_.advance_tail();
            }
        }

        // Single-consumer drain (the subscriber's own call): pop the oldest envelope, copy its
        // payload out by value, release the shared ref. false == lane currently empty.
        //
        // poll_unstall() here is what makes `Block`'s push_blocking_while ever wake on ORDINARY
        // draining (not just on departure): advance_tail() just returned a credit slot to the
        // producer, and poll_unstall() is the disarm-and-wake half of that credit-return's reverse
        // Dekker rendezvous. Without it, a producer parked here would only ever be released by
        // notify_departure() (unsubscribe) — a live, still-attached slow subscriber would stall its
        // producer forever even after making room, which is not what "bounded by the slowest LIVE
        // subscriber" (ADR-039 C3) means. A no-op (one relaxed-ish fence + a false exchange) on the
        // EvictAfter instantiation, since that policy never arms `stalled_`.
        [[nodiscard]] bool try_pop(M& out) {
            if (channel_.occupancy() == 0) return false;
            const std::uint64_t idx = channel_.advance_dispatch();
            const FanOutEnvelope<M> e = channel_.slot_at(idx);
            out = e.payload->value();
            e.payload->release();
            channel_.advance_tail();
            channel_.poll_unstall();
            return true;
        }

        // Exactly-once, non-silent FanOut-boundary drop count (EvictAfter only; always 0 under Block).
        // This is NOT an end-to-end delivery guarantee to the subscriber's own mailbox — it is the
        // FanOut lane's own observable, distinct from whatever backpressure the mailbox applies next.
        [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_.load(std::memory_order_relaxed); }
        [[nodiscard]] std::uint32_t capacity() const noexcept { return channel_.capacity(); }
        [[nodiscard]] std::uint64_t occupancy() const noexcept { return channel_.occupancy(); }

    private:
        friend class FanOut;
        Channel channel_;
        std::atomic<bool> active_{true};      // false once unsubscribed; also Block's keep_going flag
        std::atomic<std::uint64_t> evicted_{0};
    };

    explicit FanOut(std::size_t lane_capacity = kDefaultLaneCapacity, std::size_t payload_pool_warm = 1024,
                     std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : mr_(mr), lane_capacity_(lane_capacity), pool_(payload_pool_warm) {
        snapshot_.store(std::make_shared<const SubVec>(), std::memory_order_release);
    }
    FanOut(const FanOut&) = delete;
    FanOut& operator=(const FanOut&) = delete;

    // --- COLD path: membership -------------------------------------------------------------------
    // Idempotent subscribe with ActorId set-semantics dedup (mirrors Topic<M>): returns nullptr if
    // `id` is already subscribed. The returned shared_ptr IS the subscriber's own drain handle — hold
    // it until you are done consuming; dropping it (after unsubscribe()) is what allows reclaim.
    [[nodiscard]] std::shared_ptr<LaneEntry> subscribe(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        auto cur = snapshot_.load(std::memory_order_acquire);
        for (const Sub& s : *cur)
            if (s.id == id) return nullptr;
        auto lane = std::make_shared<LaneEntry>(
            typename Channel::Config{static_cast<std::uint32_t>(lane_capacity_)}, mr_);
        auto next = std::make_shared<SubVec>(*cur);
        next->push_back(Sub{id, lane});
        snapshot_.store(std::shared_ptr<const SubVec>(std::move(next)), std::memory_order_release);
        return lane;
    }

    // Bounded-quiescence unsubscribe (ADR-019 GATE 6, reused verbatim): flag the lane inactive, swap
    // in the new snapshot, then (Block only) force-wake a producer that may be parked on this exact
    // lane, then wait for in_flight==0. Never blocks the PUBLISHER thread of a DIFFERENT publish.
    bool unsubscribe(ActorId id) {
        std::shared_ptr<LaneEntry> departing;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto cur = snapshot_.load(std::memory_order_acquire);
            const Sub* found = nullptr;
            for (const Sub& s : *cur)
                if (s.id == id) { found = &s; break; }
            if (!found) return false;
            found->lane->active_.store(false, std::memory_order_release);
            departing = found->lane;
            auto next = std::make_shared<SubVec>();
            next->reserve(cur->size() - 1);
            for (const Sub& s : *cur)
                if (!(s.id == id)) next->push_back(s);
            snapshot_.store(std::shared_ptr<const SubVec>(std::move(next)), std::memory_order_release);
        }
        if constexpr (!detail::FanOutHasEvictCapacity<Mode>) {
            // Without this, a publish already parked in push_blocking_while on this lane has no wakeup
            // to observe and unsubscribe()'s quiescence wait below deadlocks (ADR-039 S1 firing control).
            departing->channel_.notify_departure();
        }
#ifndef QUARK_FANOUT_NO_QUIESCE
        while (in_flight_.load(std::memory_order_acquire) != 0) std::this_thread::yield();
#endif
        // CONTROL (-DQUARK_FANOUT_NO_QUIESCE): skip the barrier — a publish that already loaded the old
        // snapshot may still be mid-push into a lane the caller is free to destroy right after this
        // returns, an ASan-trapped heap-use-after-free (ADR-019 GATE 6 firing control, reused here).
        return true;
    }

    [[nodiscard]] std::size_t subscriber_count() const noexcept {
        return snapshot_.load(std::memory_order_acquire)->size();
    }

    // --- HOT path: publish -----------------------------------------------------------------------
    // Fan ONE shared refcounted payload out over the current snapshot, ordered per-(publisher,
    // subscriber). Single-producer precondition (see header note) — this method is not safe to call
    // concurrently with itself.
    FanOutReceipt publish(M message) {
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        FanOutReceipt r{};
        auto snap = snapshot_.load(std::memory_order_acquire);  // shared_ptr pins the vector
        if (snap && !snap->empty()) {
            SharedPayload<M>* pl = pool_.acquire(std::move(message));  // rc = 1 (build ref)
            const MessageId id{seq_.fetch_add(1, std::memory_order_relaxed)};
            for (const Sub& s : *snap) {
                if (!s.lane->active_.load(std::memory_order_acquire)) continue;  // just-unsubscribed
                pl->retain();  // rc++ for this lane, BEFORE the push publishes it
                const FanOutEnvelope<M> env{pl, id};
                if constexpr (detail::FanOutHasEvictCapacity<Mode>) {
                    if (s.lane->channel_.try_push(env)) {
                        ++r.delivered;
                    } else {
                        pl->release();  // full -> evict this message for this lane (bounded-lag, not ejection)
                        s.lane->evicted_.fetch_add(1, std::memory_order_relaxed);
                        ++r.evicted;
                    }
                } else {
                    if (s.lane->channel_.push_blocking_while(env, s.lane->active_)) {
                        ++r.delivered;
                    } else {
                        pl->release();  // departed mid-block -> undo the retain, never delivered
                        ++r.departed;
                    }
                }
            }
            pl->release();  // drop the BUILD ref; reclaims here iff every lane already consumed
        }
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        return r;
    }

    [[nodiscard]] SharedPayloadPool<M>& pool() noexcept { return pool_; }
    [[nodiscard]] std::uint64_t in_flight() const noexcept { return in_flight_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t lane_capacity() const noexcept { return lane_capacity_; }

private:
    struct Sub {
        ActorId id;
        std::shared_ptr<LaneEntry> lane;
    };
    using SubVec = std::vector<Sub>;

    std::mutex mu_;                                        // registry (subscribe/unsubscribe) COLD
    std::atomic<std::shared_ptr<const SubVec>> snapshot_;  // immutable COW membership (safe-acquire)
    std::atomic<std::uint64_t> in_flight_{0};              // publishes that may hold the old snapshot
    std::atomic<std::uint64_t> seq_{0};                    // per-FanOut message sequence (017 id)
    std::pmr::memory_resource* mr_;
    std::size_t lane_capacity_;
    SharedPayloadPool<M> pool_;
};

}  // namespace quark
