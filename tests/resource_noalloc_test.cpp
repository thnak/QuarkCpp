// Tests 004-Resources' headline property: ZERO dynamic resource resolution while a message is
// processed. After a one-time cold wire pass, a Cached<> access is a pure pointer read — no
// allocation, no container walk. We prove it two ways:
//   (1) a global operator new hook counts every allocation; over >= 1e6 Cached<>::get() accesses
//       (the hot-path shape) the allocation delta is exactly 0 (measured, not asserted-in-comment);
//   (2) the resolved handle address is INVARIANT across all accesses and equals the provided
//       instance — there is no re-resolution / lookup happening under the hood.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "quark/core/resource.hpp"

// --- Global allocation counter (same technique as mailbox_noalloc_test) -----------------------
namespace {
std::atomic<std::uint64_t> g_alloc_count{0};
}

void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace quark;

namespace {
struct Counter {
    std::uint64_t hits = 0;
    QUARK_ALWAYS_INLINE void bump() noexcept { ++hits; }
};
}  // namespace

int main() {
    constexpr std::uint64_t kOps = 1'000'000;

    // Cold setup: build the scope and run the one-time wire pass. Allocations here are ALLOWED
    // (the ResourceScope's vector allocates on the cold path — exactly where 004 puts resolution).
    Counter counter;
    ResourceScope scope;
    scope.provide(counter, ResourceLifetime::Activation);

    Cached<Counter> cached;
    if (!cached.wire(scope)) {
        std::fprintf(stderr, "cold wire failed\n");
        return 1;
    }
    const Counter* const resolved_addr = &cached.get();
    if (resolved_addr != &counter) {
        std::fprintf(stderr, "wire resolved to the wrong instance\n");
        return 1;
    }

    // Measure allocations strictly across the HOT loop (post-wire).
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);

    bool addr_stable = true;
    for (std::uint64_t i = 0; i < kOps; ++i) {
        Counter& c = cached.get();          // hot-path access: pure pointer read, no resolution
        addr_stable &= (&c == resolved_addr);  // no re-resolution: the handle never moves
        c.bump();
    }

    const std::uint64_t after = g_alloc_count.load(std::memory_order_relaxed);
    const std::uint64_t delta = after - before;

    volatile std::uint64_t sink = counter.hits;
    (void)sink;

    const bool ok = (delta == 0) && addr_stable && (counter.hits == kOps);
    std::printf("resource_noalloc_test: %s (ops=%" PRIu64 ", hot-path allocations=%" PRIu64
                ", addr_stable=%d, hits=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kOps, delta, static_cast<int>(addr_stable), counter.hits);
    return ok ? 0 : 1;
}
