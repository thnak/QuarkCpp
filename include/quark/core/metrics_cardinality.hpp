// Implements 009-Observability §"Histogram bucket layout and cardinality" (ADR-022, proven, Design
// 3 "Per-metric buckets with opt-in per-instance cardinality") — the TWO-LEVEL cardinality mechanism:
//
//   * PER-ACTOR-TYPE always exists: one `HistogramBlock<Spec>` per (shard, registered type), dense
//     `type_index`-addressed, sized ONCE at build-time registration (`TypeMetricsGrid<Spec>`), never
//     reallocated.
//   * PER-ACTOR-INSTANCE is strictly OPT-IN via the `PerInstanceMetrics<N>` policy tag, backed by one
//     engine-wide, build-time-sized `InstanceMetricsArena<Spec>` — a flat array partitioned by
//     prefix-sum offsets per opted-in type, with a per-(shard, type) fixed-capacity freelist handing
//     out instance slots ONCE at activation creation (`bind_activation`), never re-resolved per
//     message. Per-instance cardinality never changes bucket semantics: it multiplies the SAME
//     `Spec::boundaries()` into more physical `HistogramBlock<Spec>` instances.
//
// FAILURE MODE (009, observable not silent): freelist exhaustion degrades the overflowing activation
// to per-type-only recording — `bind_activation` returns an invalid handle, never allocates, never
// blocks — and increments a dedicated `instance_slot_exhausted` counter (§`exhausted_count`).
//
// OWNERSHIP / CONCURRENCY: a `TypeMetricsGrid` cell at (shard, type) is written ONLY by that shard's
// drain-owner (002 single-writer-per-shard invariant) — the same invariant `ShardCounters` relies on
// in metrics.hpp. An `InstanceMetricsArena` freelist for (shard, type) is likewise touched ONLY by
// that shard's drain-owner (activation creation/destruction is a drain-lane event), so freelist
// push/pop and the label side-table update need no atomics of their own (ADR-022 §S1) — the ONLY
// cross-thread access to any of this is a scraper's `HistogramBlock::snapshot()` (atomic_ref, see
// histogram_spec.hpp).
//
// BUILD-TIME BUDGET GATE (009 §"Build-time budget gate"): `metrics_grid_footprint_bytes` /
// `metrics_arena_footprint_bytes` compute the EXACT worst-case footprint via `sizeof()` — the ADR-022
// debate found hand arithmetic undercounted this by 3.5x in an early draft, so this header never adds
// per-field byte counts by hand. `validate_metrics_budget` rejects with `errc::validation` ("Metrics
// BudgetExceeded", following error.hpp's existing "do not invent ad-hoc error enums per module" rule)
// before any allocation is attempted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/error.hpp"
#include "quark/core/histogram_spec.hpp"
#include "quark/core/ids.hpp"

namespace quark {

// --- PerInstanceMetrics<N> (ADR-022 / 009-Observability) ------------------------------------------
// Opt-in policy tag: an actor type listing `PerInstanceMetrics<N>` in its `Actor<D, Policies...>`
// pack asks for up to N live per-instance metric slots PER SHARD, backed by the engine-wide
// `InstanceMetricsArena`. Absent ⇒ the actor gets ONLY the always-on per-type block (the default).
// N is a per-(shard, type) freelist capacity, not a global cap (see `InstanceMetricsArena`).
//
// Pattern-matched by template-id in policies.hpp (`as_per_instance_metrics`) via a forward
// declaration — the SAME technique already used there for `DrainBudget<N>` (engine.hpp) and
// `Persistent<Model, Mode>` (persistence.hpp) — so policies.hpp (a light, frequently-included header)
// never has to include this heavier one. `PerInstanceMetrics<N>` + `Stateless<N>` is a compile-time
// `static_assert` failure enforced in policies.hpp: pool activations have no stable per-key identity
// to pin an instance slot to (009 §"Per-instance failure modes").
template <std::size_t N>
struct PerInstanceMetrics {
    static constexpr std::size_t value = N;
};

// ====================================================================================================
// TypeMetricsGrid<Spec> — the always-on per-(shard, type_index) block. Sized ONCE at construction
// (the "build-time registration" moment); no API to grow it afterward, so the base pointer is stable
// for the grid's whole lifetime (mirrors the per-type-grid design's proven S2: "array sized to live
// count, never reallocated").
// ====================================================================================================
template <HistogramSpecConcept Spec>
class TypeMetricsGrid {
public:
    TypeMetricsGrid(std::size_t shard_count, std::size_t type_count)
        : shard_count_(shard_count),
          type_count_(type_count),
          cells_(std::make_unique<Cell[]>(shard_count * type_count == 0 ? 1 : shard_count * type_count)) {}

