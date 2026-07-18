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
// about the sequential drain, 023), so a producer-side lock is in-budget. SCOPE (006): one pool for
// local delivery; per-shard single-writer pools + per-producer caches are the 003/022 seam.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "quark/core/activation.hpp"  // ReclaimSink
#include "quark/core/descriptor.hpp"

namespace quark::detail {

// Type-erased payload destructor. `void(*)(void*) noexcept` matches the pool's stored thunk.
template <class Msg>
void destroy_payload(void* p) noexcept {
    static_cast<Msg*>(p)->~Msg();
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

    explicit MessagePool(std::size_t capacity) {
        cells_.reserve(capacity);
        for (std::size_t i = 0; i < capacity; ++i) grow_one();
    }
    MessagePool(const MessagePool&) = delete;
    MessagePool& operator=(const MessagePool&) = delete;

    // Hot path (send lane): pop a cell, arm its payload destructor, hand back the descriptor +
    // inline payload storage. Grows cold if the pool is exhausted (pre-size to avoid it).
    [[nodiscard]] Slot acquire(void (*destroy)(void*) noexcept) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        if (free_head_ == nullptr) grow_one();  // cold — should not happen on a pre-sized pool
        Descriptor* d = free_head_;
        free_head_ = d->link.next.load(std::memory_order_relaxed);
        Cell* c = cell_of(d);
        c->destroy = destroy;
        d->link.next.store(nullptr, std::memory_order_relaxed);
        return Slot{d, c->payload};
    }

    // Reclaim (drain lane): run the payload destructor (which, for an unanswered ask, fails the
    // ReplyCell — reply-before-teardown), bump the descriptor generation, and recycle the cell.
    void reclaim(Descriptor* d) noexcept {
        Cell* c = cell_of(d);
        if (c->destroy != nullptr && d->payload != nullptr) c->destroy(d->payload);
        c->destroy = nullptr;
        std::lock_guard<std::mutex> g(mu_);
        d->release();  // bump generation, reset to Queued (003 §Cancellation)
        d->link.next.store(free_head_, std::memory_order_relaxed);
        free_head_ = d;
    }

    static void reclaim_thunk(Descriptor* d, void* self) noexcept {
        static_cast<MessagePool*>(self)->reclaim(d);
    }
    // The Activation reclamation seam (002/003): wire this into the Activation so completed /
    // tombstoned / torn-down messages return their cell here.
    [[nodiscard]] ReclaimSink sink() noexcept { return ReclaimSink{&reclaim_thunk, this}; }

private:
    struct alignas(kPayloadAlign) Cell {
        Descriptor desc;                        // offset 0 — Descriptor* == Cell*
        void (*destroy)(void*) noexcept;         // payload destructor thunk
        alignas(kPayloadAlign) unsigned char payload[kMaxPayload];
    };
    static_assert(offsetof(Cell, desc) == 0, "Descriptor must be the first member (offset-0 cast)");
    static_assert(std::is_standard_layout_v<Cell>);

    // Descriptor is the first member (offset 0), so a Descriptor* IS a Cell*. Route through void*
    // (not reinterpret_cast) so the offset-0 identity carries no -Wcast-align noise.
    static Cell* cell_of(Descriptor* d) noexcept {
        return static_cast<Cell*>(static_cast<void*>(d));
    }

    void grow_one() {  // caller holds mu_ (or is the ctor, single-threaded)
        auto c = std::make_unique<Cell>();
        c->destroy = nullptr;
        Descriptor* d = &c->desc;
        d->link.next.store(free_head_, std::memory_order_relaxed);
        free_head_ = d;
        cells_.push_back(std::move(c));
    }

    std::mutex mu_;
    std::vector<std::unique_ptr<Cell>> cells_;
    Descriptor* free_head_ = nullptr;
};

}  // namespace quark::detail
