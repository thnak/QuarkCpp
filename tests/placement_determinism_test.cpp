// Tests 025 §Part B determinism invariant: a placement policy is a PURE FUNCTION of (ActorId, gossiped
// membership+capability set). Two capability views built in DIFFERENT insertion orders but with
// IDENTICAL content yield the SAME NodeId for every id — for Require AND Weighted — so every node
// computes the same mapping with no coordinator (the property that makes placement coordinator-free).
#include <cstdint>
#include <cstdio>
#include <utility>
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

using Gpu = HasFlag<"gpu">;

// The SAME logical capability content, assembled in two different node/cap insertion orders.
NodeCapabilities caps_for(std::uint64_t nid) {
    switch (nid) {
        case 11: return NodeCapabilities{Flag{"gpu"}, Scalar{"weight", 3.0}, Label{"zone", "eu"}};
        case 22: return NodeCapabilities{Scalar{"weight", 1.0}, Label{"zone", "us"}};
        case 33: return NodeCapabilities{Flag{"gpu"}, Scalar{"weight", 8.0}, Label{"zone", "us"}};
        case 44: return NodeCapabilities{Scalar{"weight", 2.0}, Flag{"gpu"}, Label{"zone", "eu"}};
        default: return NodeCapabilities{};
    }
}

CapabilityView build(const std::vector<std::uint64_t>& order) {
    std::vector<NodeId> nodes;
    std::vector<std::pair<NodeId, NodeCapabilities>> caps;
    for (std::uint64_t nid : order) {
        nodes.push_back(NodeId{nid});
        caps.emplace_back(NodeId{nid}, caps_for(nid));
    }
    return make_capability_view(std::move(nodes), /*epoch*/ 7, std::move(caps));
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 60'000;

    // Same content, three independent assembly orders.
    const CapabilityView a = build({11, 22, 33, 44});
    const CapabilityView b = build({44, 11, 33, 22});
    const CapabilityView c = build({33, 44, 22, 11});

    bool req_same = true, w_same = true, plain_same = true;
    for (std::uint64_t k = 0; k < M; ++k) {
        const ActorId id{TypeKey{0xDEADBE}, detail::splitmix64(k * 2654435761ULL + 1)};

        const auto ra = resolve_placement<Placement<HashById, Require<Gpu>>>(id, a);
        const auto rb = resolve_placement<Placement<HashById, Require<Gpu>>>(id, b);
        const auto rc = resolve_placement<Placement<HashById, Require<Gpu>>>(id, c);
        if (!ra || !rb || !rc || *ra != *rb || *ra != *rc) req_same = false;

        const auto wa = resolve_placement<Placement<HashById, Weighted>>(id, a);
        const auto wb = resolve_placement<Placement<HashById, Weighted>>(id, b);
        const auto wc = resolve_placement<Placement<HashById, Weighted>>(id, c);
        if (!wa || !wb || !wc || *wa != *wb || *wa != *wc) w_same = false;

        const auto pa = resolve_placement<Placement<HashById>>(id, a);
        const auto pb = resolve_placement<Placement<HashById>>(id, b);
        if (!pa || !pb || *pa != *pb) plain_same = false;
    }

    check(req_same, "Require<Gpu> placement is order-independent (identical content ⇒ identical node)",
          ok);
    check(w_same, "Weighted placement is order-independent (identical content ⇒ identical node)", ok);
    check(plain_same, "unconstrained HashById placement is order-independent", ok);

    std::printf("placement_determinism_test: %s (req=%d weighted=%d plain=%d, M=%llu)\n",
                ok ? "OK" : "FAIL", req_same, w_same, plain_same, (unsigned long long)M);
    return ok ? 0 : 1;
}
