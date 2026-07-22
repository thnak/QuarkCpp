// Tests 006-Messaging-and-Addressing §tell + 023 §hot-path budgets / ADR-007 F1 — `tell` performs
// 0 heap allocations on the steady hot path. Global operator new/delete are hooked with a counter;
// after warmup (which pre-touches the pools, threads, and registry) the counter is reset and N
// tells are driven + drained. The measured window must show ZERO allocations.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>  // _aligned_malloc/_aligned_free
#endif

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

namespace {
std::atomic<std::size_t> g_allocs{0};

// MSVC's CRT has no std::aligned_alloc (a long-standing gap) and requires the mismatched
// _aligned_malloc/_aligned_free pair instead of malloc/free for over-aligned allocations.
inline void* aligned_alloc_compat(std::size_t alignment, std::size_t size) noexcept {
#if defined(_MSC_VER)
    return ::_aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}
inline void aligned_free_compat(void* p) noexcept {
#if defined(_MSC_VER)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}
}

// --- Hooked allocator (counts every global allocation on any thread) ---------------------------
// GCC's -Wmismatched-new-delete false-positives on replacement operator new/delete implemented via
// malloc/free (it cannot correlate the custom pair); the malloc↔free / aligned_alloc↔free pairing
// below is correct. Suppress only around these definitions.
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
    void* p = aligned_alloc_compat(static_cast<std::size_t>(a), n != 0 ? n : static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { aligned_free_compat(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace quark;

namespace {

struct Ping {
    int n;
};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Ping>;
    std::atomic<long> sum{0};
    std::atomic<int> count{0};
    void handle(const Ping& p) noexcept {
        sum.fetch_add(p.n, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

void drain_until(Sink& s, int target) {
    while (s.count.load(std::memory_order_acquire) < target) { /* spin */ }
}

}  // namespace

int main() {
    bool ok = true;

    detail::MessagePool pool(4096);  // pre-sized cold — no growth during the measured window
    Sink actor;
    auto act = std::make_unique<Activation>(&actor, Sink::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Sink>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Sink> ref = router.get<Sink>(1);
    eng.start();

    // Warmup: prime pool cells, worker futex state, page faults — anything one-time.
    constexpr int kWarm = 256;
    for (int i = 0; i < kWarm; ++i) ref.tell(Ping{1});
    drain_until(actor, kWarm);

    // Measured window: 0 allocations expected.
    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    constexpr int N = 2000;
    for (int i = 0; i < N; ++i) ref.tell(Ping{2});
    drain_until(actor, kWarm + N);
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    eng.stop();

    const std::size_t delta = after - before;
    if (delta != 0) {
        std::fprintf(stderr, "  CHECK FAILED: %zu allocation(s) on the tell hot path\n", delta);
        ok = false;
    }

    std::printf("tell_noalloc_test: %s  (hot-path allocs=%zu, count=%d)\n", ok ? "OK" : "FAIL", delta,
                actor.count.load());
    return ok ? 0 : 1;
}
