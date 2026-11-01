// Tests 006 §Publish/Subscribe / ADR-019 GATE 6 (subscribe/unsubscribe race-free) + GATE 4 under
// concurrency. The load-bearing safety property: after unsubscribe() returns, NO publish still
// references the departing subscriber's inbox (bounded quiescence: active=false, publish new snapshot,
// wait in_flight==0) — so it is safe to DESTROY the inbox. A churn thread subscribes → lets publishes
// land → unsubscribes → drains → DESTROYS its inbox, in a tight loop, while two publisher threads fan
// out continuously. If quiescence were wrong, a publisher would push into a freed inbox → ASan
// heap-use-after-free / TSan data race. At the end, every constructed payload was destroyed exactly
// once (g_ctor == g_dtor) — GATE 4 no-leak / no-double-free.
//
// NEGATIVE CONTROL (this same file, compiled with -DQUARK_TOPIC_NO_QUIESCE — see tests/CMakeLists.txt):
// Topic::unsubscribe() drops the `wait in_flight==0` barrier, so the caller may free an inbox a
// publisher still references. That control used to rely on the race window being hit by timing luck
// across 200k churn rounds; on a large host (Linux g++ ASan) the UAF simply did not trap (issue #28).
// The control now drives a deterministic single round through the test-only hook in
// Topic::publish(): the publisher parks AFTER loading the pre-unsubscribe snapshot, passing the
// active check, and retaining the payload — but BEFORE the inbox push. The churn thread then
// unsubscribes (no wait under the macro) and frees the inbox, and the publisher resumes into freed
// memory. ASan must trap; CTest's WILL_FAIL turns that non-zero exit into a PASS. If the hook never
// arms or no trap happens, the process exits 0 and WILL_FAIL marks the control FAILED — a control
// that cannot fire is a failing signal, not a silent pass.
//
// MACHINE SAFETY: the positive path is exactly 3 worker threads (2 publishers + 1 churn), modest
// iterations. The control path is a single publisher + the main thread, run once. Neither saturates
// the box.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/topic.hpp"

using namespace quark;

namespace {
// Net-live counter: +1 in EVERY constructor, -1 in the destructor. At full quiescence (all threads
// joined, all inboxes drained/destroyed) it must be 0 — every constructed payload was destroyed
// exactly once. A leaked SharedPayload cell leaves it > 0; a double-free traps under ASan first.
std::atomic<std::int64_t> g_live{0};
struct Ev {
    std::uint64_t v = 0;
    Ev() { g_live.fetch_add(1, std::memory_order_relaxed); }
    explicit Ev(std::uint64_t x) : v(x) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev(const Ev& o) : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev(Ev&& o) noexcept : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
    Ev& operator=(const Ev& o) { v = o.v; return *this; }
    Ev& operator=(Ev&& o) noexcept { v = o.v; return *this; }
    ~Ev() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};
#ifndef QUARK_TOPIC_NO_QUIESCE
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
#endif
}  // namespace

#ifdef QUARK_TOPIC_NO_QUIESCE
// The test-provided widen hook topic.hpp declares under this same macro. publish() calls it after it
// has decided to deliver (active check + per-enqueue retain) and before the inbox push. For the one
// inbox under control it signals the churn thread and parks until that inbox has been freed.
namespace quark {
namespace {
std::atomic<const void*> g_control_inbox{nullptr};  // the inbox the churn thread will free
std::atomic<bool> g_publisher_armed{false};         // a publisher is parked inside the window
std::atomic<bool> g_inbox_destroyed{false};         // ...and the inbox is now freed
}  // namespace

void quark_topic_no_quiesce_test_hook(const void* inbox) {
    if (g_control_inbox.load(std::memory_order_acquire) != inbox) return;  // stable subscriber: ignore
    g_publisher_armed.store(true, std::memory_order_release);
    std::uint64_t spins = 0;
    while (!g_inbox_destroyed.load(std::memory_order_acquire)) {
        // Bounded so a broken harness reports "did not fire" (exit 0 => WILL_FAIL FAIL) instead of
        // hanging the suite.
        if (++spins > 4'000'000'000ULL) return;
    }
}
}  // namespace quark

namespace {
constexpr std::uint64_t kControlArmSpins = 4'000'000'000ULL;

// Deterministic firing control (issue #28): see the file banner. Returns 0 on every non-trap path
// ("the control did not fire"), which WILL_FAIL reports as a failure; the only PASS is the ASan trap.
int run_control() {
    Topic<Ev> topic(64);
    const ActorId cid{{7}, 42};
    auto inbox = std::make_unique<BoundedInbox<Ev>>(8);

    topic.subscribe(cid, inbox.get());
    quark::g_control_inbox.store(inbox.get(), std::memory_order_release);

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        std::uint64_t k = 0;
        while (!stop.load(std::memory_order_relaxed)) (void)topic.publish(Ev{k++});
    });

