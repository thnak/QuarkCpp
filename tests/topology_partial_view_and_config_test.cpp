// Tests 026-Large-Scale-Cluster-Topology §"Bounded Partial-View" + §"Configuration" (013 / 008
// validation). Two invariants:
//   (A) BoundedPartialView holds an ACTIVE view bounded by ~c*log(max_nodes) peers (O(log N), NOT O(N))
//       plus a PASSIVE backup view (disjoint from active, still no sockets). The active set is a
//       deterministic subset of the roster.
//   (B) The three config axes carry the spec DEFAULTS (Flat/FullMesh/Direct small; PartialView/
//       BoundedPartialView/VirtualBins large) and enforce `B >= 16*max_nodes` (008) + the zero-cost
//       rule (a Flat cluster may not require any large-scale machinery).
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"

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

    // ---- (A) Bounded partial view: O(log N) active, disjoint passive -----------------------------
    {
        constexpr std::uint64_t max_nodes = 4096;  // ceil(log2 4096) = 12 -> bound = c*12 = 36 (c=3)
        std::vector<NodeId> roster;
        std::uint64_t x = 0x9187;
        for (int i = 0; i < 2000; ++i) {
            x = detail::splitmix64(x);
            roster.push_back(NodeId{x | 1});
        }
        const NodeId self = roster[0];
        BoundedPartialView pv(self, roster, max_nodes);
        const std::size_t bound = BoundedPartialView::active_view_bound(max_nodes);  // 36
        std::printf("  partial view: N=%zu active=%zu passive=%zu (O(log N) bound=%zu, roster N/A O(N))\n",
                    roster.size(), pv.active().size(), pv.passive().size(), bound);
        check(pv.active().size() <= bound, "active view <= c*log2(max_nodes) (O(log N), not O(N))", ok);
        check(pv.active().size() * 20 < roster.size(), "active view is far below O(N) (sub-linear)", ok);
        // active and passive are disjoint, and neither contains self.
        bool disjoint = true, no_self = true;
        for (NodeId an : pv.active()) {
            if (an == self) no_self = false;
            for (NodeId pn : pv.passive())
                if (an == pn) disjoint = false;
        }
        check(disjoint, "active and passive views are disjoint", ok);
        check(no_self, "self is not in its own active view", ok);
        check(!pv.passive().empty(), "passive backup view is populated", ok);
        // The bound grows like log N: 8x the nodes adds only ~c*3 peers.
        const std::size_t b_small = BoundedPartialView::active_view_bound(512);
        const std::size_t b_big = BoundedPartialView::active_view_bound(4096);
        check(b_big > b_small && b_big < b_small + 20, "active bound grows logarithmically in max_nodes",
              ok);
    }

    // ---- (B) Config axes: defaults + validation --------------------------------------------------
    {
        ClusterTopologyConfig small;  // defaults
        check(small.topology == Topology::Flat && small.connections == Conn::FullMesh &&
                  small.placement_cache == Cache::Direct,
              "small default = Flat / FullMesh / Direct", ok);
        check(small.buckets() == 0, "Direct config instantiates NO bin table (B==0)", ok);
        check(small.valid(), "the flat small default validates", ok);

        ClusterTopologyConfig big = ClusterTopologyConfig::large_scale(4096);
        check(big.topology == Topology::PartialView && big.connections == Conn::BoundedPartialView &&
                  big.placement_cache == Cache::VirtualBins,
              "large default = PartialView / BoundedPartialView / VirtualBins", ok);
        check(big.buckets() == virtual_bin_count(4096) && big.buckets() >= 16 * 4096,
              "VirtualBins config sizes B = next_pow2(16*max_nodes) >= 16*max_nodes", ok);
        check(big.valid(), "the large-scale default validates (B >= 16*max_nodes)", ok);
        std::printf("  large-scale cfg: max_nodes=%llu B=%llu active_view=%u relay_cap=%u\n",
                    static_cast<unsigned long long>(big.max_nodes), static_cast<unsigned long long>(big.buckets()),
                    big.effective_active_view(), big.effective_relay_cap());
        check(big.effective_relay_cap() == 12, "relay_cap defaults to ceil(log2 max_nodes) = 12", ok);

        // Validation rejects: B < 16*max_nodes is structurally impossible via buckets(), but a Flat
        // topology that also demands large-scale machinery is rejected (zero-cost-when-unused rule).
        ClusterTopologyConfig bad;
        bad.topology = Topology::Flat;
        bad.placement_cache = Cache::VirtualBins;
        bad.max_nodes = 1024;
        check(!bad.valid(), "Flat + VirtualBins is rejected (Flat must instantiate no bin machinery)",
              ok);
        ClusterTopologyConfig zero;
        zero.max_nodes = 0;
        check(!zero.valid(), "max_nodes == 0 is rejected", ok);
    }

    std::printf("topology_partial_view_and_config_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
