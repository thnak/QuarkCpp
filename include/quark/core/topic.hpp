// Implements 006-Messaging-and-Addressing §Publish/Subscribe (broadcast) as decided by ADR-019
// (winner: `Topic<M>` best-effort at-most-once fan-out). A publisher fires a message to MANY
// subscribers WITHOUT caring who or how many are listening — fan out and fire-and-forget. The
// load-bearing semantic (GATE 1) is that the publisher NEVER blocks and NEVER stalls on any
// subscriber: a slow / full / dead subscriber is DROPPED for (per-subscriber, counted), it does NOT
// back-pressure the publisher. This is the deliberate OPPOSITE of the outbound streaming reply
// (ADR-018), which HAS credit backpressure.
//
// MECHANISM (ADR-019 winner D-A, all three designs converged on this safe architecture):
//   * MEMBERSHIP is an `std::atomic<std::shared_ptr<const SubVec>>` IMMUTABLE copy-on-write snapshot
//     — a safe-acquire SMR whose control block keeps the subscriber vector alive across the
//     publisher's load+walk. D-B's disqualifying bug was a hand-rolled refcount reached through a
//     mutable head_ pointer (a heap-use-after-free on the clean build under both compilers); the
//     atomic<shared_ptr> snapshot is exactly what avoids it. subscribe/unsubscribe rebuild the vector
//     under a mutex (COLD path); the publish HOT path only loads the snapshot.
//   * DELIVERY is N ordinary tells sharing ONE immutable refcounted `SharedPayload<M>` — 1 payload
//     copy per publish, independent of N and sizeof(M) (NOT N copies — the 022 amplification hazard).
//     Each subscriber has a bounded inbox with PER-SUBSCRIBER DROP-ON-FULL (at-most-once), so a full
//     inbox drops rather than stalling the fan-out.
//   * The shared payload is reclaimed EXACTLY ONCE (refcount to zero) whether a subscriber consumes,
//     drops, unsubscribes, or dies — the GATE-4 lifetime gate (ASan+TSan, firing controls in ADR-019).
//   * UNSUBSCRIBE is BOUNDED QUIESCENCE: set the entry's `active=false`, publish the new snapshot,
//     then wait for in_flight==0 — so no publish that loaded the old snapshot still references the
//     departing subscriber's inbox once unsubscribe() returns. It never awaits, never delays the
//     PUBLISHER (GATE 6).
//
// WHAT IS PROVEN vs WHAT IS A SEAM (ADR-019 §Residual risks, honestly reproduced):
//   * This header is the std-only core of the LOCAL fan-out primitive: membership snapshot, shared
//     refcounted payload + pool, per-subscriber bounded drop-on-full inbox, bounded-quiescence
//     unsubscribe. Wiring delivery onto the REAL engine Mailbox + Descriptor pool + exec-state
//     scheduler is the integration seam — GATE 5's specific at-most-one-executor exec-state CAS was
//     INCONCLUSIVE in the ADR's own harness (no scheduler modelled); only the single-consumer / FIFO
//     / no-loss inbox invariant it protects is proven here. The `BoundedInbox<M>` below is that
//     provable inbox; the engine mailbox is the drop-in the addressing layer lowers onto.
//   * CROSS-NODE fan-out stays Draft (GATE 7): the by-node coalescing + relay-tree are 010/026 seams,
//     proven only on an in-process simulated transport. Not implemented here.
//
// x86-TSO ONLY. The refcount acq_rel, the active-flag acquire/release, the atomic<shared_ptr> swap,
// and the in_flight barrier are proven on x86-TSO; the AArch64 weak-memory re-gate defers (ADR-019).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/descriptor.hpp"  // quark::MessageId
#include "quark/core/ids.hpp"         // quark::ActorId

