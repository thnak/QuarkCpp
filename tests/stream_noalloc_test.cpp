// Tests 024-Streaming §Buffer ownership — 0 per-frame hot-path heap allocation (S2 / ADR-005): the
// ring + slots are pre-allocated COLD at stream-open; the steady push/drain path allocates NOTHING. A
// global operator-new hook counts every allocation; after cold setup we run >=1e6 push->drain->retire
// cycles and assert the delta is EXACTLY 0 (measured, not asserted-in-comment — CONVENTIONS).
//
// This test REPLACES global operator new, so it is auto-excluded from the TSan build (it would collide
// with tsan_cxx's own operator new — see tests/CMakeLists.txt); the 0-alloc invariant stays fully
// verified by the gcc / clang / ASan builds.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <new>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

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
struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};
}  // namespace

int main() {
    constexpr std::uint64_t kOps = 1'000'000;

    // Cold setup: the ring pre-allocates its slot array here (allocations allowed pre-window). Back the
    // pmr with a pre-sized monotonic buffer so even the ONE cold ring allocation is served from a
    // stack buffer and never hits the global new inside the measured window.
    alignas(64) unsigned char arena[64 * 512];
    std::pmr::monotonic_buffer_resource mr(arena, sizeof(arena), std::pmr::null_memory_resource());
    StreamChannel<Frame>::Config cfg;
    cfg.capacity = 256;
    StreamChannel<Frame> ch(cfg, &mr);

    std::uint64_t checksum = 0;
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);

    // Hot loop: push one, drain one, retire one — reusing the pre-allocated ring every cycle.
    for (std::uint64_t i = 0; i < kOps; ++i) {
        Frame f{i, detail::splitmix64(i)};
        if (!ch.try_push(f)) return 1;  // capacity 256 with occupancy 0 each cycle — never full
        StreamBatch<Frame> b(ch, 1);
        const Frame* g = b.next();
        if (g == nullptr) return 1;
        checksum += g->checksum;
        b.retire();
    }

    const std::uint64_t after = g_alloc_count.load(std::memory_order_relaxed);
    const std::uint64_t delta = after - before;

    volatile std::uint64_t sink = checksum;
    (void)sink;

    const bool ok = (delta == 0);
    std::printf("stream_noalloc_test: %s  (ops=%" PRIu64 " hot-path-allocations=%" PRIu64
                " checksum=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kOps, delta, checksum);
    return ok ? 0 : 1;
}
