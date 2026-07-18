// Tests 025 §Part B `Weighted` MINIMAL CHURN (ADR-011 Gate B / ADR-013): raising ONE node's weight
// moves ~only the intended share delta and — the load-bearing invariant — **0 keys move between the
// UNCHANGED-weight nodes**. Every moved key goes to the bumped node; movement is non-vacuous (> 0) and
// never global. The mandatory CONTROL is a MODULO / replica-slot re-shard on the identical Δ: it
// reshuffles a LARGE fraction (≥ 0.5), proving the log-WRH zero-between-unchanged is a real property,
// not an artifact of an immobile table. Deterministic — ids seeded from splitmix64 by index.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/placement_policies.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

CapabilityView view_with_weights(const std::vector<NodeId>& nodes, const std::vector<double>& w) {
    std::vector<std::pair<NodeId, NodeCapabilities>> caps;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        caps.emplace_back(nodes[i], NodeCapabilities{Scalar{"weight", w[i]}});
    return make_capability_view(nodes, /*epoch*/ 1, std::move(caps));
}

// The MODULO / replica-slot re-shard control: node i occupies round(w_i) contiguous slots; owner(id) =
// slot[id % total_slots]. Re-weighting changes total_slots ⇒ nearly every id remaps (ADR-011 control 4).
std::vector<NodeId> build_slots(const std::vector<NodeId>& nodes, const std::vector<double>& w) {
    std::vector<NodeId> slots;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const int reps = static_cast<int>(std::lround(w[i]));
        for (int r = 0; r < reps; ++r) slots.push_back(nodes[i]);
    }
    return slots;
}
NodeId modulo_owner(const std::vector<NodeId>& slots, std::uint64_t key) {
    return slots[key % slots.size()];
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 300'000;

    const std::vector<NodeId> nodes = {NodeId{101}, NodeId{202}, NodeId{404}, NodeId{808}};
    const std::vector<double> w_before = {1.0, 2.0, 4.0, 8.0};   // Σ = 15
    const std::vector<double> w_after = {1.0, 2.0, 4.0, 16.0};   // bump node 808: 8 → 16, Σ = 23
    const NodeId bumped = NodeId{808};

    const CapabilityView vb = view_with_weights(nodes, w_before);
    const CapabilityView va = view_with_weights(nodes, w_after);

    // Intended share delta gained by the bumped node.
    const double share_before = 8.0 / 15.0;
    const double share_after = 16.0 / 23.0;
    const double intended_delta = share_after - share_before;  // ≈ 0.163

    // ---- log-WRH churn ------------------------------------------------------------------------
    std::uint64_t moved = 0, between_unchanged = 0, to_bumped = 0;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId id{TypeKey{0xC0FFEE}, detail::splitmix64(k)};
        const auto a = resolve_placement<Placement<HashById, Weighted>>(id, vb);
        const auto b = resolve_placement<Placement<HashById, Weighted>>(id, va);
        check(a.has_value() && b.has_value(), "weighted placement resolves before & after", ok);
        if (!a || !b) break;
        if (*a != *b) {
            ++moved;
            if (b->value == bumped.value) ++to_bumped;
            // "between unchanged": both endpoints are NON-bumped nodes (their weights did not change).
            if (a->value != bumped.value && b->value != bumped.value) ++between_unchanged;
        }
    }
    const double moved_frac = static_cast<double>(moved) / M;
    std::printf("  log-WRH: moved=%.5f  intended_delta=%.5f  to_bumped=%llu  between_unchanged=%llu\n",
                moved_frac, intended_delta, (unsigned long long)to_bumped,
                (unsigned long long)between_unchanged);
    check(between_unchanged == 0,
          "0 keys move between two UNCHANGED-weight nodes (perfect cross-node minimal disruption)", ok);
    check(to_bumped == moved, "every moved key now maps to the bumped node", ok);
    check(moved > 0, "churn is NON-vacuous (some keys did move — the positive control)", ok);
    check(std::fabs(moved_frac - intended_delta) < 0.03,
          "total moved fraction ≈ the intended share delta (within a few %)", ok);
    check(moved_frac < 0.30, "movement is never GLOBAL (bounded by the intended delta)", ok);

    // ---- Modulo control on the identical Δ: reshuffles a large fraction (teeth). ---------------
    {
        const std::vector<NodeId> sb = build_slots(nodes, w_before);
        const std::vector<NodeId> sa = build_slots(nodes, w_after);
        std::uint64_t cmoved = 0, cbetween = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const std::uint64_t key = detail::splitmix64(k);
            const NodeId a = modulo_owner(sb, key);
            const NodeId b = modulo_owner(sa, key);
            if (a.value != b.value) {
                ++cmoved;
                if (a.value != bumped.value && b.value != bumped.value) ++cbetween;
            }
        }
        const double cfrac = static_cast<double>(cmoved) / M;
        std::printf("  CONTROL modulo re-shard on identical Δ: moved=%.5f  between_unchanged=%llu\n",
                    cfrac, (unsigned long long)cbetween);
        check(cfrac >= 0.50,
              "CONTROL: modulo re-shard moves ≥ 0.5 on the same Δ (near-global reshuffle) — teeth fire",
              ok);
        check(cbetween > 0,
              "CONTROL: modulo moves keys BETWEEN unchanged nodes (what log-WRH provably never does)",
              ok);
    }

    std::printf("placement_weighted_churn_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