namespace quark {

// The result of one publish (ADR-019 §006: PublishReceipt). All best-effort counts; the publisher
// reads them to observe fan-out, never to retry (retry would reintroduce a stall — 017).
struct PublishReceipt {
    std::uint32_t delivered = 0;         // subscribers whose inbox accepted the message
    std::uint32_t dropped_full = 0;      // subscribers whose bounded inbox was full (at-most-once drop)
    std::uint32_t dropped_deadline = 0;  // reserved: deadline-expired drops (018) — 0 in the core
    std::uint32_t remote = 0;            // reserved: distinct remote nodes fanned to (010 seam) — 0 here
};

// ============================================================================================
// SharedPayload<M> — one immutable, pool-allocated, refcounted payload per publish (ADR-019 §003).
// `rc` sits on its OWN cache line, padded away from the immutable M, so the read-side (subscribers
// reading M) and the reclaim-side (refcount decrements) do not false-share. The refcount protocol:
//   * acquire()      -> rc = 1  (the publisher BUILD ref, held for the whole fan-out)
//   * retain()       -> rc += 1 (per admitted enqueue; relaxed — the build ref keeps the object alive)
//   * release()      -> rc -= 1 (acq_rel); the decrement that hits 0 runs ~M and returns the cell.
// So even if a subscriber consumes+releases mid-fan-out, the build ref keeps M alive until the
// publisher's final release() — reclaimed EXACTLY ONCE (GATE 4).
// ============================================================================================
template <class M>
class SharedPayloadPool;

template <class M>
class SharedPayload {
public:
    [[nodiscard]] const M& value() const noexcept {
        return *std::launder(reinterpret_cast<const M*>(storage_));
    }

    void retain() noexcept { rc_.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept;  // defined after the pool

    [[nodiscard]] std::uint32_t use_count() const noexcept { return rc_.load(std::memory_order_acquire); }

private:
    friend class SharedPayloadPool<M>;

    // Construct M in place and arm the build ref. Pool-lane only (mutex held).
    void arm(M&& m, SharedPayloadPool<M>* pool) {
        pool_ = pool;
        ::new (storage_) M(std::move(m));
        rc_.store(1, std::memory_order_relaxed);  // the publisher BUILD ref
    }

    QUARK_CACHE_ALIGNED std::atomic<std::uint32_t> rc_{0};  // own line — no false sharing with M
    SharedPayloadPool<M>* pool_ = nullptr;
    alignas(M) unsigned char storage_[sizeof(M)];
};

// A warm pool of SharedPayload<M> cells so publish() does 0 malloc after warmup (ADR-019 §004:
// pool-resolved). Mutex-guarded (acquire on publish lanes, reclaim on consumer/drop lanes) — a lock,
// not a heap allocation, so steady-state publish is 0 pool-upstream allocation.
template <class M>
class SharedPayloadPool {
public:
    explicit SharedPayloadPool(std::size_t warm) {
        storage_.reserve(warm);
        free_.reserve(warm);
        for (std::size_t i = 0; i < warm; ++i) {
            auto c = std::make_unique<SharedPayload<M>>();
            free_.push_back(c.get());
            storage_.push_back(std::move(c));
        }
    }
    SharedPayloadPool(const SharedPayloadPool&) = delete;
    SharedPayloadPool& operator=(const SharedPayloadPool&) = delete;

    // Take a cell, construct M, arm rc=1. Cold-grows only if the warm set is exhausted.
    [[nodiscard]] SharedPayload<M>* acquire(M&& m) {
        SharedPayload<M>* cell;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (free_.empty()) {
                auto n = std::make_unique<SharedPayload<M>>();
                cell = n.get();
                storage_.push_back(std::move(n));
            } else {
                cell = free_.back();
                free_.pop_back();
            }
        }
        cell->arm(std::move(m), this);
        return cell;
    }

    // Return a cell whose M has already been destroyed by the final release().
    void reclaim(SharedPayload<M>* cell) {
        std::lock_guard<std::mutex> g(mu_);
        free_.push_back(cell);
    }