    TypeMetricsGrid(const TypeMetricsGrid&) = delete;
    TypeMetricsGrid& operator=(const TypeMetricsGrid&) = delete;

    // The (shard, type_index) cell. Only `shard`'s drain-owner may call `record()` on the result.
    [[nodiscard]] HistogramBlock<Spec>& cell(std::size_t shard, std::size_t type_index) noexcept {
        return cells_[shard * type_count_ + type_index].block;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept { return shard_count_; }
    [[nodiscard]] std::size_t type_count() const noexcept { return type_count_; }

    // Aggregate-on-scrape across every shard for one type (off hot path; scraper-only).
    [[nodiscard]] HistogramBlockSnapshot<Spec::bucket_count> snapshot_for_type(
        std::size_t type_index) noexcept {
        HistogramBlockSnapshot<Spec::bucket_count> s;
        for (std::size_t sh = 0; sh < shard_count_; ++sh) s.merge(cell(sh, type_index).snapshot());
        return s;
    }

    // Exact worst-case footprint via sizeof() — the build-time budget gate input (009 §"Build-time
    // budget gate"). Deliberately NOT hand arithmetic (ADR-022 found a real 3.5x undercount that way).
    [[nodiscard]] static constexpr std::size_t footprint_bytes(std::size_t shard_count,
                                                                 std::size_t type_count) noexcept {
        return sizeof(Cell) * shard_count * type_count;
    }

private:
    // Cache-line padded so two adjacent (shard,type) cells never share a line under concurrent
    // drain-owner writes on different shards (023 false-sharing avoidance, mirrors ShardCounters).
    struct QUARK_CACHE_ALIGNED Cell {
        HistogramBlock<Spec> block;
    };
    std::size_t shard_count_;
    std::size_t type_count_;
    std::unique_ptr<Cell[]> cells_;
};

// ====================================================================================================
// InstanceMetricsArena<Spec> — the opt-in, engine-wide, build-time-sized critical arena. Holds ONE
// flat array of `HistogramBlock<Spec>`, partitioned by prefix-sum offsets per opted-in type, plus a
// per-(shard, type) fixed-capacity freelist and an instance-slot -> ActorId label side-table.
// ====================================================================================================

// A resolved instance slot, cached by the caller (the activation) at creation and reused for every
// subsequent `record()` — never re-resolved per message (009 §F3 "slot resolution amortizes to O(1)
// per activation lifetime"). `valid == false` means "opted in but the freelist for this (shard, type)
// was exhausted at bind time" OR "this type never opted in" — either way, degrade to per-type-only.
template <HistogramSpecConcept Spec>
struct InstanceSlotHandle {
    HistogramBlock<Spec>* block = nullptr;
    std::uint32_t shard = 0;
    std::uint32_t type_index = 0;
    std::uint32_t slot = 0;
    bool valid = false;
};

template <HistogramSpecConcept Spec>
class InstanceMetricsArena {
public:
    // One opted-in type's reservation: `type_index` (dense, engine-assigned) and `capacity_per_shard`
    // (the declared `PerInstanceMetrics<N>`'s N — a PER-(shard,type) freelist size, not a global cap).
    struct TypeReservation {
        std::size_t type_index;
        std::size_t capacity_per_shard;
    };

    // Build-time-sized construction (cold; the "Engine::build()-equivalent construction path"). Types
    // NOT listed here never opted in and consume ZERO arena bytes (009's opt-in-is-free contract).
    InstanceMetricsArena(std::size_t shard_count, std::vector<TypeReservation> types)
        : shard_count_(shard_count), types_(std::move(types)) {
        offsets_.reserve(types_.size());
        std::size_t running = 0;
        for (const auto& t : types_) {
            offsets_.push_back(running);
            running += t.capacity_per_shard * shard_count_;
        }
        total_slots_ = running;
        blocks_ = std::make_unique<HistogramBlock<Spec>[]>(total_slots_ == 0 ? 1 : total_slots_);

        freelists_.resize(types_.size());
        labels_.resize(types_.size());
        exhausted_.resize(types_.size());
        for (std::size_t ti = 0; ti < types_.size(); ++ti) {
            const std::size_t cap = types_[ti].capacity_per_shard;
            type_to_local_.emplace(types_[ti].type_index, ti);
            freelists_[ti].reserve(shard_count_);
            for (std::size_t sh = 0; sh < shard_count_; ++sh) freelists_[ti].emplace_back(cap);
            labels_[ti].assign(shard_count_ * cap, ActorId{});
            exhausted_[ti].assign(shard_count_, 0);
        }
    }

