// Tests 025 §Part B `Weighted` (ADR-013) — the proportional log-WRH form `score = wₙ / (−ln H)`:
// over a large id sample, each node's ownership SHARE is proportional to its `weight` (ρ = realized /
// weight ≈ 1 at the multinomial floor). The mandatory CONTROL is the NON-PROPORTIONAL `weightₙ·H`
// form (ADR-011/012 control 5): it must MISS proportionality badly, proving the corrected form's pass
// is a real property, not a vacuous one. Deterministic — ids seeded from splitmix64 by index, no RNG.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
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

// The NON-PROPORTIONAL control form: argmax of weightₙ · H (ADR-006 line-104 literal; proven skewed).
std::optional<NodeId> place_weight_times_h(std::uint64_t key_hash, std::span<const NodeId> nodes,
                                           const CapabilityView& v) {
    bool have = false;
    NodeId best{};
    double best_s = 0.0;
    for (NodeId n : nodes) {
        const double h = hrw_unit_open(rendezvous_weight(n, key_hash));
        const double s = v.weight_of(n) * h;
        if (!have || s > best_s || (s == best_s && n.value > best.value)) {
            best = n;
            best_s = s;
            have = true;
        }
    }
    return have ? std::optional<NodeId>(best) : std::nullopt;
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 300'000;

    // Four nodes with weights {1,2,4,8} (Σ = 15). Heterogeneous (min:max = 8), the ADR-013 stress mix.
    const std::vector<NodeId> nodes = {NodeId{101}, NodeId{202}, NodeId{404}, NodeId{808}};
    const std::vector<double> w = {1.0, 2.0, 4.0, 8.0};
    const double W = 15.0;

    auto view = make_capability_view(nodes, /*epoch*/ 1,
                                     {
                                         {NodeId{101}, NodeCapabilities{Scalar{"weight", 1.0}}},
                                         {NodeId{202}, NodeCapabilities{Scalar{"weight", 2.0}}},
                                         {NodeId{404}, NodeCapabilities{Scalar{"weight", 4.0}}},
                                         {NodeId{808}, NodeCapabilities{Scalar{"weight", 8.0}}},
                                     });
    // The node vector is sorted by MembershipView; index the counts by NodeId::value.
    auto idx_of = [&](std::uint64_t nid) -> int {
        for (int i = 0; i < 4; ++i)
            if (nodes[static_cast<std::size_t>(i)].value == nid) return i;
        return -1;
    };

    std::vector<std::uint64_t> cnt(4, 0), cnt_ctrl(4, 0);
    for (std::uint64_t k = 0; k < M; ++k) {
        // A well-distributed key per index (no RNG, no real time) — splitmix64 varied by index.
        const ActorId id{TypeKey{0x51A7ED}, detail::splitmix64(k)};
        const auto r = resolve_placement<Placement<HashById, Weighted>>(id, view);
        check(r.has_value(), "weighted placement always places on a non-empty view", ok);
        if (r) {
            const int i = idx_of(r->value);
            if (i >= 0) ++cnt[static_cast<std::size_t>(i)];
        }
        const auto rc = place_weight_times_h(id.hash(), std::span<const NodeId>(nodes), view);
        if (rc) {
            const int i = idx_of(rc->value);
            if (i >= 0) ++cnt_ctrl[static_cast<std::size_t>(i)];
        }
    }

    // --- Proportional form: realized ρ = (share / M) / (w / W) must sit near 1 at every node. -----
    std::printf("  weight | share       | ideal p   | rho=share/ideal  (log-WRH, ADR-013)\n");
    double max_dev = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double share = static_cast<double>(cnt[static_cast<std::size_t>(i)]) / M;
        const double p = w[static_cast<std::size_t>(i)] / W;
        const double rho = share / p;
        max_dev = std::max(max_dev, std::fabs(rho - 1.0));
        std::printf("   %4.0f  | %.6f    | %.6f  | rho=%.4f\n", w[static_cast<std::size_t>(i)], share,
                    p, rho);
        check(rho > 0.93 && rho < 1.07,
              "weighted ownership share is proportional to weight (rho within [0.93,1.07])", ok);
    }
    std::printf("  proportional-form max |rho-1| = %.4f (band 0.07)\n", max_dev);

    // --- Control: weightₙ·H must be visibly NON-proportional (some node's rho far from 1). ---------
    double ctrl_max_dev = 0.0;
    std::printf("  CONTROL weight*H (non-proportional):\n");
    for (int i = 0; i < 4; ++i) {
        const double share = static_cast<double>(cnt_ctrl[static_cast<std::size_t>(i)]) / M;
        const double p = w[static_cast<std::size_t>(i)] / W;
        const double rho = share / p;
        ctrl_max_dev = std::max(ctrl_max_dev, std::fabs(rho - 1.0));
        std::printf("   %4.0f  | share=%.6f | ideal=%.6f | rho=%.4f\n", w[static_cast<std::size_t>(i)],
                    share, p, rho);
    }
    std::printf("  control max |rho-1| = %.4f\n", ctrl_max_dev);
    check(ctrl_max_dev > 0.15,
          "CONTROL: weight*H is NON-proportional (some node's rho far from 1) — teeth fire", ok);
    check(max_dev < ctrl_max_dev,
          "the proportional log-WRH form is materially closer to proportional than weight*H", ok);

    std::printf("placement_weighted_proportionality_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