    [[nodiscard]] std::size_t free_count() {
        std::lock_guard<std::mutex> g(mu_);
        return free_.size();
    }

private:
    std::mutex mu_;
    std::vector<std::unique_ptr<SharedPayload<M>>> storage_;
    std::vector<SharedPayload<M>*> free_;
};

template <class M>
inline void SharedPayload<M>::release() noexcept {
    if (rc_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // The decrement to zero: run the type dtor, then return the cell to its pool. acq_rel above
        // synchronizes with every prior release so this is the sole, exactly-once reclamation.
        std::launder(reinterpret_cast<M*>(storage_))->~M();
        pool_->reclaim(this);
    }
}

// One inbox slot: a reference to the shared payload + the per-message id (017: (sender,seq), carried
// for per-(pub,sub) FIFO/observability only, NEVER for dedup). Trivially copyable.
template <class M>
struct BroadcastEnvelope {
    SharedPayload<M>* payload = nullptr;
    MessageId id{};
};

// ============================================================================================
// BoundedInbox<M> — the per-subscriber sink: a fixed-capacity Vyukov bounded MPMC queue of
// BroadcastEnvelope<M> with DROP-ON-FULL (at-most-once) push. Producers = publisher lanes (there may
// be several — many topics / publishers target one subscriber), consumer = the subscriber's actor
// lane. `try_push` returns false when full (the caller drops + releases the payload ref); it NEVER
// blocks (GATE 1). This is the provable stand-in for the engine mailbox (see header note): single-
// consumer FIFO, bounded, no loss beyond the counted drop.
// ============================================================================================
template <class M>
class BoundedInbox {
public:
    explicit BoundedInbox(std::uint32_t capacity)
        : cap_(round_up_pow2(capacity == 0 ? 1u : capacity)), mask_(cap_ - 1),
          cells_(std::make_unique<Cell[]>(cap_)) {
        for (std::uint64_t i = 0; i < cap_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }
    BoundedInbox(const BoundedInbox&) = delete;
    BoundedInbox& operator=(const BoundedInbox&) = delete;

    // Drain remaining envelopes and release their payload refs, so a subscriber that DIES with queued
    // broadcasts does not leak the shared payloads (GATE 4 — reclaim on die).
    ~BoundedInbox() {
        BroadcastEnvelope<M> e;
        while (try_pop(e)) {
            if (e.payload) e.payload->release();
        }
    }

    // Non-blocking, lossless-or-dropped. false == full (the caller drops this copy). Multiple
    // producers safe (fetch-free CAS on enqueue_pos_).
    [[nodiscard]] bool try_push(const BroadcastEnvelope<M>& env) noexcept {
        Cell* cell;
        std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const std::int64_t dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;  // full -> DROP (at-most-once; publisher never stalls)
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->env = env;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Single (or multi) consumer pop. false == empty.
    [[nodiscard]] bool try_pop(BroadcastEnvelope<M>& out) noexcept {
        Cell* cell;
        std::uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const std::int64_t dif = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->env;
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return cap_; }

    static constexpr std::uint32_t round_up_pow2(std::uint32_t x) noexcept {
        if (x <= 1) return 1;
        --x;
        x |= x >> 1;  x |= x >> 2;  x |= x >> 4;  x |= x >> 8;  x |= x >> 16;
        return x + 1;
    }

private:
    struct Cell {
        std::atomic<std::uint64_t> seq;
        BroadcastEnvelope<M> env{};
    };

    std::uint32_t cap_;
    std::uint64_t mask_;
    std::unique_ptr<Cell[]> cells_;
    QUARK_CACHE_ALIGNED std::atomic<std::uint64_t> enqueue_pos_{0};
    QUARK_CACHE_ALIGNED std::atomic<std::uint64_t> dequeue_pos_{0};
};

// ============================================================================================
// Topic<M> — the best-effort broadcast primitive (ADR-019). Subscribers register a BoundedInbox<M>*;
// publish() fans one shared refcounted payload out over the immutable membership snapshot. The
// publisher never blocks. Zero cost when no Topic<M> is instantiated (nothing here is engine-global).
//
// Subscriber inbox lifetime is the SUBSCRIBER'S (ActorRef/007 discipline, ADR-019): the topic only
// references the inbox. After unsubscribe() returns, no publish references it (bounded quiescence),
// so the owner may then destroy it.
// ============================================================================================
template <class M>
class Topic {
public:
    explicit Topic(std::size_t payload_pool_warm = 1024)
        : pool_(payload_pool_warm) {
        snapshot_.store(std::make_shared<const SubVec>(), std::memory_order_release);
    }
    Topic(const Topic&) = delete;
    Topic& operator=(const Topic&) = delete;

    // --- COLD path: membership -------------------------------------------------------------------
    // Idempotent subscribe with ActorId set-semantics dedup: a double-subscribe yields ONE delivery
    // (returns false the second time). COW rebuild under the registry mutex; the publish hot path is
    // untouched (it only loads the snapshot).
    bool subscribe(ActorId id, BoundedInbox<M>* inbox) {
        std::lock_guard<std::mutex> g(mu_);
        auto cur = snapshot_.load(std::memory_order_acquire);
        for (const Sub& s : *cur)
            if (s.id == id) return false;  // set-dedup — already subscribed
        auto next = std::make_shared<SubVec>(*cur);
        next->push_back(Sub{id, inbox, std::make_shared<std::atomic<bool>>(true)});
        snapshot_.store(std::shared_ptr<const SubVec>(std::move(next)), std::memory_order_release);
        return true;
    }

    // Bounded-quiescence unsubscribe (GATE 6): flag the entry inactive (an in-flight publish that
    // loaded the old snapshot will skip it), publish the new snapshot, then wait for in_flight==0 so
    // no publish still references this subscriber's inbox once we return. Never blocks the PUBLISHER.
    bool unsubscribe(ActorId id) {
        {
            std::lock_guard<std::mutex> g(mu_);
            auto cur = snapshot_.load(std::memory_order_acquire);
            const Sub* found = nullptr;
            for (const Sub& s : *cur)
                if (s.id == id) { found = &s; break; }
            if (!found) return false;
            found->active->store(false, std::memory_order_release);  // in-flight publish skips it
            auto next = std::make_shared<SubVec>();
            next->reserve(cur->size() - 1);
            for (const Sub& s : *cur)
                if (!(s.id == id)) next->push_back(s);
            snapshot_.store(std::shared_ptr<const SubVec>(std::move(next)), std::memory_order_release);
        }
        // Bounded quiescence: spin until every publish that could hold the old snapshot has finished.
        // GATE 6 — this is what makes it safe to destroy the inbox after unsubscribe() returns.
#ifndef QUARK_TOPIC_NO_QUIESCE
        while (in_flight_.load(std::memory_order_acquire) != 0) std::this_thread::yield();
#endif
        // CONTROL (-DQUARK_TOPIC_NO_QUIESCE): skip the barrier. A publish that already loaded the old
        // snapshot then pushes into an inbox the caller destroys right after unsubscribe() returns —
        // a heap-use-after-free the ASan control binary traps (ADR-019 GATE 6 firing control).
        return true;
    }

    [[nodiscard]] std::size_t subscriber_count() const noexcept {
        return snapshot_.load(std::memory_order_acquire)->size();
    }

    // --- HOT path: publish -----------------------------------------------------------------------
    // Fan ONE shared refcounted payload out over the current snapshot. The publisher NEVER blocks: a
    // full subscriber inbox drops (counted). Reclamation is exactly-once via the build ref + per-
    // enqueue retain / per-consume-or-drop release.
    PublishReceipt publish(M message) {
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        PublishReceipt r{};
        auto snap = snapshot_.load(std::memory_order_acquire);  // shared_ptr pins the vector
        if (snap && !snap->empty()) {
            SharedPayload<M>* pl = pool_.acquire(std::move(message));  // rc = 1 (build ref)
            const MessageId id{seq_.fetch_add(1, std::memory_order_relaxed)};
            for (const Sub& s : *snap) {
                if (!s.active->load(std::memory_order_acquire)) continue;  // just-unsubscribed -> skip
                pl->retain();  // rc++ for this enqueue, BEFORE the push publishes it
                if (s.inbox->try_push(BroadcastEnvelope<M>{pl, id})) {
                    ++r.delivered;
                } else {
                    pl->release();  // full -> drop; undo the retain (may reclaim if build ref already gone — it isn't yet)
                    ++r.dropped_full;
                }
            }
            pl->release();  // drop the BUILD ref; reclaims here iff every subscriber already consumed
        }
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        return r;
    }

    // Consume-side helper: pop one broadcast from a subscriber inbox and return its payload by value,
    // releasing the shared ref exactly once. Returns false when the inbox is empty. (The real engine
    // dispatches the descriptor to the actor handler; this is the std-only consume the tests drive.)
    [[nodiscard]] static bool consume(BoundedInbox<M>& inbox, M& out) {
        BroadcastEnvelope<M> e;
        if (!inbox.try_pop(e)) return false;
        out = e.payload->value();     // copy the immutable payload out before releasing our ref
        e.payload->release();         // per-consume release (reclaims iff we were the last ref)
        return true;
    }

    [[nodiscard]] SharedPayloadPool<M>& pool() noexcept { return pool_; }
    [[nodiscard]] std::uint64_t in_flight() const noexcept {
        return in_flight_.load(std::memory_order_acquire);
    }

private:
    struct Sub {
        ActorId id;
        BoundedInbox<M>* inbox;
        std::shared_ptr<std::atomic<bool>> active;  // stable across snapshots; flagged false on leave
    };
    using SubVec = std::vector<Sub>;

    std::mutex mu_;                                            // registry (subscribe/unsubscribe) COLD
    std::atomic<std::shared_ptr<const SubVec>> snapshot_;      // immutable COW membership (safe-acquire)
    std::atomic<std::uint64_t> in_flight_{0};                 // publishes that may hold the old snapshot
    std::atomic<std::uint64_t> seq_{0};                       // per-topic message sequence (017 id)
    SharedPayloadPool<M> pool_;
};

}  // namespace quark