    InstanceMetricsArena(const InstanceMetricsArena&) = delete;
    InstanceMetricsArena& operator=(const InstanceMetricsArena&) = delete;

    // Resolve (once, at activation creation) a per-instance slot for `id` of `type_index` on `shard`.
    // ONLY the owning shard's drain-owner calls this (activation creation is a drain-lane event, like
    // `bind_activation`/`unbind_activation` below) — no atomics needed here (ADR-022 §S1).
    //   * type never opted in            -> {valid=false} (per-type-only, always; not a failure)
    //   * freelist exhausted for (sh,ty) -> {valid=false}, `instance_slot_exhausted` incremented
    //   * otherwise                      -> a fresh (reset) slot, label side-table updated in-step
    [[nodiscard]] InstanceSlotHandle<Spec> bind_activation(std::size_t shard, std::size_t type_index,
                                                             ActorId id) noexcept {
        const auto it = type_to_local_.find(type_index);
        if (it == type_to_local_.end()) return {};  // not opted in: per-type-only, not a failure
        const std::size_t ti = it->second;
        Freelist& fl = freelists_[ti][shard];
        if (fl.empty()) {
            ++exhausted_[ti][shard];  // 009: observable, dedicated instance_slot_exhausted counter
            return {};                // degrade to per-type-only — never allocates, never blocks
        }
        const std::uint32_t local_slot = fl.pop();
        const std::size_t cap = types_[ti].capacity_per_shard;
        HistogramBlock<Spec>& blk = blocks_[offsets_[ti] + shard * cap + local_slot];
        blk.reset();  // first use of a freshly-claimed slot must start clean
        labels_[ti][shard * cap + local_slot] = id;  // side-table update, same step as the pop
        return InstanceSlotHandle<Spec>{&blk, static_cast<std::uint32_t>(shard),
                                          static_cast<std::uint32_t>(type_index), local_slot, true};
    }

    // Release a previously-bound slot (activation destruction). Idempotent / a no-op on an invalid
    // handle (an activation that never held a slot, e.g. degraded at bind time) — mirrors ADR-022's
    // S2r fix (saturating no-op, not a fatal double-release).
    void unbind_activation(const InstanceSlotHandle<Spec>& h) noexcept {
        if (!h.valid) return;
        const auto it = type_to_local_.find(h.type_index);
        if (it == type_to_local_.end()) return;
        const std::size_t ti = it->second;
        const std::size_t cap = types_[ti].capacity_per_shard;
        h.block->reset();  // C4: a recycled slot must never inherit stale data
        labels_[ti][h.shard * cap + h.slot] = ActorId{};  // clear label, same step as the freelist push
        freelists_[ti][h.shard].push(h.slot);
    }

    // The instance-slot -> ActorId label (009 §"Instance-slot -> ActorId label side-table"), for
    // scrape-time labeling of per-instance output. Off hot path.
    [[nodiscard]] ActorId label(std::size_t type_index, std::size_t shard, std::uint32_t slot) const {
        const auto it = type_to_local_.find(type_index);
        if (it == type_to_local_.end()) return {};
        const std::size_t ti = it->second;
        const std::size_t cap = types_[ti].capacity_per_shard;
        return labels_[ti][shard * cap + slot];
    }

    // Freelist-exhaustion counter for (type, shard) — 009's dedicated `instance_slot_exhausted`.
    [[nodiscard]] std::uint64_t exhausted_count(std::size_t type_index, std::size_t shard) const {
        const auto it = type_to_local_.find(type_index);
        if (it == type_to_local_.end()) return 0;
        return exhausted_[it->second][shard];
    }

