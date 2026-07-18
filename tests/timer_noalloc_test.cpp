// Tests 011-Timers-and-Scheduled-Work §Data structure ("no pointer chasing per tick") + 023 hot-path
// budget — the tick/fire path performs 0 heap allocations. Global operator new/delete are hooked
// with a counter; after warmup (which primes the entry pool, the mailbox pool, the worker lane) the
// counter is reset and a periodic timer is fired N times via the deterministic tick() path. The
// measured window must show ZERO allocations on the wheel tick + fire (`tell`) path.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/timer_service.hpp"

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
using namespace std::chrono_literals;

namespace {

struct Tick {
    int n = 0;
};

// Atomic-only sink: its handler allocates nothing, so any allocation in the measured window is the
// timer/tell path's, not the receiver's.
struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Tick>;
    std::atomic<int> count{0};
    void handle(const Tick&) noexcept { count.fetch_add(1, std::memory_order_release); }
};

void drain_until(Sink& s, int target) {
    while (s.count.load(std::memory_order_acquire) < target) { /* progress spin */ }
}

}  // namespace

int main() {
    bool ok = true;

    detail::MessagePool pool(4096);
    Sink actor;
    auto act = std::make_unique<Activation>(&actor, Sink::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Sink>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Sink> ref = router.get<Sink>(1);
    eng.start();

    // 1 ns tick, period 1 tick ⇒ exactly one fire per tick. Pre-sized entry pool (1 timer).
    TimerService svc(TimerService::Config{1ns, 64});
    auto handle = svc.schedule_every(ref, 1ns, Tick{1});
    (void)handle;

    // Warmup: fire the periodic timer many times to prime the mailbox pool cells, worker futex
    // state, page faults — anything one-time. Each tick fires once (after the first).
    constexpr int kWarm = 256;
    for (int i = 0; i < kWarm + 2; ++i) svc.tick();
    drain_until(actor, kWarm);

    // Measured window: N ticks ⇒ N fires ⇒ N tells. 0 allocations expected on this path.
    const int base = actor.count.load(std::memory_order_acquire);
    const std::size_t before = g_allocs.load(std::memory_order_relaxed);
    constexpr int N = 2000;
    for (int i = 0; i < N; ++i) {
        svc.tick();
        drain_until(actor, base + i + 1);
    }
    const std::size_t after = g_allocs.load(std::memory_order_relaxed);

    eng.stop();

    const std::size_t delta = after - before;
    if (delta != 0) {
        std::fprintf(stderr, "  CHECK FAILED: %zu allocation(s) on the timer tick/fire path\n", delta);
        ok = false;
    }

    std::printf("timer_noalloc_test: %s  (tick/fire allocs=%zu, fires=%d)\n", ok ? "OK" : "FAIL",
                delta, actor.count.load() - base);
    return ok ? 0 : 1;
}
