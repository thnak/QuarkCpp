// Tests 025-Placement-Policies-and-Stateless-Workers §Part B — capability-constrained placement:
//   * Require<Gpu>  — places ONLY on gpu nodes (hard filter); an empty eligible set surfaces a 007
//     `result` ERROR (not UB).
//   * Prefer<zone>  — ranks preferred nodes first, and FALLS BACK to the full eligible set when no
//     node qualifies.
// Placement is a pure function of (ActorId, annotated membership+capability view); no engine, no net.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/capabilities.hpp"
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

ActorId actor(std::uint64_t key) { return ActorId{TypeKey{0xABCDEF}, key}; }

using Gpu = HasFlag<"gpu">;
using ZoneEU = HasLabel<"zone", "eu-west-1">;

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 40'000;

    // Nodes 1..5. GPU on {1,3,5}. Zone eu-west-1 on {1,2}, us on {3,4,5}.
    auto view = make_capability_view(
        {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}}, /*epoch*/ 1,
        {
            {NodeId{1}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "eu-west-1"}}},
            {NodeId{2}, NodeCapabilities{Label{"zone", "eu-west-1"}}},
            {NodeId{3}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "us-east-1"}}},
            {NodeId{4}, NodeCapabilities{Label{"zone", "us-east-1"}}},
            {NodeId{5}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "us-east-1"}}},
        });

    // ---- (1) Require<Gpu>: every placement lands on a gpu node {1,3,5}. -------------------------
    {
        std::uint64_t on_gpu = 0, off_gpu = 0;
        std::vector<std::uint64_t> hit(6, 0);
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Require<Gpu>>>(actor(k), view);
            check(r.has_value(), "Require<Gpu> places (gpu nodes exist)", ok);
            if (!r) break;
            const std::uint64_t nid = r->value;
            (nid == 1 || nid == 3 || nid == 5) ? ++on_gpu : ++off_gpu;
            if (nid <= 5) ++hit[nid];
        }
        check(off_gpu == 0, "Require<Gpu>: NEVER places on a non-gpu node", ok);
        check(on_gpu == M, "Require<Gpu>: every actor placed on a gpu node", ok);
        check(hit[1] > 0 && hit[3] > 0 && hit[5] > 0, "Require<Gpu>: all 3 gpu nodes are used", ok);
        check(hit[2] == 0 && hit[4] == 0, "Require<Gpu>: non-gpu nodes 2,4 never used", ok);
        std::printf("  Require<Gpu>: n1=%llu n3=%llu n5=%llu (off_gpu=%llu)\n",
                    (unsigned long long)hit[1], (unsigned long long)hit[3],
                    (unsigned long long)hit[5], (unsigned long long)off_gpu);
    }

    // ---- (2) Empty eligible set → a 007 result ERROR (not UB). ----------------------------------
    {
        // No node has BOTH gpu AND zone=ap-south-1 (no such zone advertised at all).
        using ApSouth = HasLabel<"zone", "ap-south-1">;
        const auto r =
            resolve_placement<Placement<HashById, Require<Gpu, ApSouth>>>(actor(7), view);
        check(!r.has_value(), "empty eligible set → error (not a placement)", ok);
        check(r.error().code == errc::not_found,
              "empty-eligible surfaces errc::not_found (007 result, not UB)", ok);
        std::printf("  empty-eligible error: code=%d detail=\"%.*s\"\n",
                    static_cast<int>(r.error().code),
                    static_cast<int>(r.error().detail.size()), r.error().detail.data());
    }

    // ---- (3) Prefer<ZoneEU>: prefers zone eu {1,2}, over an unconstrained eligible set. ---------
    {
        std::uint64_t on_eu = 0, off_eu = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Prefer<ZoneEU>>>(actor(k), view);
            check(r.has_value(), "Prefer places", ok);
            if (!r) break;
            (r->value == 1 || r->value == 2) ? ++on_eu : ++off_eu;
        }
        check(on_eu == M, "Prefer<ZoneEU>: ranks eu nodes first — all placements land on {1,2}", ok);
        check(off_eu == 0, "Prefer<ZoneEU>: no placement escapes the preferred set when it is non-empty",
              ok);
        std::printf("  Prefer<ZoneEU>: on_eu=%llu off_eu=%llu\n", (unsigned long long)on_eu,
                    (unsigned long long)off_eu);
    }

    // ---- (4) Prefer FALLBACK: no node qualifies → fall back to the full eligible set. -----------
    {
        using ZoneMars = HasLabel<"zone", "mars-1">;  // no node advertises it
        std::vector<std::uint64_t> hit(6, 0);
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Prefer<ZoneMars>>>(actor(k), view);
            check(r.has_value(), "Prefer fallback still places", ok);
            if (!r) break;
            if (r->value <= 5) ++hit[r->value];
        }
        std::uint64_t used = 0;
        for (std::uint64_t i = 1; i <= 5; ++i)
            if (hit[i] > 0) ++used;
        check(used == 5, "Prefer<unmatchable>: falls back to ALL eligible nodes (all 5 used)", ok);
        std::printf("  Prefer fallback: distinct nodes used=%llu (expect 5)\n",
                    (unsigned long long)used);
    }

    // ---- (4b) LocalFirst: the calling node wins when it is eligible; else fall through to HRW. ---
    {
        // Caller = node 2 (eu, no gpu). With LocalFirst + no hard constraint, node 2 is eligible ⇒
        // every placement pins to the caller (per-caller latency optimization, deterministic).
        std::uint64_t on_caller = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r =
                resolve_placement<Placement<HashById, LocalFirst>>(actor(k), view, /*caller*/ NodeId{2});
            if (r && r->value == 2) ++on_caller;
        }
        check(on_caller == M, "LocalFirst: eligible caller node wins every placement (locality)", ok);

        // Caller = node 2, but Require<Gpu> makes it INELIGIBLE ⇒ LocalFirst falls through to HRW over
        // the gpu subset (never the caller).
        std::uint64_t caller_leak = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Require<Gpu>, LocalFirst>>(
                actor(k), view, /*caller*/ NodeId{2});
            if (r && r->value == 2) ++caller_leak;
        }
        check(caller_leak == 0, "LocalFirst falls through when the caller is ineligible (Require wins)",
              ok);
        std::printf("  LocalFirst: on_caller=%llu caller_leak=%llu\n", (unsigned long long)on_caller,
                    (unsigned long long)caller_leak);
    }

    // ---- (5) Determinism: a HashById placement over eligible == place_hash over the same subset. -
    {
        // Manually build the gpu subset and confirm Require<Gpu> agrees with raw HRW over it.
        std::vector<NodeId> gpu = {NodeId{1}, NodeId{3}, NodeId{5}};
        bool agree = true;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto a = resolve_placement<Placement<HashById, Require<Gpu>>>(actor(k), view);
            const auto b = place_hash(actor(k).hash(), gpu);
            if (!a || !b || *a != *b) agree = false;
        }
        check(agree, "Require<Gpu> == uniform HRW over the eligible gpu subset (minimal-reassignment)",
              ok);
    }

    std::printf("placement_capability_eligibility_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
