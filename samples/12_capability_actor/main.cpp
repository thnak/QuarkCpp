// Quark sample 12 — Capability-constrained placement (025 Part A/B).
//
// Some actors can only run where the hardware/topology supports them: a GPU decoder needs a GPU node,
// a region-pinned service needs a node in the right zone. In Quark that is expressed by the actor's
// PLACEMENT POLICY, declared right in its CRTP policy list:
//
//     struct GpuWorker : Actor<GpuWorker, Sequential, Placement<HashById, Require<Gpu>>> { ... };
//
// Nodes advertise STATIC, typed capabilities at startup — boolean Flags ("gpu"), string Labels
// ("zone"="eu-west-1"), numeric Scalars ("weight"=capacity). Placement is then a PURE FUNCTION of
// (actor id, gossiped membership + capabilities): every node computes the same answer, no coordinator.
// This sample shows the four modifiers:
//   * Require<Cap>  — HARD filter: place ONLY on nodes that have Cap (empty eligible set => an error).
//   * Prefer<Cap>   — SOFT rank: prefer nodes with Cap, fall back to all eligible if none qualify.
//   * Weighted      — capacity-proportional: a node with weight 3 gets ~3x the actors of a weight-1 node.
//   * the actor's declared policy is visible as compile-time metadata (placement_of<A>).
//
// The capability gossip wire (carrying caps in the SWIM join, disseminating with membership) is a
// documented 010/021 seam; here `make_capability_view` is the std-only stand-in (like InProcessMembership
// for SWIM). Pure functions — no engine, no network, no threads.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 12_capability_actor
// Run  :  taskset -c 0-3 build/samples/12_capability_actor
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "quark/core/actor.hpp"
#include "quark/core/capabilities.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/placement_policies.hpp"

using namespace quark;

// Capability predicates a developer aliases from the ready-made HasFlag/HasLabel.
using Gpu = HasFlag<"gpu">;
using ZoneEU = HasLabel<"zone", "eu-west-1">;

// A message this worker handles — it's a real actor, not just a placement function.
struct Decode {
    std::uint64_t frame;
};

// THE capability actor: it declares that it must be placed on a GPU node (Require<Gpu>) and, among
// those, prefers the eu-west-1 zone (Prefer<ZoneEU>). A plain Sequential handler otherwise.
struct GpuWorker : Actor<GpuWorker, Sequential, Placement<HashById, Require<Gpu>, Prefer<ZoneEU>>> {
    using protocol = Protocol<Decode>;
    void handle(const Decode&) noexcept { /* ... decode on the GPU ... */ }
};

// A worker that requires a capability NO node advertises — to show the graceful empty-eligible error.
using Fpga = HasFlag<"fpga">;
struct FpgaWorker : Actor<FpgaWorker, Sequential, Placement<HashById, Require<Fpga>>> {
    using protocol = Protocol<Decode>;
    void handle(const Decode&) noexcept {}
};

// The declared policy is compile-time METADATA the engine can read (here we just assert it).
static_assert(std::is_same_v<placement_of<GpuWorker>,
                             Placement<HashById, Require<Gpu>, Prefer<ZoneEU>>>,
              "GpuWorker's declared capability policy is visible via placement_of<A>");

static ActorId actor(std::uint64_t key) { return ActorId{TypeKey{0x6907A}, key}; }

