// Implements 006 §Addressing — the ActorId -> Schedulable* registry behind `Engine::resolve()`;
// ADR-064 (perf: replaces std::unordered_map on the per-`tell` path).
//
// `std::unordered_map::find` costs, per call: a hash, a 64-bit `div` for the bucket index (~25-40
// cycles on x86), and a pointer chase through the bucket list. This is an open-addressing table with a
// power-of-two capacity: index = hash & mask, linear probing over contiguous 16-byte slots, load factor
// <= 1/2 (so a probe sequence is ~1.5 slots on average and almost always one cache line).
//
// Concurrency contract (identical to the map it replaces): MUTATED ONLY COLD, single-threaded, before
// `Engine::start()`; read concurrently by every producer thread afterwards. `find` is a pure read of
// immutable memory — no locks, no atomics. `insert_or_assign` after the table is shared is a data race
// (and may reallocate) exactly as it was for `unordered_map`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "quark/core/ids.hpp"

namespace quark::detail {

template <class V>
class FlatActorMap {
public:
    FlatActorMap() { slots_.resize(kMinCapacity); }

    // Cold. Overwrites an existing entry for `id` (operator[]= semantics).
    void insert_or_assign(ActorId id, V* value) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        Slot& s = probe(slots_, id);
        if (s.value == nullptr) ++size_;
        s.key = id;
        s.value = value;
    }

    // Hot. nullptr if absent. `value == nullptr` marks an empty slot, so a stored value must be non-null.
    [[nodiscard]] V* find(ActorId id) const noexcept {
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = id.hash() & mask;
        for (;;) {
            const Slot& s = slots_[i];
            if (s.value == nullptr) return nullptr;
            if (s.key == id) return s.value;
            i = (i + 1) & mask;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Slot {
        ActorId key{};
        V* value = nullptr;
    };
    static constexpr std::size_t kMinCapacity = 16;  // power of two

    // The slot holding `id`, or the first empty slot of its probe sequence. Capacity is a power of two
    // and the load factor stays <= 1/2, so an empty slot always exists and this terminates.
    static Slot& probe(std::vector<Slot>& slots, ActorId id) noexcept {
        const std::size_t mask = slots.size() - 1;
        std::size_t i = id.hash() & mask;
        while (slots[i].value != nullptr && !(slots[i].key == id)) i = (i + 1) & mask;
        return slots[i];
    }

    void grow() {
        std::vector<Slot> next(slots_.size() * 2);
        for (const Slot& s : slots_)
            if (s.value != nullptr) probe(next, s.key) = s;
        slots_ = std::move(next);
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

}  // namespace quark::detail
