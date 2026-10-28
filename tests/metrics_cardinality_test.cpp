// Tests 009-Observability §"Histogram bucket layout and cardinality" (ADR-022, Design 3) — the
// two-level cardinality mechanism: TypeMetricsGrid (always-on per-type), InstanceMetricsArena
// (opt-in per-instance, bounded critical-arena + freelist), the build-time budget gate, the
// PerInstanceMetrics<N> / Stateless<N> compile-time incompatibility, and the observable freelist-
// exhaustion failure mode.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/histogram_spec.hpp"
#include "quark/core/metrics_cardinality.hpp"
#include "quark/core/policies.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

using Spec = LatencyNsSpec;

// ---- PerInstanceMetrics<N> policy surface: zero-cost-when-unused + extraction -------------------
struct Ping {};
struct Plain : Actor<Plain, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
struct Opted : Actor<Opted, Sequential, PerInstanceMetrics<8>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
// The rejected combination — defined only to read its rule PREDICATE (never validated/registered),
// exactly the pattern placement_policy_validation_test.cpp uses for the 025 conflicts.
struct BadPooledOpted : Actor<BadPooledOpted, Stateless<4>, PerInstanceMetrics<8>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

static_assert(!has_per_instance_metrics_v<Plain>, "a plain actor does not opt into per-instance metrics");
static_assert(has_per_instance_metrics_v<Opted> && per_instance_metrics_capacity_of<Opted>() == 8,
              "PerInstanceMetrics<N> extraction");
static_assert(validate_actor_policies<Plain>());
static_assert(validate_actor_policies<Opted>());
// e) PerInstanceMetrics<N> + Stateless<N> is a DETECTED conflict (the predicate fires); a real
// compile-time static_assert failure (not just the predicate) is demonstrated separately in
// metrics_per_instance_stateless_negative_compile.cpp (deliberately excluded from the test build —
// its entire point is to NOT compile).
static_assert(per_instance_metrics_stateless_conflict<BadPooledOpted>(),
              "PerInstanceMetrics<N> + Stateless<N> is a detected conflict (009/ADR-022)");
static_assert(!per_instance_metrics_stateless_conflict<Opted>());
static_assert(!per_instance_metrics_stateless_conflict<Plain>());

}  // namespace

