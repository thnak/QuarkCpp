// Implements 006-Messaging-and-Addressing §tell + 003-Memory §Descriptor/§Payload — the send-side
// message source: a pooled Descriptor paired 1:1 with inline payload storage, so `tell`/`ask`
// construct a message with 0 heap allocation on the hot path (measured, ADR-007 F1 / 023).
//
// Each cell is {Descriptor, destroy-thunk, inline payload}. The Descriptor is the first member so a
// `Descriptor*` IS the cell address (offset-0, pointer-interconvertible — mirrors 003's mailbox
// intrusive-link rule); the free-list threads through the Descriptor's intrusive link while pooled.
// Reclaim (the Activation ReclaimSink, 002/003) runs the payload destructor, then recycles the cell.
//
// THREAD SAFETY: acquire() runs on arbitrary producer (send) lanes; reclaim() runs on the single
// per-shard drain lane. The free-list is mutex-guarded — a lock, never a heap allocation, so the
// 0-hot-path-alloc guarantee holds. This is NOT the drain path (the 0-cross-core-RMW invariant is
// about the sequential drain, 023), so a producer-side lock is in-budget.
//
// PARTITIONING (the 003/022 "per-producer caches" seam): a single shared free-list serializes every
// concurrent producer on one mutex regardless of which actor they target (measured: throughput
// flatlines under N producers whether messages spread across 100 or 100,000 actors — the pool, not
// the mailbox, was the bottleneck; see ADR-020's flagged-but-unproven "acquire() under
// producer/consumer imbalance" gap). `num_partitions` (ctor, default 1 == today's exact behavior)
// splits the free-list into independent partitions, one per calling thread's lane id (mod
// num_partitions). Each partition owns its OWN cell storage, so there is no second, pool-wide lock
// for growth. reclaim() — which runs on whichever worker drains the target shard, almost certainly a
// DIFFERENT thread than the one that acquired the cell — routes back to the cell's ORIGINAL
// partition (stamped once at construction), never the reclaiming thread's own lane, so partitions
// never need to coordinate with each other. This is thread-keyed, not shard-keyed: it removes
// producer-vs-producer contention but does not (yet) give the target shard's cache-locality that
// 003-Memory.md's "each shard owns its allocator" describes — a shard-keyed follow-up is a separate,
// larger seam (Engine-owned per-shard pools, spawn()'s reclaim-sink wiring, PostCourier).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "quark/core/activation.hpp"  // ReclaimSink
#include "quark/core/config.hpp"      // cache_line_size, QUARK_CACHE_ALIGNED
#include "quark/core/descriptor.hpp"

