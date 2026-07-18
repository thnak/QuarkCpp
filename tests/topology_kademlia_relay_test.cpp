// Tests 026-Large-Scale-Cluster-Topology §"DHT-Relay" — a MAINTAINED Kademlia XOR-routed overlay
// delivers to every target within <= ceil(log2 N) hops, 100% (ADR-006 F3). The MANDATORY control — an
// UNMAINTAINED view where each node knows only a tiny random subset — dead-ends a large fraction of
// routes, proving it is table MAINTENANCE that earns the hop bound (F3: unmaintained dead-ends 46-95%).
// Deterministic: node ids + the control's known-subsets are seeded from splitmix64 (no random_device).
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

// ceil(log2 n) for n >= 1.
unsigned clog2(std::uint64_t n) {
    if (n < 2) return 1;
    return static_cast<unsigned>(64 - std::countl_zero(n - 1));
}

std::vector<NodeId> make_roster(std::uint64_t N, std::uint64_t seed) {
    std::vector<NodeId> r;
    std::uint64_t x = seed;
    for (std::uint64_t i = 0; i < N; ++i) {
        x = detail::splitmix64(x);
        r.push_back(NodeId{x | 1});  // well-spread 64-bit ids
    }
    return r;
}

void run_n(std::uint64_t N, bool& ok) {
    const std::vector<NodeId> roster = make_roster(N, 0xC0FFEE + N);
    const unsigned bound = clog2(N);

    // ---- MAINTAINED: every node knows the whole roster (k-buckets keep k closest per bucket). ----
    RelayOverlay maintained(roster, /*k*/ 16);
    std::vector<std::uint64_t> hist(bound + 2, 0);
    std::uint64_t completed = 0, over_bound = 0, total = 0, max_hops = 0;
    // Route from a fixed set of senders to ALL targets (bounded: sample senders to cap work).
    const std::uint64_t senders = N < 64 ? N : 64;
    for (std::uint64_t si = 0; si < senders; ++si) {
        const NodeId src = roster[si];
        for (NodeId dst : roster) {
            if (dst == src) continue;
            ++total;
            const std::vector<NodeId> path = maintained.route(src, dst, /*max_hops*/ bound);
            if (!path.empty() && path.back() == dst) {
                ++completed;
                const std::uint64_t h = path.size();
                if (h > max_hops) max_hops = h;
                if (h <= bound) ++hist[h];
                if (h > bound) ++over_bound;
            }
        }
    }
    const double comp_frac = static_cast<double>(completed) / static_cast<double>(total);
    std::printf("  N=%llu bound=ceil(log2 N)=%u : maintained completion=%.4f max_hops=%llu over_bound=%llu\n",
                static_cast<unsigned long long>(N), bound, comp_frac, static_cast<unsigned long long>(max_hops),
                static_cast<unsigned long long>(over_bound));
    std::printf("    hop histogram:");
    for (unsigned h = 1; h <= bound; ++h)
        std::printf(" h%u=%llu", h, static_cast<unsigned long long>(hist[h]));
    std::printf("\n");
    check(completed == total, "maintained overlay: 100% completion within the hop bound", ok);
    check(over_bound == 0, "maintained overlay: every route <= ceil(log2 N) hops", ok);
    check(max_hops <= bound, "max hops <= ceil(log2 N)", ok);

    // ---- CONTROL: UNMAINTAINED — each node knows only ~2 random peers (empty buckets dead-end). ----
    auto known_of = [&](NodeId self) {
        std::vector<NodeId> kn;
        std::uint64_t x = detail::splitmix64(self.value ^ 0x9151);
        for (int i = 0; i < 2; ++i) {
            x = detail::splitmix64(x);
            kn.push_back(roster[static_cast<std::size_t>(x % roster.size())]);
        }
        return kn;
    };
    RelayOverlay unmaintained(roster, /*k*/ 0, known_of);
    std::uint64_t dead = 0, tot2 = 0;
    for (std::uint64_t si = 0; si < senders; ++si) {
        const NodeId src = roster[si];
        for (NodeId dst : roster) {
            if (dst == src) continue;
            ++tot2;
            const std::vector<NodeId> path = unmaintained.route(src, dst, /*max_hops*/ bound);
            if (path.empty() || path.back() != dst) ++dead;
        }
    }
    const double dead_frac = static_cast<double>(dead) / static_cast<double>(tot2);
    std::printf("  N=%llu : UNMAINTAINED control dead-end fraction=%.4f (F3: 46-95%%)\n",
                static_cast<unsigned long long>(N), dead_frac);
    check(dead_frac > 0.30, "control: unmaintained view dead-ends a large fraction (teeth)", ok);
}
}  // namespace

int main() {
    bool ok = true;
    run_n(256, ok);
    run_n(1024, ok);
    std::printf("topology_kademlia_relay_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