int main() {
    // ---- TypeMetricsGrid: always-on, dense (shard, type_index), sized once, never reallocated ----
    {
        constexpr std::size_t kShards = 4, kTypes = 8;
        TypeMetricsGrid<Spec> grid(kShards, kTypes);
        check(grid.shard_count() == kShards && grid.type_count() == kTypes, "grid dims");

        HistogramBlock<Spec>* p_before = &grid.cell(1, 3);
        for (int i = 0; i < 10'000; ++i) grid.cell(1, 3).record(static_cast<std::uint64_t>(i));
        HistogramBlock<Spec>* p_after = &grid.cell(1, 3);
        check(p_before == p_after, "S2: cell address stable — grid never reallocated");

        // Different (shard,type) cells stay isolated (C2: per-type isolation, zero cross-contamination).
        for (int i = 0; i < 500; ++i) grid.cell(0, 0).record(1);
        check(grid.cell(0, 0).snapshot().count == 500, "isolated cell count");
        check(grid.cell(1, 3).snapshot().count == 10'000, "other cell unaffected");

        // Aggregate-on-scrape across shards for one type.
        grid.cell(2, 5).record(7);
        grid.cell(3, 5).record(9);
        const auto agg = grid.snapshot_for_type(5);
        check(agg.count == 2 && agg.sum == 16, "snapshot_for_type aggregates across shards");
    }

    // ---- InstanceMetricsArena: opt-in, bounded, prefix-sum-partitioned, freelist-backed -----------
    {
        constexpr std::size_t kShards = 2;
        constexpr std::size_t kCapPerShard = 3;
        constexpr std::size_t kTypeIdx = 0;
        InstanceMetricsArena<Spec>::TypeReservation rsv{kTypeIdx, kCapPerShard};
        InstanceMetricsArena<Spec> arena(kShards, {rsv});
        check(arena.total_slots() == kShards * kCapPerShard, "arena sized shard_count*capacity");
        check(arena.capacity_of(kTypeIdx) == kCapPerShard, "capacity_of");
        check(arena.capacity_of(/*never opted in*/ 99) == 0, "non-opted-in type has zero capacity");

        // Bind exactly `capacity` activations on shard 0 — all succeed, none exhausted.
        std::vector<InstanceSlotHandle<Spec>> handles;
        for (std::uint64_t k = 0; k < kCapPerShard; ++k) {
            auto h = arena.bind_activation(0, kTypeIdx, ActorId{TypeKey{1}, k});
            check(h.valid, "bind succeeds within capacity");
            handles.push_back(h);
        }
        check(arena.exhausted_count(kTypeIdx, 0) == 0, "no exhaustion within capacity");

        // d) The (capacity+1)th and (capacity+2)th binds on the SAME (shard,type) are exhausted:
        // never allocate, never block, degrade to invalid (per-type-only) — and the dedicated
        // instance_slot_exhausted counter increments exactly once per overflow attempt.
        auto over1 = arena.bind_activation(0, kTypeIdx, ActorId{TypeKey{1}, 100});
        auto over2 = arena.bind_activation(0, kTypeIdx, ActorId{TypeKey{1}, 101});
        check(!over1.valid && !over2.valid, "freelist exhaustion degrades to invalid handle");
        check(arena.exhausted_count(kTypeIdx, 0) == 2, "instance_slot_exhausted increments exactly once per overflow");
        // Shard 1's freelist for the same type is UNTOUCHED by shard 0's exhaustion.
        check(arena.exhausted_count(kTypeIdx, 1) == 0, "exhaustion is per-(shard,type), not global");
        auto shard1_ok = arena.bind_activation(1, kTypeIdx, ActorId{TypeKey{1}, 200});
        check(shard1_ok.valid, "a different shard's freelist for the same type is independent");
        arena.unbind_activation(shard1_ok);

        // Label side-table reflects the bound ActorId, and clears on unbind.
        check(arena.label(kTypeIdx, 0, handles[0].slot) == ActorId{TypeKey{1}, 0}, "label set at bind");

        // C1: per-type >= sum(per-instance), equality iff no overflow. Record the SAME N messages
        // into (a) the always-on per-type grid cell and (b) each activation's instance block —
        // exactly the composition contract (per-instance never replaces per-type; it multiplies the
        // same Spec into more physical blocks).
        TypeMetricsGrid<Spec> grid(kShards, /*type_count*/ 1);
        constexpr std::uint64_t kMsgsPerActivation = 1000;
        // 5 "activations" total tried on shard 0 (3 got slots, 2 overflowed) — ALL 5 record into the
        // per-type cell; only the 3 with valid slots also record into their instance block.
        for (std::size_t a = 0; a < kCapPerShard; ++a) {
            for (std::uint64_t m = 0; m < kMsgsPerActivation; ++m) {
                grid.cell(0, 0).record(m);
                handles[a].block->record(m);  // C2: same Spec, same value stream, same buckets
            }
        }
        for (std::uint64_t m = 0; m < kMsgsPerActivation; ++m) grid.cell(0, 0).record(m);  // over1's traffic
        for (std::uint64_t m = 0; m < kMsgsPerActivation; ++m) grid.cell(0, 0).record(m);  // over2's traffic

        const auto type_snap = grid.snapshot_for_type(0);
        std::uint64_t inst_sum_count = 0;
        for (auto& h : handles) inst_sum_count += h.block->snapshot().count;
        check(type_snap.count == 5 * kMsgsPerActivation, "per-type sees ALL activations' traffic");
        check(inst_sum_count == kCapPerShard * kMsgsPerActivation,
              "per-instance sees only the bound (non-overflowed) activations");
        check(type_snap.count > inst_sum_count, "C1: per-type strictly > sum(instances) when overflow occurred");

        // C2: cardinality never changes bucket semantics — the per-type cell's bucket distribution
        // for handles[0]'s value stream, restricted to that stream, matches its own instance block's
        // distribution bucket-for-bucket (both used the identical Spec on the identical values).
        HistogramBlock<Spec> mirror;
        for (std::uint64_t m = 0; m < kMsgsPerActivation; ++m) mirror.record(m);
        const auto inst0 = handles[0].block->snapshot();
        const auto mirror_snap = mirror.snapshot();
        bool buckets_match = true;
        for (std::size_t i = 0; i < Spec::bucket_count; ++i)
            if (inst0.buckets[i] != mirror_snap.buckets[i]) buckets_match = false;
        check(buckets_match && inst0.count == mirror_snap.count && inst0.sum == mirror_snap.sum,
              "C2: per-instance block bucket-exact vs an independent block fed the identical stream");

        // --- No-overflow control: capacity == activation count -> per-type COUNT == sum(instances) ---
        // (equality iff no overflow — the other half of C1's claim).
        InstanceMetricsArena<Spec>::TypeReservation rsv2{0, /*capacity*/ 4};
        InstanceMetricsArena<Spec> arena2(1, {rsv2});
        TypeMetricsGrid<Spec> grid2(1, 1);
        std::vector<InstanceSlotHandle<Spec>> h2;
        for (std::uint64_t k = 0; k < 4; ++k) h2.push_back(arena2.bind_activation(0, 0, ActorId{TypeKey{2}, k}));
        for (auto& h : h2) check(h.valid, "no-overflow control: every bind succeeds");
        for (auto& h : h2)
            for (std::uint64_t m = 0; m < 250; ++m) {
                grid2.cell(0, 0).record(m);
                h.block->record(m);
            }
        std::uint64_t sum2 = 0;
        for (auto& h : h2) sum2 += h.block->snapshot().count;
        check(grid2.snapshot_for_type(0).count == sum2, "C1 equality: no overflow -> per-type == sum(instances)");

        // C4: recycled slot never inherits stale data. Unbind handles[0] (which just recorded 1000
        // nonzero values), then rebind on the same (shard,type) — the freelist hands back the SAME
        // local slot (LIFO stack, last freed = first reused), and it must start at count==0.
        const std::uint32_t recycled_slot = handles[0].slot;
        arena.unbind_activation(handles[0]);
        auto rebound = arena.bind_activation(0, kTypeIdx, ActorId{TypeKey{1}, 999});
        check(rebound.valid, "rebind after unbind succeeds");
        check(rebound.slot == recycled_slot, "LIFO freelist hands back the just-freed slot");
        const auto rebound_snap = rebound.block->snapshot();
        check(rebound_snap.count == 0 && rebound_snap.sum == 0,
              "C4: recycled slot starts clean (reset-on-release), no stale data (count=333 bug class)");

        // F3: slot resolution amortizes to O(1) per activation lifetime — bind ONCE, then hammer
        // record() through the CACHED handle a large number of times without re-resolving.
        auto amort = arena.bind_activation(1, kTypeIdx, ActorId{TypeKey{1}, 500});
        check(amort.valid, "amortization-test bind");
        constexpr std::uint64_t kHammer = 1'000'000;
        for (std::uint64_t m = 0; m < kHammer; ++m) amort.block->record(m % 4096);
        check(amort.block->snapshot().count == kHammer,
              "F3: one bind_activation call, 1,000,000 record() calls via the cached handle");
        arena.unbind_activation(amort);
        arena.unbind_activation(rebound);
    }

    // ---- Build-time budget gate (both-sides control): sizeof()-computed footprint, not hand math ---
    {
        constexpr std::size_t kShards = 8, kTypes = 64;
        InstanceMetricsArena<Spec>::TypeReservation big{0, 1024};
        const std::size_t footprint = metrics_total_footprint_bytes<Spec>(kShards, kTypes, {big});
        check(footprint > 0, "footprint is nonzero for a nontrivial grid+arena");

        // Just under: MUST fire (no allocation attempted in this branch — footprint computed, no
        // TypeMetricsGrid/InstanceMetricsArena constructed).
        const auto rejected = validate_metrics_budget(footprint, footprint - 1);
        check(!rejected.has_value(), "MetricsBudgetExceeded fires when budget < exact footprint");
        check(rejected.error().code == errc::validation, "rejection carries errc::validation");

        // Exactly at the footprint: MUST pass (both-sides control, same footprint value).
        const auto accepted = validate_metrics_budget(footprint, footprint);
        check(accepted.has_value(), "budget exactly at footprint is accepted (not exceeded)");

        // A deliberately oversized request against a small budget also fires, and a deliberately
        // undersized one (way under budget) does not — the sizeof()-based path, not hand arithmetic
        // (ADR-022 found a real 3.5x undercount doing it by hand).
        const std::size_t oversized = metrics_total_footprint_bytes<Spec>(64, 256, {{0, 65536}});
        check(!validate_metrics_budget(oversized, 1024 * 1024).has_value(),
              "an 11GiB-class request is correctly rejected against a 1MiB budget");
        check(validate_metrics_budget(1024, 1024ull * 1024 * 1024).has_value(),
              "a tiny request against a generous budget is accepted");
    }

    std::printf("metrics_cardinality_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
