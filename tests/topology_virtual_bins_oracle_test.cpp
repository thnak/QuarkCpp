// Tests 026-Large-Scale-Cluster-Topology §"VirtualBins" — the bin table is a FAITHFUL CACHE of the
// O(N) HRW oracle (ADR-006 C3: "bin_owner == per-bin HRW"). VirtualBins quantizes placement into B
// bins, so it caches placement at BIN granularity: for every actor, `owner_of(id)` must equal the HRW
// winner (place_hash) computed INDEPENDENTLY for that id's bin's representative key. This proves the
// cache faithfully reflects the O(N) argmax with no corruption / mis-indexing / skew.
//
// NOTE (spec precision): the faithful equality is PER-BIN, not per-id. `place_hash(id.hash())` (raw
// per-id HRW) and `owner_of(id)` differ at HRW sub-bin boundaries — that is the DELIBERATE 026
// quantization ("bins quantize placement; two actors sharing a bin co-locate"). We assert the per-bin
// oracle match at 100% (the load-bearing invariant) and also PRINT the per-id agreement for honesty.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

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
    constexpr std::uint64_t kMaxNodes = 256;
    const std::uint64_t B = virtual_bin_count(kMaxNodes);  // 4096

    // A live cluster of N nodes (well under max_nodes, so bins/node is generous).
    constexpr std::uint64_t N = 40;
    auto vec = std::make_shared<std::vector<NodeId>>();
    for (std::uint64_t i = 1; i <= N; ++i) vec->push_back(NodeId{i * 131 + 17});
    MembershipView view{vec, 1};
    const std::span<const NodeId> nodes = view.nodes();

    VirtualBins vb(nodes, B);

    // (1) PER-BIN faithful cache: owner_of(id) == place_hash(bin_key(bin_of(id))) for EVERY id.
    std::uint64_t per_bin_mismatch = 0, per_id_disagree = 0;
    constexpr std::uint64_t M = 200'000;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId id{TypeKey{0x5150}, k};
        const std::optional<NodeId> cached = vb.owner_of(id);
        const std::optional<NodeId> per_bin = place_hash(bin_key(vb.bin_of(id)), nodes);
        if (cached != per_bin) ++per_bin_mismatch;
        // Per-id raw HRW (the un-quantized oracle) — recorded, NOT asserted equal (quantization).
        if (cached != place(id, view)) ++per_id_disagree;
    }
    check(per_bin_mismatch == 0,
          "owner_of(id) == per-bin HRW oracle place_hash(bin_key) for every id (faithful cache)", ok);

    // (2) Every raw bin owner equals the independent per-bin HRW winner (the table build is exact).
    std::uint64_t bin_build_mismatch = 0;
    for (std::uint64_t b = 0; b < B; ++b) {
        const std::optional<NodeId> w = place_hash(bin_key(b), nodes);
        if (!w || !(vb.owner_of_bin(b) == *w)) ++bin_build_mismatch;
    }
    check(bin_build_mismatch == 0, "each of B bins stores exactly its per-bin HRW winner", ok);

    const double per_id_agree = 1.0 - static_cast<double>(per_id_disagree) / static_cast<double>(M);
    std::printf("  oracle match: per-bin mismatches=%llu / %llu ids (0 required); "
                "bin-build mismatches=%llu / %llu bins; per-id HRW agreement=%.4f (quantization gap)\n",
                static_cast<unsigned long long>(per_bin_mismatch), static_cast<unsigned long long>(M),
                static_cast<unsigned long long>(bin_build_mismatch), static_cast<unsigned long long>(B), per_id_agree);
    std::printf("topology_virtual_bins_oracle_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
