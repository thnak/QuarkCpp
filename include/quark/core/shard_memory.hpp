// Implements 003-Memory §Allocators — the shard-owned std::pmr allocators for descriptors and
// payloads. Pre-allocated COLD; the hot path (acquire/release) does 0 heap allocation.
//
// Each shard owns its memory (003 §Shard ownership): a shard is drained by one worker at a time
// in the common case, so these pools are single-writer / lock-free by construction — they carry
// NO internal synchronization and must not be shared across shards.
#pragma once

#include <cstddef>
#include <memory_resource>
#include <new>

#include "quark/core/descriptor.hpp"

namespace quark {

// Fixed-size descriptor pool: a free-list over a block pre-allocated from a pmr upstream at
// construction (cold). acquire() is a pop, release() is a push — 0 heap allocation on the hot
// path (003 §Allocators). Single-writer: the owning shard's worker only.
class DescriptorPool {
public:
    explicit DescriptorPool(std::size_t capacity,
                            std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
        : upstream_(upstream), capacity_(capacity) {
        if (capacity_ == 0) return;
        void* raw = upstream_->allocate(capacity_ * sizeof(Descriptor), alignof(Descriptor));
        storage_ = static_cast<Descriptor*>(raw);
        // Construct every descriptor and thread the free-list through the intrusive link (unused
        // while pooled). Build the list so acquire() hands out ascending addresses first.
        for (std::size_t i = capacity_; i-- > 0;) {
            Descriptor* d = ::new (static_cast<void*>(storage_ + i)) Descriptor();
            d->link.next.store(free_head_, std::memory_order_relaxed);
            free_head_ = d;
        }
        available_ = capacity_;
    }

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;
    DescriptorPool(DescriptorPool&&) = delete;
    DescriptorPool& operator=(DescriptorPool&&) = delete;

    ~DescriptorPool() {
        if (storage_ == nullptr) return;
        for (std::size_t i = 0; i < capacity_; ++i) storage_[i].~Descriptor();
        upstream_->deallocate(storage_, capacity_ * sizeof(Descriptor), alignof(Descriptor));
    }

    // Hot path: pop a descriptor. Returns nullptr when the pool is exhausted (the unbounded queue
    // has no backpressure — admission/overload is 022's job, 003 §Backpressure).
    [[nodiscard]] Descriptor* acquire() noexcept {
        Descriptor* d = free_head_;
        if (d == nullptr) return nullptr;
        free_head_ = d->link.next.load(std::memory_order_relaxed);
        --available_;
        d->link.next.store(nullptr, std::memory_order_relaxed);
        return d;
    }

    // Hot path: return a descriptor to the pool. Bumps its generation (fences a late cancel
    // against reuse — 003 §Cancellation) and re-threads it onto the free-list.
    void release(Descriptor* d) noexcept {
        d->release();  // bump generation, reset state to Queued
        d->link.next.store(free_head_, std::memory_order_relaxed);
        free_head_ = d;
        ++available_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t available() const noexcept { return available_; }

private:
    std::pmr::memory_resource* upstream_;
    Descriptor* storage_ = nullptr;
    Descriptor* free_head_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t available_ = 0;
};

// Payload arena: short-lived message data lives here, INDEPENDENTLY of descriptors (003
// §Payload). Backed by a pmr monotonic buffer pre-sized cold; `null_memory_resource` upstream
// makes an over-run throw loudly instead of silently heap-allocating on the hot path. Bulk
// reset() is the quiescent-point optimization (003 Open questions / 015).
class PayloadArena {
public:
    explicit PayloadArena(std::size_t bytes)
        : buffer_(operator_new_cold(bytes)),
          bytes_(bytes),
          resource_(buffer_, bytes_, std::pmr::null_memory_resource()) {}

    PayloadArena(const PayloadArena&) = delete;
    PayloadArena& operator=(const PayloadArena&) = delete;
    PayloadArena(PayloadArena&&) = delete;
    PayloadArena& operator=(PayloadArena&&) = delete;

    ~PayloadArena() { ::operator delete(buffer_, bytes_); }

    // Hot path: bump-allocate `size` payload bytes from the pre-sized arena. Throws
    // std::bad_alloc (from the null upstream) if the arena is exhausted — a pre-sizing bug, never
    // a silent heap hit. Cold-path callers may catch; the hot path must size the arena correctly.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t)) {
        return resource_.allocate(size, align);
    }

    // Bulk-reclaim the whole arena — valid only at a quiescent point (in-flight == 0, 015).
    void reset() noexcept { resource_.release(); }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept { return &resource_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    static void* operator_new_cold(std::size_t bytes) { return ::operator new(bytes); }

    void* buffer_;
    std::size_t bytes_;
    std::pmr::monotonic_buffer_resource resource_;
};

}  // namespace quark
