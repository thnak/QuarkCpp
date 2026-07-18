// Tests 020-Security §"security is a boundary concern" — the zero-cost-when-unused rule: a single-node,
// no-ingress, no-cluster deployment pays NOTHING for security. The ambient Principal that rides the
// MessageContext is a trivially-copyable 16-byte value defaulting to ANONYMOUS; the attenuation algebra
// is pure constexpr arithmetic; and exercising the whole intra-node principal path over >=1e6 cycles
// does 0 heap allocation. This mirrors governance_zero_cost_test's allocator-probe method (and, like it,
// replaces global operator new, so the CMake content rule auto-excludes it from the TSan build).
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

#include "quark/core/message_context.hpp"
#include "quark/core/principal.hpp"
#include "quark/core/security.hpp"

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
// The ambient identity is a small, trivially-copyable value — no heap, no vtable, no cost when default.
static_assert(std::is_trivially_copyable_v<Principal>, "Principal must be trivially copyable");
static_assert(std::is_trivially_destructible_v<Principal>, "Principal must be trivially destructible");
static_assert(sizeof(Principal) == 16, "Principal is two u64s (subject, rights) — 16 bytes");

// The default (anonymous) principal carries no authority — a purely intra-process tell leaves it so.
static_assert(Principal{}.anonymous(), "default Principal is anonymous");
// Attenuation is constexpr — evaluable with zero runtime cost / zero allocation.
static_assert(attenuate(Principal{0, 0b110}, Principal{0, 0b111}).rights == 0b110,
              "constexpr attenuate intersects rights (no amplification)");
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kOps = 1'000'000;

    // Exercise the full intra-node ambient-principal path: publish an ambient context, stamp an outbound
    // principal from it (attenuate), check domination — the operations a secured local hop would run.
    // None of them allocate.
    volatile std::uint64_t sink = 0;
    const std::uint64_t before = g_alloc_count.load(std::memory_order_relaxed);
    for (std::uint64_t i = 0; i < kOps; ++i) {
        MessageContext ctx{};
        ctx.principal = Principal{i, i & 0b1111};  // some inbound authority
        detail::AmbientContextScope amb(ctx);

        const MessageContext* a = detail::tl_current_ctx;
        const Principal inbound = a ? a->principal : Principal{};
        const Principal outbound = attenuate(inbound, Principal{i, i & 0b0101});  // delegate a subset
        sink += outbound.rights + (dominates(inbound, outbound) ? 1u : 0u);
    }
    const std::uint64_t delta = g_alloc_count.load(std::memory_order_relaxed) - before;
    (void)sink;

    const bool pass = (delta == 0);
    ok = ok && pass;
    std::printf("  intra-node principal path: %s (ops=%" PRIu64 ", hot allocations=%" PRIu64 ")\n",
                pass ? "OK" : "FAIL", kOps, delta);

    std::printf("security_zero_cost_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
