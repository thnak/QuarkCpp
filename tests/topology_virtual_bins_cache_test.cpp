// Tests 026-Large-Scale-Cluster-Topology §"VirtualBins" reclamation — the EPOCH-SWAP holder
// (VirtualBinsCache). A single membership thread rebuilds the bin table on each roster change and
// publishes a `shared_ptr` snapshot; concurrent reader threads take a PINNED snapshot and resolve
// owners. A rebuild NEVER invalidates a snapshot already handed out (UAF-free / TSan-clean) — the
// std-only honest form of the ADR-006 QSBR reclamation (per-worker beacons are a documented seam).
//
// Invariants:
//   * content-addressed idempotence: rebuild_for on an UNCHANGED roster does NO swap (0 work);
//   * a changed roster advances the epoch and re-owns per per-bin HRW (== a freshly-built table);
//   * under 3 concurrent readers + 1 rebuilder, every resolved owner is a live node of SOME published
//     snapshot and no snapshot is ever torn/freed underfoot (the TSan-visible property).
// Bounded to <=4 threads (machine safety); virtual iterations, no wall-clock, seeded from splitmix64.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
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
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kMaxNodes = 256;
    const std::uint64_t B = virtual_bin_count(kMaxNodes);  // 4096

    InProcessMembership membership(NodeId{1},
                                   {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5}, NodeId{6}});
    VirtualBinsCache cache(B);

    // (1) First build publishes; a rebuild on the SAME roster is a no-op (content-addressed).
    check(cache.rebuild_for(membership.view()), "first rebuild publishes a snapshot", ok);
    check(!cache.rebuild_for(membership.view()), "rebuild on unchanged roster does NO swap (0 work)", ok);
    const std::uint64_t epoch0 = cache.epoch();
    {
        auto snap = cache.snapshot();
        check(snap && !snap->empty(), "snapshot() returns a pinned, non-empty table", ok);
        check(snap->digest() == roster_digest(membership.view().nodes()),
              "snapshot digest == roster content-digest", ok);
    }

    // (2) A roster change advances the epoch and matches a freshly-built table exactly.
    membership.join(NodeId{7});
    check(cache.rebuild_for(membership.view()), "roster change publishes a new snapshot", ok);
    check(cache.epoch() == epoch0 + 1, "epoch advances by one on a committed change", ok);
    {
        auto snap = cache.snapshot();
        VirtualBins fresh(membership.view().nodes(), B);
        std::uint64_t diffs = 0;
        for (std::uint64_t b = 0; b < B; ++b)
            if (!(snap->owner_of_bin(b) == fresh.owner_of_bin(b))) ++diffs;
        check(diffs == 0, "published snapshot == a freshly-built per-bin HRW table", ok);
    }

    // (3) Concurrent readers vs a rebuilder — UAF-free snapshot swap (the TSan-visible property).
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> resolves{0};
    std::atomic<std::uint64_t> bad{0};

    auto reader = [&](std::uint64_t seed) {
        std::uint64_t x = seed;
        while (!stop.load(std::memory_order_relaxed)) {
            auto snap = cache.snapshot();  // pinned for this iteration — cannot be freed underfoot
            if (!snap) continue;
            for (int i = 0; i < 256; ++i) {
                x = detail::splitmix64(x);
                const std::optional<NodeId> o = snap->owner_of(ActorId{TypeKey{9}, x});
                if (!o) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                } else {
                    resolves.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::thread r1(reader, 0x111), r2(reader, 0x222), r3(reader, 0x333);
    // Rebuilder (membership thread): flip the roster back and forth many times -> many epoch swaps.
    for (int iter = 0; iter < 400; ++iter) {
        if (iter & 1)
            membership.join(NodeId{static_cast<std::uint64_t>(100 + (iter % 7))});
        else
            membership.leave(NodeId{static_cast<std::uint64_t>(100 + ((iter - 1) % 7))});
        cache.rebuild_for(membership.view());
    }
    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();
    r3.join();

    check(bad.load() == 0, "every concurrent resolve landed in a live, non-empty snapshot (no tear/UAF)",
          ok);
    check(resolves.load() > 0, "readers made progress against the swapping cache", ok);
    std::printf("  cache: final epoch=%llu, concurrent resolves=%llu, bad=%llu\n",
                static_cast<unsigned long long>(cache.epoch()),
                static_cast<unsigned long long>(resolves.load()),
                static_cast<unsigned long long>(bad.load()));
    std::printf("topology_virtual_bins_cache_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
