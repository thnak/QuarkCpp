// Implements 020-Security §3 + ADR-044 (Flag-Gated Envelope Pool) — the second, wire-scale pool a
// non-anonymous-principal Descriptor is sourced from. Mirrors detail::MessagePool::Cell's proven
// offset-0 pattern (message_pool.hpp, ADR-037) EXACTLY so envelope_of()'s reinterpret_cast is
// well-founded: Descriptor first, tail data at a compiler-computed fixed offset, one mutex (no
// ADR-037 magazine — inbound frame rate is bounded by network/connection count, not core-to-core
// message-passing throughput; ADR-044's F2r/residual-risk-2 scope).
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include "quark/core/descriptor.hpp"
#include "quark/detail/envelope.hpp"

namespace quark::detail {

class EnvelopePool {
public:
    static constexpr std::size_t kMaxPayload = 192;   // matches MessagePool::kMaxPayload
    static constexpr std::size_t kPayloadAlign = 16;  // matches MessagePool::kPayloadAlign

    struct Slot {
        Descriptor* desc = nullptr;
        void* payload = nullptr;
    };

    // PUBLIC (unlike MessagePool::Cell, which is private): envelope_of() below needs to name this
    // type from outside the class to reinterpret_cast a flagged Descriptor* into it. Still never
    // constructed/touched directly by any caller outside acquire()/reclaim()/envelope_of().
    struct alignas(kPayloadAlign) Cell {
        Descriptor desc;  // offset 0 — Descriptor* == Cell* (mirrors MessagePool::Cell)
        DescriptorEnvelope env;
        void (*destroy)(void*) noexcept = nullptr;
        Cell* next_free = nullptr;
        alignas(kPayloadAlign) unsigned char payload[kMaxPayload];
    };
    static_assert(offsetof(Cell, desc) == 0, "Descriptor must be the first member (offset-0 cast)");
    static_assert(std::is_standard_layout_v<Cell>);

    explicit EnvelopePool(std::size_t capacity) {
        for (std::size_t i = 0; i < capacity; ++i) grow_one_locked();
    }
    EnvelopePool(const EnvelopePool&) = delete;
    EnvelopePool& operator=(const EnvelopePool&) = delete;

    // Cold-ish (bounded by wire/connection rate, not intra-process fan-out): one mutex, no magazine.
    [[nodiscard]] Slot acquire(void (*destroy)(void*) noexcept, const Principal& principal) noexcept {
        std::lock_guard<std::mutex> g(mu_);
        if (free_head_ == nullptr) grow_one_locked();
        Cell* c = free_head_;
        free_head_ = c->next_free;
        c->destroy = destroy;
        c->env.principal = principal;  // the ONLY principal-carrying write, ever
        Descriptor* d = &c->desc;
        d->link.next.store(nullptr, std::memory_order_relaxed);
        const std::uint16_t f = GenState::flags_of(d->gen_state.load(std::memory_order_relaxed));
        d->set_flags(f | kControlFlagHasEnvelope);  // single-writer, pre-publish (003 contract)
        return Slot{d, c->payload};
    }

    void reclaim(Descriptor* d) noexcept {
        Cell* c = cell_of(d);
        if (c->destroy != nullptr && d->payload != nullptr) c->destroy(d->payload);
        c->destroy = nullptr;
        c->env.principal = Principal{};  // wipe before returning to free list
        d->release();                    // bump generation, reset to Queued (003 §Cancellation)
        std::lock_guard<std::mutex> g(mu_);
        c->next_free = free_head_;
        free_head_ = c;
    }

    // Raw thunk matching ReclaimSink::fn's signature (`void(*)(Descriptor*, void*) noexcept`) WITHOUT
    // naming ReclaimSink itself here — ReclaimSink is declared in activation.hpp, which (transitively,
    // via engine.hpp) needs to include THIS header for envelope_of(); naming ReclaimSink here would
    // be circular. The caller (actor_ref.hpp's LocalRouter, where ReclaimSink IS visible) wraps this
    // thunk into a real ReclaimSink: `ReclaimSink{&EnvelopePool::reclaim_thunk, envelope_pool_ptr}`.
    static void reclaim_thunk(Descriptor* d, void* self) noexcept {
        static_cast<EnvelopePool*>(self)->reclaim(d);
    }

    // Test/prove seam: how many cells currently exist (grown so far).
    [[nodiscard]] std::size_t cells_grown() const noexcept { return cells_.size(); }

private:
    static Cell* cell_of(Descriptor* d) noexcept { return static_cast<Cell*>(static_cast<void*>(d)); }

    void grow_one_locked() {  // caller holds mu_ (or is the ctor, single-threaded)
        auto c = std::make_unique<Cell>();
        c->next_free = free_head_;
        free_head_ = c.get();
        cells_.push_back(std::move(c));
    }

    std::mutex mu_;
    Cell* free_head_ = nullptr;
    std::vector<std::unique_ptr<Cell>> cells_;
};

// Reached from a flagged Descriptor* with zero stored pointer (pure offset arithmetic via the
// enclosing-struct cast — mirrors MessagePool::cell_of()). PRECONDITION: caller already observed
// kControlFlagHasEnvelope set (from the SAME flags word try_claim()/set_flags() already touched —
// zero extra load to check the precondition) — see ADR-044's S2r per-site guard requirement.
[[nodiscard]] inline DescriptorEnvelope& envelope_of(Descriptor* d) noexcept {
    return reinterpret_cast<EnvelopePool::Cell*>(d)->env;
}

}  // namespace quark::detail