namespace quark::detail {

// Type-erased payload destructor. `void(*)(void*) noexcept` matches the pool's stored thunk.
template <class Msg>
void destroy_payload(void* p) noexcept {
    static_cast<Msg*>(p)->~Msg();
}

// A calling thread's stable lane id within this process, assigned once (lazily) from a single
// monotonic counter shared by every MessagePool. A pool reduces it mod its own partition count, so
// the same physical thread can land on different partition indices in different pools — harmless,
// since a partition only needs to be stable PER THREAD WITHIN ONE POOL, never coordinated globally.
// Race-free by construction: the thread_local is never touched cross-thread, and the atomic
// fetch_add only needs distinct values (no ordering with anything else), so relaxed suffices.
inline std::atomic<std::uint32_t> g_next_pool_lane_id{0};
inline thread_local std::uint32_t t_pool_lane_id = 0xFFFF'FFFFu;  // sentinel: not yet assigned

[[nodiscard]] inline std::uint32_t pool_lane_id() noexcept {
    if (t_pool_lane_id == 0xFFFF'FFFFu)
        t_pool_lane_id = g_next_pool_lane_id.fetch_add(1, std::memory_order_relaxed);
    return t_pool_lane_id;
}

class MessagePool {
public:
    // Inline payload capacity per cell. Sized for the local send path (small messages + an ask
    // envelope carrying a Responder). Oversized messages are a compile error at the send site.
    static constexpr std::size_t kMaxPayload = 192;
    static constexpr std::size_t kPayloadAlign = 16;

    struct Slot {
        Descriptor* desc = nullptr;
        void* payload = nullptr;
    };

    // `num_partitions` defaults to 1 — every existing single-arg call site keeps today's exact
    // behavior (one mutex, one free-list). Pass > 1 to spread producer-side contention across
    // independent partitions (see the file banner).
    explicit MessagePool(std::size_t capacity, std::size_t num_partitions = 1)
        : num_partitions_(num_partitions == 0 ? 1 : num_partitions),
          partitions_(std::make_unique<Partition[]>(num_partitions_)) {
        for (std::size_t i = 0; i < capacity; ++i) grow_one(partitions_[i % num_partitions_]);
    }
    MessagePool(const MessagePool&) = delete;
    MessagePool& operator=(const MessagePool&) = delete;

    // Hot path (send lane): pop a cell, arm its payload destructor, hand back the descriptor +
    // inline payload storage. Grows cold if the partition is exhausted (pre-size to avoid it).
    [[nodiscard]] Slot acquire(void (*destroy)(void*) noexcept) noexcept {
        Partition& p = partitions_[pool_lane_id() % num_partitions_];
        std::lock_guard<std::mutex> g(p.mu);
        if (p.free_head == nullptr) grow_one(p);  // cold — should not happen on a pre-sized pool
        Descriptor* d = p.free_head;
        p.free_head = d->link.next.load(std::memory_order_relaxed);
        Cell* c = cell_of(d);
        c->destroy = destroy;
        d->link.next.store(nullptr, std::memory_order_relaxed);
        return Slot{d, c->payload};
    }

    // Reclaim (drain lane): run the payload destructor (which, for an unanswered ask, fails the
    // ReplyCell — reply-before-teardown), bump the descriptor generation, and recycle the cell to
    // the partition it was ORIGINALLY acquired from (never the reclaiming thread's own lane — the
    // reclaiming worker is very likely not the thread that acquired this cell).
    void reclaim(Descriptor* d) noexcept {
        Cell* c = cell_of(d);
        if (c->destroy != nullptr && d->payload != nullptr) c->destroy(d->payload);
        c->destroy = nullptr;
        Partition* p = c->home;  // stamped once at grow_one, immutable after
        std::lock_guard<std::mutex> g(p->mu);
        d->release();  // bump generation, reset to Queued (003 §Cancellation)
        d->link.next.store(p->free_head, std::memory_order_relaxed);
        p->free_head = d;
    }

    static void reclaim_thunk(Descriptor* d, void* self) noexcept {
        static_cast<MessagePool*>(self)->reclaim(d);
    }
    // The Activation reclamation seam (002/003): wire this into the Activation so completed /
    // tombstoned / torn-down messages return their cell here.
    [[nodiscard]] ReclaimSink sink() noexcept { return ReclaimSink{&reclaim_thunk, this}; }

private:
    struct Partition;  // fwd — Cell::home points to it

    struct alignas(kPayloadAlign) Cell {
        Descriptor desc;                        // offset 0 — Descriptor* == Cell*
        void (*destroy)(void*) noexcept;         // payload destructor thunk
        Partition* home = nullptr;               // which partition to reclaim() back into
        alignas(kPayloadAlign) unsigned char payload[kMaxPayload];
    };
    static_assert(offsetof(Cell, desc) == 0, "Descriptor must be the first member (offset-0 cast)");
    static_assert(std::is_standard_layout_v<Cell>);

    // One independent shard of pool state. Cache-line aligned so two partitions' mutexes never
    // false-share. Each partition owns its OWN cell storage — no cross-partition shared mutable
    // state exists, so growth never needs a second, pool-wide lock.
    struct QUARK_CACHE_ALIGNED Partition {
        std::mutex mu;
        Descriptor* free_head = nullptr;
        std::vector<std::unique_ptr<Cell>> cells;
    };

    // Descriptor is the first member (offset 0), so a Descriptor* IS a Cell*. Route through void*
    // (not reinterpret_cast) so the offset-0 identity carries no -Wcast-align noise.
    static Cell* cell_of(Descriptor* d) noexcept {
        return static_cast<Cell*>(static_cast<void*>(d));
    }

    void grow_one(Partition& p) {  // caller holds p.mu (or is the ctor, single-threaded)
        auto c = std::make_unique<Cell>();
        c->destroy = nullptr;
        c->home = &p;
        Descriptor* d = &c->desc;
        d->link.next.store(p.free_head, std::memory_order_relaxed);
        p.free_head = d;
        p.cells.push_back(std::move(c));
    }

    std::size_t num_partitions_;
    std::unique_ptr<Partition[]> partitions_;
};

}  // namespace quark::detail