    std::uint64_t spins = 0;
    while (!quark::g_publisher_armed.load(std::memory_order_acquire)) {
        if (++spins > kControlArmSpins) {
            stop.store(true, std::memory_order_relaxed);
            publisher.join();
            std::fprintf(stderr,
                         "topic_no_quiesce_control: publisher never entered the control window "
                         "(control did NOT fire)\n");
            return 0;
        }
    }

    // A publisher is parked in the unprotected window. Quiescence is compiled out, so unsubscribe's
    // barrier is gone and this returns with the publish still in flight; then the inbox is freed.
    topic.unsubscribe(cid);
    { Ev out; while (Topic<Ev>::consume(*inbox, out)) {} }
    inbox.reset();
    quark::g_inbox_destroyed.store(true, std::memory_order_release);
    stop.store(true, std::memory_order_relaxed);

    publisher.join();  // resumes into the freed inbox -> ASan trap (the only PASS)
    std::fprintf(stderr, "topic_no_quiesce_control: no use-after-free trap (control did NOT fire)\n");
    return 0;
}
}  // namespace
#endif

int main() {
#ifdef QUARK_TOPIC_NO_QUIESCE
    return run_control();
#else
    bool ok = true;
    // Positive-path churn budget. (This count no longer affects topic_no_quiesce_control: that
    // target now takes the deterministic single-round path above, so its "must fire" property is
    // independent of timing luck and iteration count — issue #28.) Kept high as a stress test of the
    // real bounded-quiescence barrier: each round is cheap (µs-scale) even under ASan.
    constexpr std::uint64_t kRounds = 200'000;  // churn rounds
    constexpr std::uint32_t kStable = 8;        // stable subscribers so publishers always have work

    Topic<Ev> topic(4096);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> post_unsub_deliveries{0};

    // Stable subscribers (their inboxes outlive the run; drained at the end).
    std::vector<std::unique_ptr<BoundedInbox<Ev>>> stable;
    for (std::uint32_t i = 0; i < kStable; ++i) {
        stable.push_back(std::make_unique<BoundedInbox<Ev>>(256));
        topic.subscribe(ActorId{{1}, i}, stable[i].get());
    }

    // Two publisher threads fan out continuously.
    auto publisher = [&] {
        std::uint64_t k = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            (void)topic.publish(Ev{k++});
            // opportunistically drain the stable inboxes so they don't wedge full (not required for
            // correctness — a full stable inbox just drops).
            for (auto& in : stable) { Ev out; for (int d = 0; d < 4; ++d) if (!Topic<Ev>::consume(*in, out)) break; }
        }
    };
    std::thread p1(publisher), p2(publisher);

    // Churn thread: subscribe -> let publishes land -> unsubscribe -> drain -> DESTROY the inbox.
    std::thread churn([&] {
        const ActorId cid{{7}, 42};
        for (std::uint64_t r = 0; r < kRounds; ++r) {
            auto inbox = std::make_unique<BoundedInbox<Ev>>(64);
            topic.subscribe(cid, inbox.get());
            // let a few publishes target it
            std::this_thread::yield();
            for (int spin = 0; spin < 32; ++spin) { Ev out; if (Topic<Ev>::consume(*inbox, out)) break; }
            // Unsubscribe: after this returns, NO publish may reference `inbox` (bounded quiescence).
            topic.unsubscribe(cid);
            // Drain whatever landed before unsubscribe; then a second drain MUST be empty (no delivery
            // after unsubscribe returns).
            Ev out;
            while (Topic<Ev>::consume(*inbox, out)) {}
            std::uint64_t extra = 0;
            while (Topic<Ev>::consume(*inbox, out)) ++extra;  // must be 0
            post_unsub_deliveries.fetch_add(extra, std::memory_order_relaxed);
            // Destroy the inbox. If a publisher still held a pointer, this is a UAF (ASan/TSan trap).
            inbox.reset();
        }
        stop.store(true, std::memory_order_relaxed);
    });

    churn.join();
    p1.join();
    p2.join();

    // Drain the stable inboxes so their held refs are released before the balance check.
    for (auto& in : stable) { Ev out; while (Topic<Ev>::consume(*in, out)) {} }
    stable.clear();  // destroy stable inboxes (any residual refs released)

    check(post_unsub_deliveries.load() == 0, "GATE 6: no delivery after unsubscribe() returns", ok);
    // Every constructed payload destroyed exactly once — no leak, no double-free (GATE 4). Threads are
    // joined and all inboxes drained/destroyed, so this read is race-free.
    const std::int64_t live = g_live.load();
    check(live == 0, "GATE 4: every payload reclaimed exactly once under churn (0 live at quiescence)", ok);
    check(topic.in_flight() == 0, "no publish in flight at quiescence", ok);

    std::fprintf(stderr, "topic_subscribe_race_test: %s (live=%lld post_unsub=%llu)\n",
                 ok ? "PASS" : "FAIL", static_cast<long long>(live),
                 static_cast<unsigned long long>(post_unsub_deliveries.load()));
    return ok ? 0 : 1;
#endif
}
