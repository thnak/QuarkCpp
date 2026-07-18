// Tests the 016/023 hot-path rule (ADR-016 §"0-alloc — MEASURED"): the tagless wire-encode does
// 0 heap allocation. A global operator new hook counts every allocation; after cold setup we run
// >= 1.2e6 tagless encodes into a pre-provided caller buffer and assert the delta is exactly 0.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "quark/core/serialize.hpp"

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
struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};
QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))
}  // namespace

int main() {
    constexpr std::uint64_t kOps = 1'200'000;

    // Cold setup — the caller buffer is pre-provided once; the hot loop never grows it.
    std::byte buf[64];
    Pod p{0x0102030405060708ULL, 0xaabbccddU, 0x11223344U, 3.14159};
    std::uint64_t checksum = 0;

    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < kOps; ++i) {
        p.id = i;  // vary input so nothing is hoisted
        const std::size_t n = encode_tagless(p, buf);
        checksum += n + std::to_integer<std::uint8_t>(buf[0]);
    }
    const std::uint64_t after = g_alloc_count.load(std::memory_order_relaxed);
    const std::uint64_t delta = after - before;

    volatile std::uint64_t sink = checksum;
    (void)sink;

    const bool pass = (delta == 0);
    std::printf("serialize_noalloc_test: %s  (ops=%llu, hot-path allocations=%llu)\n",
                pass ? "OK" : "FAIL", static_cast<unsigned long long>(kOps),
                static_cast<unsigned long long>(delta));
    return pass ? 0 : 1;
}