    [[nodiscard]] std::size_t total_slots() const noexcept { return total_slots_; }
    [[nodiscard]] std::size_t capacity_of(std::size_t type_index) const noexcept {
        const auto it = type_to_local_.find(type_index);
        return it == type_to_local_.end() ? 0 : types_[it->second].capacity_per_shard;
    }

    // Exact worst-case footprint via sizeof() (see file banner: no hand arithmetic — ADR-022 found a
    // real 3.5x undercount doing it that way). Covers the histogram data AND the bookkeeping
    // (freelist storage, label side-table, exhaustion counters) so M1 ("bookkeeping bounded, small
    // vs. histogram data") is testable directly against this same number.
    [[nodiscard]] static std::size_t footprint_bytes(std::size_t shard_count,
                                                        const std::vector<TypeReservation>& types) {
        std::size_t slots = 0;
        for (const auto& t : types) slots += t.capacity_per_shard * shard_count;
        std::size_t bytes = sizeof(HistogramBlock<Spec>) * slots;
        for (const auto& t : types) {
            bytes += shard_count * t.capacity_per_shard * sizeof(std::uint32_t);  // freelist storage
            bytes += shard_count * t.capacity_per_shard * sizeof(ActorId);        // label side-table
            bytes += shard_count * sizeof(std::uint64_t);                        // exhaustion counters
        }
        return bytes;
    }

private:
    // Fixed-capacity intrusive stack of free local slot indices for one (shard, type). Touched ONLY
    // by that shard's drain-owner (ADR-022 §S1) — no atomics. All `capacity_per_shard` slots start
    // free.
    class Freelist {
    public:
        explicit Freelist(std::size_t capacity) : slots_(capacity) {
            for (std::size_t i = 0; i < capacity; ++i) slots_[i] = static_cast<std::uint32_t>(i);
            top_ = capacity;
        }
        [[nodiscard]] bool empty() const noexcept { return top_ == 0; }
        [[nodiscard]] std::uint32_t pop() noexcept { return slots_[--top_]; }
        void push(std::uint32_t s) noexcept { slots_[top_++] = s; }

    private:
        std::vector<std::uint32_t> slots_;
        std::size_t top_ = 0;
    };

    std::size_t shard_count_;
    std::vector<TypeReservation> types_;
    std::vector<std::size_t> offsets_;                        // prefix-sum, one per opted-in type
    std::unordered_map<std::size_t, std::size_t> type_to_local_;  // engine type_index -> local index
    std::unique_ptr<HistogramBlock<Spec>[]> blocks_;
    std::vector<std::vector<Freelist>> freelists_;   // [local type][shard]
    std::vector<std::vector<ActorId>> labels_;       // [local type][shard*cap + slot]
    std::vector<std::vector<std::uint64_t>> exhausted_;  // [local type][shard]
    std::size_t total_slots_ = 0;
};

// ====================================================================================================
// Build-time budget gate (009 §"Build-time budget gate"): rejects before any allocation is attempted
// if a computed footprint exceeds the configured `metrics_memory_budget_bytes` knob. Reuses the
// existing generic `errc::validation` code (error.hpp explicitly says "do not invent ad-hoc error
// enums per module") with a `"MetricsBudgetExceeded"`-tagged detail string, so callers/tests can match
// on the exact condition without a new enum member.
// ====================================================================================================
[[nodiscard]] inline result<void> validate_metrics_budget(
    std::size_t footprint_bytes, std::size_t metrics_memory_budget_bytes) noexcept {
    if (footprint_bytes > metrics_memory_budget_bytes) {
        return fail(errc::validation,
                     "MetricsBudgetExceeded: metrics grid/arena worst-case footprint exceeds "
                     "metrics_memory_budget_bytes (009 §Build-time budget gate, ADR-022)");
    }
    return {};
}

// Convenience combinator: the exact worst-case footprint of a type grid + an instance arena together
// (what a real `Engine::build()` integration would sum before allocating either).
template <HistogramSpecConcept Spec>
[[nodiscard]] std::size_t metrics_total_footprint_bytes(
    std::size_t shard_count, std::size_t type_count,
    const std::vector<typename InstanceMetricsArena<Spec>::TypeReservation>& instance_types) {
    return TypeMetricsGrid<Spec>::footprint_bytes(shard_count, type_count) +
           InstanceMetricsArena<Spec>::footprint_bytes(shard_count, instance_types);
}

}  // namespace quark
