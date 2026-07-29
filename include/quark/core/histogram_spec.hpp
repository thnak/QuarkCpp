// Implements 009-Observability §"Histogram bucket layout and cardinality" (ADR-022, proven, Design
// 3 "revised") — per-metric configurable bucket layout via a compile-time `HistogramSpec`, with a
// CLOSED-FORM bucket lookup (`bucket_index_of`, the `{shift, step_shift}` technique) in place of the
// linear `while (v > b[i]) ++i;` scan the ADR flagged as the residual cost to fix before shipping.
//
// This is DISTINCT from (and does not touch) the shipped fixed-64-bucket `Histogram` in metrics.hpp,
// which stays exactly as-is, including its documented ARM64 weak-memory caveat (009 explicitly says
// "not re-litigated here" — see hot_cell.hpp's caveat; this header inherits it unchanged for the same
// reason: a plain-store, non-RMW single-writer pattern).
//
// LAYOUT: bucket_count total physical buckets = `sub = 1 << step_shift` LINEAR head buckets (bucket i
// covers [i*2^shift, (i+1)*2^shift - 1]) followed by successive power-of-two octaves, each split into
// `sub` equal linear sub-buckets. `bucket_index_of` computes the index in O(1) (one comparison, one
// countl_zero, one shift, one divide-by-power-of-two) — proven bit-for-bit equal to a brute-force
// linear scan over `boundaries()` across a wide value sweep for multiple distinct Specs (test:
// metrics_histogram_spec_test.cpp).
//
// MEMORY-ORDER CONTRACT (009, normative, ADR-022): `HistogramBlock` cells are PLAIN (non-atomic)
// integers on the single-writer side (a shard's drain-owner). The scraper reads them via
// `std::atomic_ref` at relaxed order and aggregates on read — the only cross-thread interaction, off
// the hot path. This depends on 002's drain-owner handoff establishing happens-before across a
// work-steal migration; `metrics_migration_stress_test.cpp` is the permanent TSan regression guard
// for that dependency (ADR-022 §S1/§Spec recommendation 3).
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "quark/core/config.hpp"

namespace quark {

// Validation-time cap on a per-metric HistogramSpec's `bucket_count` (mirrors the ADR-022 per-type
// grid design's `kHistogramCap`): bounds per-record cost and per-cell memory so a careless Spec can't
// regress either. Enforced by a `static_assert` inside `HistogramBlock<Spec>` below (008 §Validation
// — the compile-time subset), not a runtime check, since `Spec` is always a compile-time type.
inline constexpr std::size_t kHistogramSpecBucketCap = 64;

// A HistogramSpec declares the physical bucket count and the closed-form layout parameters. Two
// worked examples ship below (`LatencyNsSpec`, `MailboxDepthSpec`); an author writes their own by
// copying the shape. `boundaries()` is the metric's human-facing upper-bound array (Prometheus `le=`
// labels, brute-force test oracle) — generated FROM the same {shift, step_shift} parameters that
// `bucket_index_of` uses, so the two independently-computed code paths (O(bucket_count) scan vs O(1)
// closed form) must agree by construction, and can be proven to.
template <class S>
concept HistogramSpecConcept = requires {
    { S::bucket_count } -> std::convertible_to<std::size_t>;
    { S::shift } -> std::convertible_to<unsigned>;
    { S::step_shift } -> std::convertible_to<unsigned>;
};

// --- Closed-form bucket index (ADR-022 §"Replace the linear boundary scan") ---------------------
// v < (sub << Shift): LINEAR head region, bucket = v >> Shift (sub buckets total).
// v >= that: log-linear tail — floor(log2(v)) picks the octave, then a shift+mask into `sub` equal
// linear sub-buckets within that octave. Monotonic in v; clamps to the last physical bucket.
template <unsigned Shift, unsigned StepShift, std::size_t BucketCount>
[[nodiscard]] constexpr std::size_t bucket_index_of(std::uint64_t v) noexcept {
    static_assert(BucketCount >= 1, "bucket_count must be >= 1 (ADR-022 C1-fix: underflow at 0)");
    constexpr std::size_t sub = std::size_t{1} << StepShift;  // linear buckets per octave / head width
    constexpr unsigned k0 = Shift + StepShift;                // octave where the head region ends
    const std::uint64_t head_limit = sub << Shift;            // exclusive upper bound of the head region
    if (v < head_limit) {
        const std::size_t b = static_cast<std::size_t>(v >> Shift);
        return b < BucketCount ? b : BucketCount - 1;
    }
    // v >= head_limit >= 1 here, so v != 0 and countl_zero(v) is well-defined.
    const unsigned octave = 63u - static_cast<unsigned>(std::countl_zero(v));  // floor(log2(v))
    const unsigned oct_rel = octave - k0;                                     // octaves past the head
    const std::uint64_t width = std::uint64_t{1} << (octave - StepShift);     // this octave's sub-width
    const std::size_t sub_index =
        static_cast<std::size_t>((v - (std::uint64_t{1} << octave)) / width);
    const std::size_t b = sub + static_cast<std::size_t>(oct_rel) * sub + sub_index;
    return b < BucketCount ? b : BucketCount - 1;
}

// The upper-bound (inclusive) of each physical bucket, generated from the SAME {Shift, StepShift}
// parameters `bucket_index_of` uses — the brute-force oracle for the closed-form lookup, and the
// source of Prometheus `le=` labels. Cold / constexpr-evaluable; never called on the hot path.
template <unsigned Shift, unsigned StepShift, std::size_t BucketCount>
[[nodiscard]] constexpr std::array<std::uint64_t, BucketCount> make_boundaries() noexcept {
    static_assert(BucketCount >= 1, "bucket_count must be >= 1");
    std::array<std::uint64_t, BucketCount> b{};
    constexpr std::size_t sub = std::size_t{1} << StepShift;
    constexpr unsigned k0 = Shift + StepShift;
    for (std::size_t i = 0; i < BucketCount; ++i) {
        if (i < sub) {
            b[i] = ((static_cast<std::uint64_t>(i) + 1) << Shift) - 1;
        } else {
            const std::size_t rel = i - sub;
            const unsigned octave = k0 + static_cast<unsigned>(rel / sub);
            const std::size_t sub_index = rel % sub;
            const std::uint64_t width = std::uint64_t{1} << (octave - StepShift);
            const std::uint64_t start = (std::uint64_t{1} << octave) + sub_index * width;
            b[i] = start + width - 1;
        }
    }
    b[BucketCount - 1] = ~std::uint64_t{0};  // last physical bucket catches everything above
    return b;
}

// Two worked example Specs (used by the default per-type/per-instance metrics grid and by the
// closed-form-vs-brute-force test — deliberately DIFFERENT {shift, step_shift} shapes).
struct LatencyNsSpec {
    static constexpr std::size_t bucket_count = 48;
    static constexpr unsigned shift = 4;       // finest linear resolution: 16ns
    static constexpr unsigned step_shift = 4;  // 16 linear sub-buckets per octave
    [[nodiscard]] static constexpr auto boundaries() noexcept {
        return make_boundaries<shift, step_shift, bucket_count>();
    }
};
struct MailboxDepthSpec {
    static constexpr std::size_t bucket_count = 32;
    static constexpr unsigned shift = 0;       // finest linear resolution: 1 (depth is small-integer)
    static constexpr unsigned step_shift = 3;  // 8 linear sub-buckets per octave
    [[nodiscard]] static constexpr auto boundaries() noexcept {
        return make_boundaries<shift, step_shift, bucket_count>();
    }
};

// --- HistogramBlockSnapshot: plain aggregate, mergeable (aggregate-on-scrape, off hot path) -------
template <std::size_t N>
struct HistogramBlockSnapshot {
    std::array<std::uint64_t, N> buckets{};
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t min = 0;
    std::uint64_t max = 0;

