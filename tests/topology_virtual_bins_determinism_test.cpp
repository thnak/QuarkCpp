// Tests 026-Large-Scale-Cluster-Topology §"VirtualBins" — CONTENT-ADDRESSED DETERMINISM (ADR-006 C1).
// Two independently-ORDERED views over the SAME node CONTENT must build a byte-identical bin table:
// same roster content-digest AND the same owner in every one of B bins, regardless of the order the
// roster was assembled — the whole basis of coordinator-free O(1) placement (every node agrees).
// Covers >= 4096 bins x >= 1e5 ids (the ADR-006 C1 scale). Determinism: bins are a pure function of the
// node SET + the fixed splitmix64 mixer — no time, no random_device.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster.hpp"
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

MembershipView view_of(const std::vector<std::uint64_t>& order) {
    auto vec = std::make_shared<std::vector<NodeId>>();
    for (auto v : order) vec->push_back(NodeId{v});  // deliberately NOT sorted — order must not matter
    return MembershipView{vec, 1};
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kMaxNodes = 256;
    const std::uint64_t B = virtual_bin_count(kMaxNodes);  // next_pow2(16*256) = 4096
    check(B >= 4096, "bin count >= 4096 (test scale)", ok);

    // Same 32-node CONTENT, three independent orderings.
    std::vector<std::uint64_t> ascending, shuffled, reversed;
    for (std::uint64_t i = 1; i <= 32; ++i) ascending.push_back(i * 7 + 3);
    // A deterministic shuffle seeded by splitmix64 (never random_device).
    shuffled = ascending;
    std::uint64_t r = 0xB1A5;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
        r = detail::splitmix64(r);
        std::swap(shuffled[i - 1], shuffled[static_cast<std::size_t>(r % i)]);
    }
    reversed.assign(ascending.rbegin(), ascending.rend());

    VirtualBins va(view_of(ascending).nodes(), B);
    VirtualBins vb(view_of(shuffled).nodes(), B);
    VirtualBins vc(view_of(reversed).nodes(), B);

    // (1) Same content-digest despite different assembly order.
    check(va.digest() == vb.digest() && va.digest() == vc.digest(),
          "same node content -> identical roster content-digest (order-independent)", ok);
    check(va.bucket_count() == B && vb.bucket_count() == B && vc.bucket_count() == B,
          "all tables have B bins", ok);

    // (2) Byte-identical bin table: every one of B bins owns the same node.
    std::uint64_t bin_diffs = 0;
    const auto ta = va.table(), tb = vb.table(), tc = vc.table();
    for (std::uint64_t i = 0; i < B; ++i)
        if (!(ta[i] == tb[i]) || !(ta[i] == tc[i])) ++bin_diffs;
    check(bin_diffs == 0, "byte-identical bin_table across 3 independently-ordered views", ok);

    // (3) O(1) owner_of agrees across the three tables over >= 1e5 ids.
    std::uint64_t id_diffs = 0;
    constexpr std::uint64_t M = 120'000;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId id{TypeKey{0xABCDEF}, k};
        if (va.owner_of(id) != vb.owner_of(id) || va.owner_of(id) != vc.owner_of(id)) ++id_diffs;
    }
    check(id_diffs == 0, "owner_of() identical across the three tables over 1.2e5 ids", ok);

    std::printf("  VirtualBins determinism: B=%llu, bin_diffs=%llu, id_diffs=%llu over M=%llu\n",
                static_cast<unsigned long long>(B), static_cast<unsigned long long>(bin_diffs), static_cast<unsigned long long>(id_diffs),
                static_cast<unsigned long long>(M));
    std::printf("topology_virtual_bins_determinism_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
