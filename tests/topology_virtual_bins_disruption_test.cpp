// Tests 026-Large-Scale-Cluster-Topology §"VirtualBins" — MINIMAL DISRUPTION on a membership change
// (ADR-006 C2), inherited from HRW at BIN granularity. A join/leave rebuilds the bin table but re-owns
// only the expected small fraction of bins, and NO bin ever moves between two nodes that both survive
// the change:
//   * JOIN(x): a bin moves IFF x becomes its per-bin HRW winner; every moved bin now maps to x; no bin
//     moves between two pre-existing nodes. Expected moved fraction ~ 1/(N+1).
//   * LEAVE(x): only bins previously owned by x re-own; no other bin moves; none re-owns onto x.
// Modeled on distribution_replacement_test.cpp, at bin granularity.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kMaxNodes = 512;
    const std::uint64_t B = virtual_bin_count(kMaxNodes);  // 8192
    constexpr std::uint64_t N = 16;

    std::vector<NodeId> roster;
    for (std::uint64_t i = 1; i <= N; ++i) roster.push_back(NodeId{i * 97 + 5});

    auto owners_of = [&](const std::vector<NodeId>& r) {
        VirtualBins vb(std::span<const NodeId>(r), B);
        std::vector<std::uint64_t> o(B);
        for (std::uint64_t b = 0; b < B; ++b) o[b] = vb.owner_of_bin(b).value;
        return o;
    };

    const std::vector<std::uint64_t> before = owners_of(roster);

    // ---- JOIN a new node -----------------------------------------------------------------------
    {
        const std::uint64_t joiner = 99999;
        std::vector<NodeId> r2 = roster;
        r2.push_back(NodeId{joiner});
        const std::vector<std::uint64_t> after = owners_of(r2);

        std::uint64_t moved = 0, to_new = 0, between_old = 0;
        for (std::uint64_t b = 0; b < B; ++b)
            if (after[b] != before[b]) {
                ++moved;
                (after[b] == joiner ? to_new : between_old)++;
            }
        const double frac = static_cast<double>(moved) / static_cast<double>(B);
        std::printf("  JOIN: moved=%.4f (ideal 1/(N+1)=%.4f) to_new=%llu between_old=%llu\n", frac,
                    1.0 / (N + 1), static_cast<unsigned long long>(to_new), static_cast<unsigned long long>(between_old));
        check(between_old == 0, "JOIN: no bin moves between two pre-existing nodes", ok);
        check(to_new == moved, "JOIN: every moved bin now maps to the joiner", ok);
        check(frac > 0.02 && frac < 0.12, "JOIN: moved fraction ~ 1/(N+1) (minimal disruption)", ok);
    }

    // ---- LEAVE a node --------------------------------------------------------------------------
    {
        const std::uint64_t departed = roster[5].value;
        std::vector<NodeId> r2;
        for (NodeId n : roster)
            if (n.value != departed) r2.push_back(n);
        const std::vector<std::uint64_t> after = owners_of(r2);

        std::uint64_t moved = 0, from_departed = 0, from_other = 0, to_departed = 0;
        for (std::uint64_t b = 0; b < B; ++b)
            if (after[b] != before[b]) {
                ++moved;
                (before[b] == departed ? from_departed : from_other)++;
                if (after[b] == departed) ++to_departed;
            }
        const double frac = static_cast<double>(moved) / static_cast<double>(B);
        std::printf("  LEAVE: moved=%.4f (ideal 1/N=%.4f) from_departed=%llu from_other=%llu\n", frac,
                    1.0 / N, static_cast<unsigned long long>(from_departed), static_cast<unsigned long long>(from_other));
        check(from_other == 0, "LEAVE: only bins owned by the departed node re-own", ok);
        check(to_departed == 0, "LEAVE: no bin re-owns onto the departed node", ok);
        check(moved == from_departed, "LEAVE: all moved bins were owned by the departed node", ok);
        check(frac > 0.03 && frac < 0.11, "LEAVE: moved fraction ~ share of the departed node", ok);
    }

    std::printf("topology_virtual_bins_disruption_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