    void merge(const HistogramBlockSnapshot& o) noexcept {
        for (std::size_t i = 0; i < N; ++i) buckets[i] += o.buckets[i];
        if (o.count != 0) {
            if (count == 0 || o.min < min) min = o.min;
            if (o.max > max) max = o.max;
        }
        count += o.count;
        sum += o.sum;
    }
};

// --- HistogramBlock<Spec>: the ADR-022 "revised" cell — plain (non-atomic) writer side, atomic_ref
// scraper side (see file banner for the full memory-order contract). One physical instance backs
// EITHER a per-type grid cell or a per-instance arena slot — cardinality never changes this type.
template <HistogramSpecConcept Spec>
struct HistogramBlock {
    static constexpr std::size_t kBuckets = Spec::bucket_count;
    static_assert(kBuckets >= 1 && kBuckets <= kHistogramSpecBucketCap,
                  "HistogramSpec::bucket_count must be in [1, kHistogramSpecBucketCap] "
                  "(ADR-022 Validation-time cap on per-metric bucket count)");

    std::array<std::uint64_t, kBuckets> buckets{};  // PLAIN — single shard drain-owner writes these
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t min = ~std::uint64_t{0};
    std::uint64_t max = 0;

    // Hot path: plain increments, NO atomics, NO RMW (009's ADR-022-revised normative contract).
    QUARK_ALWAYS_INLINE void record(std::uint64_t v) noexcept {
        const std::size_t b = bucket_index_of<Spec::shift, Spec::step_shift, kBuckets>(v);
        ++buckets[b];
        ++count;
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }

    // Off-hot-path reset (ADR-022 C4: "recycled slot never inherits stale data"). Only ever called by
    // the owning single-writer shard thread (a freelist push/pop is drain-owner-only, S1), so a plain
    // (non-atomic) reset is consistent with the write-side contract above.
    void reset() noexcept {
        buckets.fill(0);
        count = 0;
        sum = 0;
        min = ~std::uint64_t{0};
        max = 0;
    }

    // Scraper-side snapshot: std::atomic_ref at relaxed order — the ONLY cross-thread interaction
    // with this block, off the hot path (009 normative memory-order contract).
    [[nodiscard]] HistogramBlockSnapshot<kBuckets> snapshot() noexcept {
        HistogramBlockSnapshot<kBuckets> s;
        for (std::size_t i = 0; i < kBuckets; ++i)
            s.buckets[i] = std::atomic_ref<std::uint64_t>(buckets[i]).load(std::memory_order_relaxed);
        s.count = std::atomic_ref<std::uint64_t>(count).load(std::memory_order_relaxed);
        s.sum = std::atomic_ref<std::uint64_t>(sum).load(std::memory_order_relaxed);
        const std::uint64_t mn = std::atomic_ref<std::uint64_t>(min).load(std::memory_order_relaxed);
        s.min = (s.count == 0) ? 0 : mn;
        s.max = std::atomic_ref<std::uint64_t>(max).load(std::memory_order_relaxed);
        return s;
    }
};

}  // namespace quark