int main() {
    bool ok = true;
    constexpr std::uint64_t M = 40'000;

    // A 5-node cluster. GPU on {1,3,5}; zone eu-west-1 on {1,2}; per-node capacity weights.
    const CapabilityView view = make_capability_view(
        {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}}, /*epoch*/ 1,
        {
            {NodeId{1}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "eu-west-1"}, Scalar{"weight", 1.0}}},
            {NodeId{2}, NodeCapabilities{Label{"zone", "eu-west-1"}, Scalar{"weight", 1.0}}},
            {NodeId{3}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "us-east-1"}, Scalar{"weight", 1.0}}},
            {NodeId{4}, NodeCapabilities{Label{"zone", "us-east-1"}, Scalar{"weight", 1.0}}},
            {NodeId{5}, NodeCapabilities{Flag{"gpu"}, Label{"zone", "us-east-1"}, Scalar{"weight", 1.0}}},
        });

    // ---- 1) GpuWorker: Require<Gpu> then Prefer<ZoneEU>. --------------------------------------
    // Require<Gpu> narrows to {1,3,5}; Prefer<ZoneEU> then favors eu among those = {1}. So every
    // GpuWorker lands on node 1 (the only GPU+eu node) — hard requirement + soft preference composed.
    {
        std::uint64_t hit[6] = {0}, off_gpu = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = place_actor<GpuWorker>(actor(k), view);
            if (!r.has_value()) { ok = false; break; }
            if (r->value == 2 || r->value == 4) ++off_gpu;  // MUST be 0 (non-gpu nodes)
            if (r->value <= 5) ++hit[r->value];
        }
        ok &= (off_gpu == 0) && (hit[1] == M);  // all on node 1 (GPU∩eu)
        std::printf("GpuWorker Require<Gpu>+Prefer<eu>: n1=%llu n3=%llu n5=%llu  off-gpu=%llu  (all on n1, the GPU+eu node)\n",
                    (unsigned long long)hit[1], (unsigned long long)hit[3],
                    (unsigned long long)hit[5], (unsigned long long)off_gpu);
    }

    // ---- 2) Require<Gpu> ALONE (no zone preference): spreads across all GPU nodes {1,3,5}. -----
    {
        std::uint64_t hit[6] = {0}, off_gpu = 0;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Require<Gpu>>>(actor(k), view);
            if (!r.has_value()) { ok = false; break; }
            if (r->value == 2 || r->value == 4) ++off_gpu;
            if (r->value <= 5) ++hit[r->value];
        }
        ok &= (off_gpu == 0) && hit[1] > 0 && hit[3] > 0 && hit[5] > 0;
        std::printf("Require<Gpu> only:                n1=%llu n3=%llu n5=%llu  off-gpu=%llu  (balanced over all GPU nodes)\n",
                    (unsigned long long)hit[1], (unsigned long long)hit[3],
                    (unsigned long long)hit[5], (unsigned long long)off_gpu);
    }

    // ---- 3) FpgaWorker: nobody has "fpga" => empty eligible set => a graceful error, not UB. ---
    {
        const auto r = place_actor<FpgaWorker>(actor(7), view);
        const bool graceful = !r.has_value() && r.error().code == errc::not_found;
        ok &= graceful;
        std::printf("FpgaWorker Require<fpga> (no such node): placed=%s  error=%s  (graceful, not UB)\n",
                    r.has_value() ? "yes (BUG)" : "no",
                    graceful ? "errc::not_found" : "UNEXPECTED");
    }

    // ---- 4) Weighted: node capacity drives proportional share. Give node 3 weight 4, others 1. --
    {
        const CapabilityView wview = make_capability_view(
            {NodeId{1}, NodeId{2}, NodeId{3}}, /*epoch*/ 1,
            {
                {NodeId{1}, NodeCapabilities{Scalar{"weight", 1.0}}},
                {NodeId{2}, NodeCapabilities{Scalar{"weight", 1.0}}},
                {NodeId{3}, NodeCapabilities{Scalar{"weight", 4.0}}},  // 4x the capacity
            });
        std::uint64_t hit[4] = {0};
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto r = resolve_placement<Placement<HashById, Weighted>>(actor(k), wview);
            if (!r.has_value()) { ok = false; break; }
            if (r->value <= 3) ++hit[r->value];
        }
        // node 3 should get ~4/6 of the actors; nodes 1 and 2 ~1/6 each. Loose bounds (statistical).
        const double share3 = static_cast<double>(hit[3]) / static_cast<double>(M);
        ok &= (share3 > 0.55 && share3 < 0.78);
        std::printf("Weighted (n3 weight=4x): n1=%llu n2=%llu n3=%llu  (n3 share=%.1f%%, expected ~66.7%%)\n",
                    (unsigned long long)hit[1], (unsigned long long)hit[2],
                    (unsigned long long)hit[3], share3 * 100);
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
