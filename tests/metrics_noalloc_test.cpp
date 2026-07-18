// Tests 009-Observability §Metrics + CONVENTIONS.md hot-path rule — the metrics/tracing/dead-letter
// RECORD path performs 0 heap allocations. Global operator new/delete are hooked with a counter;
// after warmup the counter is reset and the record paths are hammered. The measured window must
// show ZERO allocations (all state is fixed-size and pre-constructed cold).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "quark/core/dead_letter.hpp"
#include "quark/core/metrics.hpp"
#include "quark/core/tracing.hpp"

namespace {
std::atomic<std::size_t> g_allocs{0};
}

// --- Hooked allocator (counts every global allocation on any thread) ---------------------------
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
    void* p = std::aligned_alloc(static_cast<std::size_t>(a),
                                 n != 0 ? n : static_cast<std::size_t>(a));
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

int main() {
    bool ok = true;

    // Cold setup: fixed-size, pre-constructed. The DeadLetterRegistry pre-sizes its ring here.
    ShardCounters sc;
    SpanRing<256> ring;
    DeadLetterRegistry dlq(64);
    Descriptor d;  // a stand-in descriptor for the dead-letter record path
    d.trace_id = make_engine_trace_id(0x1234, /*sampled=*/true);
    d.reserved = 7;

    // Warmup: touch every code path once so any one-time lazy init is paid before measuring.
    for (int i = 0; i < 256; ++i) {
        sc.messages_processed.inc();
        sc.message_latency_ns.record(static_cast<std::uint64_t>(i + 1));
        SpanEvent e;
        e.trace_id = d.trace_id;
        ring.append(e);
        dlq.record(&d, error{errc::supervised_stop, "handler_fault"}, i);
    }

    // Measured window: 0 allocations expected on the record paths.
    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    constexpr int N = 100'000;
    for (int i = 0; i < N; ++i) {
        sc.messages_processed.inc();
        sc.mailbox_enqueued.inc();
        sc.dead_letters.inc();
        sc.user[3].inc();
        sc.message_latency_ns.record(static_cast<std::uint64_t>((i % 1024) + 1));
        sc.mailbox_depth.record(static_cast<std::uint64_t>(i % 64));
        SpanEvent e;
        e.trace_id = d.trace_id;
        e.msg_slot = 3;
        e.outcome = SpanOutcome::Ok;
        ring.append(e);
        dlq.record(&d, error{errc::supervised_stop, "handler_fault"}, i);
    }
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    const std::size_t delta = after - before;
    if (delta != 0) {
        std::fprintf(stderr, "  CHECK FAILED: %zu allocation(s) on the metrics record path\n", delta);
        ok = false;
    }

    // Sanity: the paths actually ran (so the compiler didn't elide them).
    if (sc.messages_processed.load() < static_cast<std::uint64_t>(N)) ok = false;
    if (ring.total_recorded() < static_cast<std::uint64_t>(N)) ok = false;
    if (dlq.total() < static_cast<std::uint64_t>(N)) ok = false;

    std::printf("metrics_noalloc_test: %s  (record-path allocs=%zu)\n", ok ? "OK" : "FAIL", delta);
    return ok ? 0 : 1;
}
