// Tests 026-Large-Scale-Cluster-Topology "ZERO-COST WHEN UNUSED" (026 normative). The `Cache::Direct`
// placement path uses the 010 `place()` oracle and constructs NO bin table: a Direct cluster holds no
// VirtualBins. Proven two ways:
//   (1) DirectPlacement is an EMPTY type (holds no state) — compile-time.
//   (2) Global operator new is hooked; resolving many ids through the Direct path allocates ZERO on the
//       heap, while building a VirtualBins DOES allocate (the table) — so the cost exists only when the
//       large-scale cache is actually selected.
// (This test replaces global operator new, so it is auto-excluded from the TSan build by
// tests/CMakeLists.txt content-detection — that is correct; the 0-alloc invariant is verified under
// gcc/clang/ASan.)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

#include "quark/core/cluster_topology.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

namespace {
std::atomic<std::size_t> g_allocs{0};
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::aligned_alloc(static_cast<std::size_t>(a), n != 0 ? n : static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

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

    // (1) DirectPlacement holds no state (zero-cost holder) — the config default constructs no cache.
    static_assert(std::is_empty_v<DirectPlacement>, "Direct placement must be empty");
    check(std::is_empty_v<DirectPlacement>, "DirectPlacement is an empty type (no bin table)", ok);

    // Build a roster (cold, allocates — outside the measured window).
    auto vec = std::make_shared<std::vector<NodeId>>();
    for (std::uint64_t i = 1; i <= 32; ++i) vec->push_back(NodeId{i * 71 + 3});
    MembershipView view{vec, 1};

    // ---- Measured window A: the Direct path resolves 0 heap allocations ---------------------------
    volatile std::uint64_t sink = 0;
    g_allocs.store(0, std::memory_order_relaxed);
    for (std::uint64_t k = 0; k < 200'000; ++k) {
        const std::optional<NodeId> o = DirectPlacement::owner_of(ActorId{TypeKey{7}, k}, view);
        sink += o ? o->value : 0;
    }
    const std::size_t direct_allocs = g_allocs.load(std::memory_order_relaxed);
    check(direct_allocs == 0, "Direct placement path performs 0 heap allocations (zero-cost)", ok);

    // Direct must agree with the 010 oracle exactly (it IS place()).
    bool agrees = true;
    for (std::uint64_t k = 0; k < 5000; ++k) {
        const ActorId id{TypeKey{7}, k};
        if (DirectPlacement::owner_of(id, view) != place(id, view)) agrees = false;
    }
    check(agrees, "Direct placement == place() (the 010 fast path, unchanged)", ok);

    // ---- Measured window B: selecting VirtualBins DOES allocate (the table) — cost only when used --
    g_allocs.store(0, std::memory_order_relaxed);
    VirtualBins bins(view.nodes(), virtual_bin_count(32));
    const std::size_t bins_allocs = g_allocs.load(std::memory_order_relaxed);
    check(bins_allocs > 0, "VirtualBins build allocates the table (cost paid only when selected)", ok);
    (void)bins.owner_of(ActorId{TypeKey{7}, 1});

    std::printf("  zero-cost: Direct allocs=%zu (must be 0), VirtualBins build allocs=%zu (must be >0), "
                "sink=%llu\n",
                direct_allocs, bins_allocs, static_cast<unsigned long long>(sink));
    std::printf("topology_zero_cost_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
