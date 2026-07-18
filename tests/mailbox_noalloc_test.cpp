// Tests the 003/023 hot-path rule: 0 heap allocation on enqueue/drain. A global operator new
// hook counts every allocation; after cold setup we run >= 1e6 enqueue->dequeue->release cycles
// and assert the allocation delta is exactly 0 (measured, not asserted-in-comment — CONVENTIONS).
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"

// --- Global allocation counter -------------------------------------------------------------
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

int main() {
    constexpr std::uint64_t kOps = 1'000'000;

    // Cold setup (allocations here are allowed — pool pre-allocates its block up front).
    DescriptorPool pool(64);
    Mailbox mb;
    Descriptor* d = pool.acquire();

    std::uint64_t checksum = 0;
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);

    // Hot loop: enqueue -> drain -> release -> re-acquire, reusing pooled descriptors. This
    // exercises the steady enqueue/dequeue path AND the empty-boundary stub re-arm every cycle.
    for (std::uint64_t i = 0; i < kOps; ++i) {
        d->message_id = MessageId{i};
        mb.enqueue(d);
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) {
            std::fprintf(stderr, "unexpected drain status %d at op %" PRIu64 "\n",
                         static_cast<int>(r.status), i);
            return 1;
        }
        checksum += r.desc->message_id.value;
        pool.release(r.desc);
        d = pool.acquire();
    }

    const std::uint64_t after = g_alloc_count.load(std::memory_order_relaxed);
    const std::uint64_t delta = after - before;

    // Prevent the loop from being optimized away.
    volatile std::uint64_t sink = checksum;
    (void)sink;

    const bool ok = (delta == 0);
    std::printf("mailbox_noalloc_test: %s  (ops=%" PRIu64 ", hot-path allocations=%" PRIu64
                ", checksum=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kOps, delta, checksum);
    return ok ? 0 : 1;
}
