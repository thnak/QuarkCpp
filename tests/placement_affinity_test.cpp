// Tests 025 §Part B Affinity / AntiAffinity (+ the 026 bin-cache BYPASS):
//   * Affinity<A>     — co-locates the actor on A's node (same instance key), cache/locality.
//   * AntiAffinity<A> — NEVER lands on A's node (fault isolation / spread), and computes EXACT
//     per-actor HRW, BYPASSING the VirtualBins cache (bins quantize placement and would collapse a
//     deliberate spread — 026 §"Interaction with 025"). We prove the bypass by showing the result is
//     the exact per-actor winner (independent of whether a bin cache is supplied), even for ids whose
//     bin-quantized owner IS the forbidden node.
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/placement_policies.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct Ping {};
// The affinity target and the affine/anti-affine client — distinct actor types (distinct TypeKey), so
// their HRW keys differ even for the same instance key; affinity co-locates by DESIGN, not by hash.
struct Ledger : Actor<Ledger, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
struct Cart : Actor<Cart, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 40'000;

    std::vector<NodeId> nodes;
    for (std::uint64_t i = 1; i <= 6; ++i) nodes.push_back(NodeId{i * 97 + 3});
    const CapabilityView view = make_capability_view(nodes, /*epoch*/ 1, {});  // no caps needed

    // A 026 VirtualBins cache over the same roster — we pass it to the resolver to prove AntiAffinity
    // ignores it (exact per-actor HRW), and to find ids whose bin owner is the forbidden node.
    const VirtualBins bins(std::span<const NodeId>(nodes), virtual_bin_count(64));

    std::uint64_t aff_colocated = 0, anti_on_target = 0, anti_bin_disagree = 0, bin_eq_target = 0;
    std::uint64_t anti_bypass_mismatch = 0;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId cart_id = actor_id_of<Cart>(k);
        // The target node = where Ledger of the SAME instance key lands (exact per-actor HRW).
        const ActorId ledger_id = actor_id_of<Ledger>(k);
        const auto target = place_hash(ledger_id.hash(), std::span<const NodeId>(nodes));

        // Affinity: co-locate Cart onto Ledger's node.
        const auto aff = resolve_placement<Placement<HashById, Affinity<Ledger>>>(cart_id, view);
        check(aff.has_value() && target.has_value(), "affinity resolves", ok);
        if (aff && target && *aff == *target) ++aff_colocated;

        // AntiAffinity: never Ledger's node. Resolve WITH and WITHOUT the bin cache — must match
        // (proves the cache is bypassed) and must never equal the target.
        const auto anti_nobins = resolve_placement<Placement<HashById, AntiAffinity<Ledger>>>(cart_id, view);
        const auto anti_bins =
            resolve_placement<Placement<HashById, AntiAffinity<Ledger>>>(cart_id, view, NodeId{}, &bins);
        check(anti_nobins.has_value() && anti_bins.has_value(), "antiaffinity resolves", ok);
        if (!anti_nobins || !anti_bins || !target) continue;
        if (*anti_nobins != *anti_bins) ++anti_bypass_mismatch;  // must stay 0 (bin cache ignored)
        if (anti_nobins->value == target->value) ++anti_on_target;  // must stay 0

        // Diagnostic teeth: for ids whose BIN owner is the forbidden node, the bin cache WOULD have
        // co-located them — AntiAffinity avoided it, so it demonstrably did not consult the bin owner.
        const auto bin_owner = bins.owner_of(cart_id);
        if (bin_owner && target && bin_owner->value == target->value) {
            ++bin_eq_target;
            if (anti_nobins->value != bin_owner->value) ++anti_bin_disagree;
        }
    }

    check(aff_colocated == M, "Affinity<Ledger>: every Cart co-locates on Ledger's node", ok);
    check(anti_on_target == 0, "AntiAffinity<Ledger>: NEVER lands on Ledger's node", ok);
    check(anti_bypass_mismatch == 0,
          "AntiAffinity is EXACT: result is identical with/without the bin cache (cache bypassed)", ok);
    check(bin_eq_target > 0,
          "there exist ids whose bin-quantized owner IS the forbidden node (teeth are live)", ok);
    check(anti_bin_disagree == bin_eq_target,
          "for every such id AntiAffinity avoided the bin owner — it bypassed the bin cache", ok);

    std::printf(
        "  aff_colocated=%llu/%llu  anti_on_target=%llu  bin==target=%llu  anti!=bin(there)=%llu\n",
        (unsigned long long)aff_colocated, (unsigned long long)M, (unsigned long long)anti_on_target,
        (unsigned long long)bin_eq_target, (unsigned long long)anti_bin_disagree);
    std::printf("placement_affinity_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
