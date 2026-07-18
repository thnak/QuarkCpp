// Hot-path microbenchmark for 010/026 placement vs the 023 budget (line 141: placement lookup
// p99 ≤ 20 ns goal / ≤ 50 ns hard, N-INDEPENDENT; ADR-006 proved 5–6 ns mean, ≤38 ns p99 VirtualBins).
//
// The budget's load-bearing word is N-INDEPENDENT: raw rendezvous `place()` is O(N) over the roster;
// the 026 `VirtualBins` cache turns it into an O(1) bin lookup (splitmix64 & mask → owner_of_bin). This
// bench measures BOTH so the contrast is visible:
//   * VirtualBins owner_of() at N=16 and N=256 — must be ~equal (N-independent) and under the budget.
//   * raw HRW place() at N=16 and N=64 — GROWS with N (why the cache exists); informational, no gate.
//
// A ~5 ns op is BELOW steady_clock's per-call resolution (~25 ns of clock overhead would dominate a
// per-call timing), so we report the AMORTIZED ns/op over a tight loop (023 §harness allows rdtsc where
// the backend exposes it; the amortized loop is the honest std-only measurement of a sub-clock op). A
// volatile-fed checksum sink prevents the optimizer eliding the lookup.
//
// Pin it: `taskset -c 0 build/bench/placement_bench` — single core, NEVER saturate the machine.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {

std::vector<ActorId> make_ids(std::uint64_t count) {
    std::vector<ActorId> ids;
    ids.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i)
        ids.push_back(ActorId{TypeKey{detail::splitmix64(i ^ 0xA11CE)}, detail::splitmix64(i)});
    return ids;
}

std::vector<NodeId> make_nodes(std::uint64_t n) {
    std::vector<NodeId> nodes;
    nodes.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) nodes.push_back(NodeId{i + 1});
    return nodes;
}

// Amortized ns/op of VirtualBins O(1) owner_of() at a given roster size.
double bench_virtualbins(std::uint64_t n_nodes) {
    const auto nodes = make_nodes(n_nodes);
    VirtualBins vb(std::span<const NodeId>(nodes), 16 * n_nodes);  // 026: B ≥ 16·N for balance
    const auto ids = make_ids(4096);

    constexpr std::uint64_t kWarmup = 1'000'000;
    constexpr std::uint64_t kOps = 20'000'000;
    std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < kWarmup; ++i)
        sink += vb.owner_of(ids[i & 4095]).value_or(NodeId{0}).value;

    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i)
        sink += vb.owner_of(ids[i & 4095]).value_or(NodeId{0}).value;
    const auto t1 = pal::clock::now();

    volatile std::uint64_t keep = sink;
    (void)keep;
    const double ns_per = bench::ns_between(t0, t1) / static_cast<double>(kOps);
    std::printf("  VirtualBins owner_of  N=%4llu : %6.2f ns/op  %s (goal ≤ %.0f / hard ≤ %.0f)\n",
                static_cast<unsigned long long>(n_nodes), ns_per,
                bench::lat_verdict(ns_per, bench::budget::placement_p99_goal_ns,
                                   bench::budget::placement_p99_hard_ns),
                bench::budget::placement_p99_goal_ns, bench::budget::placement_p99_hard_ns);
    return ns_per;
}

// Amortized ns/op of the raw O(N) rendezvous place() — grows with N (informational contrast).
double bench_hrw(std::uint64_t n_nodes) {
    const auto nodes = make_nodes(n_nodes);
    InProcessMembership mem(NodeId{1}, nodes);
    const MembershipView view = mem.view();
    const auto ids = make_ids(4096);

    constexpr std::uint64_t kWarmup = 200'000;
    constexpr std::uint64_t kOps = 5'000'000;
    std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < kWarmup; ++i)
        sink += place(ids[i & 4095], view).value_or(NodeId{0}).value;

    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i)
        sink += place(ids[i & 4095], view).value_or(NodeId{0}).value;
    const auto t1 = pal::clock::now();

    volatile std::uint64_t keep = sink;
    (void)keep;
    const double ns_per = bench::ns_between(t0, t1) / static_cast<double>(kOps);
    std::printf("  raw HRW place()       N=%4llu : %6.2f ns/op  (O(N) — informational, no gate)\n",
                static_cast<unsigned long long>(n_nodes), ns_per);
    return ns_per;
}

}  // namespace

int main() {
    std::printf("== Quark 010/026 placement bench (pin with taskset -c 0) ==\n");
    std::printf("VirtualBins O(1) — the N-INDEPENDENT ≤20 ns gate (023 line 141):\n");
    const double vb_small = bench_virtualbins(16);
    const double vb_large = bench_virtualbins(256);
    const double ratio = vb_small > 0 ? vb_large / vb_small : 0;
    std::printf("  N-independence: N=256 / N=16 = %.2fx  %s (≤1.5x confirms flat)\n",
                ratio, ratio <= 1.5 ? "[flat]" : "[GROWS?]");

    std::printf("raw rendezvous place() — O(N), why the cache exists:\n");
    (void)bench_hrw(16);
    (void)bench_hrw(64);
    return 0;
}
