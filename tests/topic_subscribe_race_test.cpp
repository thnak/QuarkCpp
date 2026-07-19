// Tests 006 §Publish/Subscribe / ADR-019 GATE 6 (subscribe/unsubscribe race-free) + GATE 4 under
// concurrency. The load-bearing safety property: after unsubscribe() returns, NO publish still
// references the departing subscriber's inbox (bounded quiescence: active=false, publish new snapshot,
// wait in_flight==0) — so it is safe to DESTROY the inbox. A churn thread subscribes → lets publishes
// land → unsubscribes → drains → DESTROYS its inbox, in a tight loop, while two publisher threads fan
// out continuously. If quiescence were wrong, a publisher would push into a freed inbox → ASan
// heap-use-after-free / TSan data race. At the end, every constructed payload was destroyed exactly
// once (g_ctor == g_dtor) — GATE 4 no-leak / no-double-free.
//
// MACHINE SAFETY: exactly 3 worker threads (2 publishers + 1 churn), modest iterations. A valid
// first-class TSan/ASan test (it is correct — it never wedges). Never saturates the box.
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
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kRounds = 20'000;   // churn rounds
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
}
