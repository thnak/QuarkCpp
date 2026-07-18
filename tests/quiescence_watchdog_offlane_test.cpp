// Tests 015-Reentrancy-and-Quiescence §Bounded quiescence — the bounded-quiescence WATCHDOG driven
// OFF-LANE, concurrently with carriers and the draining lane. This is the coverage whose absence let
// the audit's Finding 1 (a data race on lane-private ReCore.seal/live when poll_quiesce_watchdog ran
// off-lane) slip through: `quiescence_bounded_test` drives the watchdog single-threaded, so no
// concurrency existed. Here a real lane worker, two carriers (complete_one), and a separate watchdog
// thread all run at once on a MaxConcurrency<8> actor sealed Draining with stuck siblings.
//
// The fix (Finding 1): the off-lane watchdog only raises an atomic signal (kEscalateCancel) + wakes
// the lane; the lane performs the seal→Cancelling + stop_token fire ON-LANE. So this test must (a)
// terminate — the stuck Drain escalates and every sibling unwinds — and (b) be race-free under TSan
// (a data race aborts the process ⇒ CTest fails on the build-tsan config).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/policies.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Stuck {
    int id;
};
struct DrainMsg {
    int tag;
};

struct Service : Actor<Service, MaxConcurrency<8>> {
    using protocol = Protocol<Stuck, DrainMsg>;
    Activation* lane = nullptr;
    std::atomic<int> unwound{0};
    std::atomic<bool> guard_obtained{false};

    // Loop-suspend so the lane worker stays inside drain_step_reentrant reading `seal` / touching
    // `live` for a wide window while sealed Draining — the window the off-lane watchdog raced.
    task<> handle(const Stuck&, const MessageContext& ctx) {
        for (;;) {
            co_await lane->async_suspend();
            if (ctx.stop_requested()) break;  // set once the escalation fires the stop_tokens on-lane
        }
        unwound.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    task<> handle(const DrainMsg&) {
        QuiescenceGuard g = co_await lane->quiesce(QuiesceMode::Drain, /*deadline_ns=*/1);
        guard_obtained.store(true, std::memory_order_release);
        co_return;
    }
};

}  // namespace

int main() {
    Service actor;
    Activation act{&actor, Service::dispatch_table(), {}, max_concurrency_of<Service>()};
    actor.lane = &act;

    constexpr int kStuck = 6;
    std::vector<Stuck> s(kStuck);
    std::vector<Descriptor> ds(kStuck);
    for (int i = 0; i < kStuck; ++i) {
        s[i].id = i;
        ds[i].payload = &s[i];
        stamp<Service, Stuck>(ds[i]);
        act.post(&ds[i]);
    }
    DrainMsg dr{5};
    Descriptor dd;
    dd.payload = &dr;
    stamp<Service, DrainMsg>(dd);
    act.post(&dd);

    // 4 threads total (== the machine core cap): 1 lane worker + 2 carriers + 1 off-lane watchdog.
    std::atomic<bool> run{true};
    std::thread worker([&] {
        const auto t0 = std::chrono::steady_clock::now();
        while (!actor.guard_obtained.load(std::memory_order_acquire)) {
            drive(act);
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) break;  // deadlock guard
        }
        drive(act);
        run.store(false, std::memory_order_release);
    });
    std::vector<std::thread> carriers;
    for (int c = 0; c < 2; ++c)
        carriers.emplace_back([&] {
            while (run.load(std::memory_order_acquire)) act.complete_one();
        });
    std::thread watchdog([&] {
        std::int64_t now = 2;  // > the deadline_ns=1, so every poll is past the bound
        while (run.load(std::memory_order_acquire)) act.poll_quiesce_watchdog(now++);
    });

    worker.join();
    for (auto& t : carriers) t.join();
    watchdog.join();

    bool ok = true;
    check(actor.guard_obtained.load(), "bounded quiescence terminated: the Drain guard resolved", ok);
    check(actor.unwound.load() == kStuck, "every stuck sibling unwound via the escalated cancellation",
          ok);
    check(act.in_flight() == 0, "fully quiescent — in-flight set drained to 0", ok);

    std::printf("quiescence_watchdog_offlane_test: %s  (guard=%d unwound=%d/%d)\n", ok ? "OK" : "FAIL",
                actor.guard_obtained.load(), actor.unwound.load(), kStuck);
    return ok ? 0 : 1;
}
