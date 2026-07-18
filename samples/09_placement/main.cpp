// Quark sample 09 — Placement (010 rendezvous/HRW + 026 VirtualBins).
//
// Where does an actor live? Quark answers with RENDEZVOUS (HRW) placement: `place(actor_id, view)` is
// a pure function of the actor's identity and the current node set. That gives three properties for
// free, with NO coordinator and NO lookup table:
//   1. DETERMINISTIC — every node computes the same owner independently (order of the roster doesn't
//      matter), so any node can route to any actor without asking anyone.
//   2. BALANCED — many actors spread roughly uniformly across the nodes (low coefficient of variation).
//   3. O(1) at scale — for a large roster, the 026 `VirtualBins` table turns the O(N) HRW scan into a
//      single bin lookup. It QUANTIZES placement into B bins (each bin owned by the per-bin HRW
//      winner), so it is deterministic and balanced like HRW, but co-locates the actors that share a
//      bin — deliberately NOT identical to per-key HRW at sub-bin boundaries (that's the 026 tradeoff).
//
// Pure functions of the node set: no engine, no network, no threads.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 09_placement
// Run  :  taskset -c 0-3 build/samples/09_placement
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include "quark/core/cluster_topology.hpp"  // VirtualBins (026)
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

using namespace quark;

static ActorId actor(std::uint64_t key) { return ActorId{TypeKey{0xABCDEF}, key}; }

// Build a membership view over the given node ids in a SPECIFIC order (to prove order-independence).
static MembershipView view_of(const std::vector<std::uint64_t>& order, std::uint64_t epoch = 1) {
    auto vec = std::make_shared<std::vector<NodeId>>();
    for (auto v : order) vec->push_back(NodeId{v});
    return MembershipView{vec, epoch};  // deliberately NOT sorted
}

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 100'000;

    // ---- 1) Determinism: same owner regardless of roster order --------------------------------
    MembershipView ascending = view_of({1, 2, 3, 4, 5, 6, 7, 8});
    MembershipView shuffled = view_of({5, 1, 8, 3, 7, 2, 6, 4});
    MembershipView reversed = view_of({8, 7, 6, 5, 4, 3, 2, 1});
    std::uint64_t disagreements = 0;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId id = actor(k);
        const auto a = place(id, ascending);
        const auto b = place(id, shuffled);
        const auto c = place(id, reversed);
        if (!a || !b || !c || *a != *b || *a != *c) ++disagreements;
    }
    ok &= (disagreements == 0);
    std::printf("determinism: %llu/%llu keys agree across 3 roster orderings  (disagreements=%llu)\n",
                (unsigned long long)(M - disagreements), (unsigned long long)M,
                (unsigned long long)disagreements);

    // ---- 2) Balance: spread M actors over 8 nodes, report the coefficient of variation ---------
    std::vector<std::uint64_t> counts(9, 0);  // index by node id 1..8
    for (std::uint64_t k = 0; k < M; ++k) counts[place(actor(k), ascending)->value]++;
    double mean = static_cast<double>(M) / 8.0, var = 0;
    for (std::uint64_t n = 1; n <= 8; ++n) { const double d = counts[n] - mean; var += d * d; }
    const double cov = std::sqrt(var / 8.0) / mean;
    ok &= (cov < 0.05);
    std::printf("balance: per-node counts");
    for (std::uint64_t n = 1; n <= 8; ++n) std::printf(" %llu", (unsigned long long)counts[n]);
    std::printf("   CoV=%.4f  (well-balanced: <0.05)\n", cov);

    // ---- 3) O(1) VirtualBins: deterministic + balanced, quantized (not per-key HRW) ------------
    // Two bin tables built over the SAME nodes in DIFFERENT orders must give identical owners for
    // every key (determinism) and identical byte-for-byte bin tables (order-independence).
    std::vector<NodeId> nodes_a, nodes_b;
    for (std::uint64_t n = 1; n <= 8; ++n) nodes_a.push_back(NodeId{n});
    for (std::uint64_t n = 8; n >= 1; --n) nodes_b.push_back(NodeId{n});   // reversed order
    constexpr std::size_t B = 4096;                                        // 026: B >> N for balance
    VirtualBins bins_a(std::span<const NodeId>(nodes_a), B);
    VirtualBins bins_b(std::span<const NodeId>(nodes_b), B);

    std::uint64_t owner_disagreements = 0, hrw_agreements = 0;
    std::vector<std::uint64_t> vb_counts(9, 0);
    for (std::uint64_t k = 0; k < M; ++k) {
        const auto oa = bins_a.owner_of(actor(k));
        const auto ob = bins_b.owner_of(actor(k));
        if (!oa || !ob || oa->value != ob->value) ++owner_disagreements;   // MUST be 0 (deterministic)
        if (oa) vb_counts[oa->value]++;
        if (oa && place(actor(k), ascending)->value == oa->value) ++hrw_agreements;  // partial by design
    }
    double vb_mean = static_cast<double>(M) / 8.0, vb_var = 0;
    for (std::uint64_t n = 1; n <= 8; ++n) { const double d = vb_counts[n] - vb_mean; vb_var += d * d; }
    const double vb_cov = std::sqrt(vb_var / 8.0) / vb_mean;
    ok &= (owner_disagreements == 0) && (bins_a.digest() == bins_b.digest()) && (vb_cov < 0.10);
    std::printf("O(1) VirtualBins: order-independent (digest match=%s, key disagreements=%llu), "
                "balanced CoV=%.4f\n",
                (bins_a.digest() == bins_b.digest()) ? "yes" : "NO",
                (unsigned long long)owner_disagreements, vb_cov);
    std::printf("            (per-key agreement with raw HRW = %.1f%% — the DELIBERATE 026 bin "
                "quantization, not a bug)\n",
                100.0 * static_cast<double>(hrw_agreements) / static_cast<double>(M));

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
