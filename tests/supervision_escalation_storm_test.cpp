// Tests 007 §Escalation-storm guards (`EscalationGuard`, supervision.hpp) — the residual named in
// ADR-009's post-decision note and OpenQuestions.md ("the runtime storm guards (`escalation_ttl`, a
// per-supervisor `MaxRestarts`/`Within`, 022 rate limiting) are documented residuals"). Unlike
// supervision_escalation_topology_test.cpp (which proves the ROUTING is correct), this proves the
// engine BOUNDS what a supervisor is exposed to:
//   * an aggregate token-bucket cap sheds excess escalations from MANY distinct sources at once
//     (the "systemic fault, many actors fault together" storm shape);
//   * a per-source sliding-window cap sheds a repeat offender's excess WITHOUT blocking a different,
//     well-behaved source in the same window (the "respawn -> immediate refault" storm shape) —
//     simulated here by re-`spawn`-ing the SAME (type, key) `ActorId` after its first instance
//     escalates + locally stops, exactly like a naive respawn-on-Escalated supervisor would;
//   * a TTL stamped on the escalation's descriptor lets a supervisor configured with 022
//     `enable_governance(..., deadline_shed=true)` shed a since-gone-stale escalation instead of
//     acting on it — reusing the EXISTING 018/022 deadline-aware-shedding path, not new machinery;
//   * a zero (default) `EscalationGuard` is unbounded — the pre-existing behavior, unchanged.
// Every shed is counted exactly via `Engine::escalations_shed()` — never a silent drop.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/supervision.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct Boom {};

struct Faulty : Actor<Faulty, Sequential, OnFailure<Escalate>> {
    using protocol = Protocol<Boom>;
    void handle(const Boom&) { throw std::runtime_error("boom"); }
};

struct Watcher : Actor<Watcher, Sequential> {
    using protocol = Protocol<Escalated>;
    static inline std::atomic<int> received{0};
    void handle(const Escalated&) { received.fetch_add(1, std::memory_order_relaxed); }
};

}  // namespace

// --- 1. Aggregate cap: many DISTINCT sources faulting at once is bounded, excess shed exactly ----
static void test_aggregate_cap() {
    const int before = g_failures;
    Watcher::received = 0;
    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    check(built.has_value(), "aggregate: ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(256);

    auto watcher_id = eng.spawn<Watcher>(1);
    check(watcher_id.has_value(), "aggregate: spawn<Watcher>");
    // Negligible refill (0.001/s) so the burst cap is the only thing that matters over a test's
    // real wall-clock duration; burst = 3 admits exactly 3 of the 10 fired below.
    eng.set_node_supervisor<Watcher>(*watcher_id, EscalationGuard{.max_per_sec = 0.001, .burst = 3.0});

    constexpr int kFaulty = 10;
    std::vector<ActorId> faulty_ids;
    for (int i = 0; i < kFaulty; ++i) {
        auto id = eng.spawn<Faulty>(static_cast<std::uint64_t>(100 + i));
        check(id.has_value(), "aggregate: spawn<Faulty>");
        faulty_ids.push_back(*id);
    }

    eng.start();
    LocalRouter router(eng.post_courier(), pool);
    for (ActorId id : faulty_ids) router.tell<Faulty>(id, Boom{});

    check(wait_until([&] { return Watcher::received.load() + static_cast<int>(eng.escalations_shed()) >=
                                    kFaulty; },
                     std::chrono::seconds(2)),
          "aggregate: every fault resolved to either 'received' or 'shed' (exact accounting)");
    check(Watcher::received.load(std::memory_order_acquire) == 3,
          "aggregate: exactly burst=3 escalations admitted, the rest shed");
    check(eng.escalations_shed() == static_cast<std::uint64_t>(kFaulty) - 3,
          "aggregate: escalations_shed() accounts for exactly the excess (7)");

    eng.stop();
    std::printf("  test_aggregate_cap: %s\n", g_failures == before ? "OK" : "FAIL");
}

// --- 2. Per-source cap: a repeat source (simulated respawn) is bounded independently of another --
static void test_per_source_cap() {
    const int before = g_failures;
    Watcher::received = 0;
    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    check(built.has_value(), "per_source: ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(256);

    auto watcher_id = eng.spawn<Watcher>(1);
    check(watcher_id.has_value(), "per_source: spawn<Watcher>");
    // Aggregate unbounded (max_per_sec == 0); only the per-source axis is under test.
    eng.set_node_supervisor<Watcher>(
        *watcher_id, EscalationGuard{.max_per_source = 1, .per_source_window_ns = 60'000'000'000});

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    // Source A, instance 1: faults + escalates (admitted: 1st for this ActorId) + locally stops.
    auto a1 = eng.spawn<Faulty>(/*key*/ 7);
    check(a1.has_value(), "per_source: spawn<Faulty>(7) #1");
    router.tell<Faulty>(*a1, Boom{});
    check(wait_until([&] { return Watcher::received.load() >= 1; }, std::chrono::seconds(2)),
          "per_source: source A's first escalation is admitted");

    // Source A, instance 2: SAME (type, key) ActorId, a fresh Activation — exactly what a naive
    // respawn-on-Escalated supervisor would produce. Same source, 2nd escalation within the window
    // -> shed by max_per_source=1.
    auto a2 = eng.spawn<Faulty>(/*key*/ 7);
    check(a2.has_value(), "per_source: spawn<Faulty>(7) #2 (respawn, same ActorId)");
    check(a2->key == a1->key && a2->type == a1->type, "per_source: respawn reuses the SAME ActorId");
    router.tell<Faulty>(*a2, Boom{});
    check(wait_until([&] { return eng.escalations_shed() >= 1; }, std::chrono::seconds(2)),
          "per_source: source A's second escalation (same ActorId) is shed");

    // Source B: a DIFFERENT ActorId, first escalation ever -> admitted, proving the cap is scoped
    // PER SOURCE, not a shared/aggregate counter that A's exhaustion would poison.
    auto b1 = eng.spawn<Faulty>(/*key*/ 8);
    check(b1.has_value(), "per_source: spawn<Faulty>(8)");
    router.tell<Faulty>(*b1, Boom{});
    check(wait_until([&] { return Watcher::received.load() >= 2; }, std::chrono::seconds(2)),
          "per_source: a DIFFERENT source's escalation is admitted independently of A's cap");

    check(Watcher::received.load(std::memory_order_acquire) == 2, "per_source: received == 2 (A#1, B#1)");
    check(eng.escalations_shed() == 1, "per_source: escalations_shed() == 1 (A#2 only)");

    eng.stop();
    std::printf("  test_per_source_cap: %s\n", g_failures == before ? "OK" : "FAIL");
}

// --- 3. TTL: a stale escalation is shed by the EXISTING 018/022 deadline-shed path, reused --------
static void test_ttl_shed() {
    const int before = g_failures;
    Watcher::received = 0;
    auto built = ConfigBuilder{}.workers(1).shards(1).default_drain_budget(64).build();
    check(built.has_value(), "ttl: ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(256);

    auto watcher_id = eng.spawn<Watcher>(1);
    check(watcher_id.has_value(), "ttl: spawn<Watcher>");
    // A TTL of 0ns budget effectively means "already expired the instant it's stamped" once even a
    // few microseconds elapse before the (single, pinned) worker gets to drain it — reliable without
    // a sleep. Aggregate/per-source axes left unbounded so only the TTL axis is under test.
    eng.set_node_supervisor<Watcher>(*watcher_id, EscalationGuard{.ttl_ns = 1});
    // 022: arm deadline-aware shedding on the supervisor, threshold 0 == always shed a doomed
    // message (activation.hpp's drain_step_governed_seq, reused verbatim — no new shedding logic).
    Schedulable* watcher_sched = eng.resolve(*watcher_id);
    check(watcher_sched != nullptr, "ttl: engine.resolve(watcher_id) finds the spawned supervisor");
    eng.enable_governance(watcher_sched, /*deadline_shed=*/true, /*shed_threshold=*/0);

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    auto f1 = eng.spawn<Faulty>(/*key*/ 20);
    check(f1.has_value(), "ttl: spawn<Faulty>");
    router.tell<Faulty>(*f1, Boom{});

    // The TTL shed happens at DRAIN time via the EXISTING 022 deadline-shed path
    // (`drain_step_governed_seq`'s `g.note_shed()`), so it lands in the Activation's own
    // `governance_sheds()` counter — distinct from `Engine::escalations_shed()`, which only counts
    // an EscalationGuard admission-time rejection (aggregate/per-source), never fired here.
    check(wait_until([&] { return watcher_sched->activation->governance_sheds() >= 1; },
                     std::chrono::seconds(2)),
          "ttl: a TTL-expired escalation is shed by the reused 018/022 deadline-shed path");
    check(Watcher::received.load(std::memory_order_acquire) == 0,
          "ttl: the stale escalation was never dispatched to the supervisor's handler");
    check(eng.escalations_shed() == 0,
          "ttl: Engine::escalations_shed() stays 0 (this shed is the 022 drain-time path, not the "
          "EscalationGuard admission path)");

    eng.stop();
    std::printf("  test_ttl_shed: %s\n", g_failures == before ? "OK" : "FAIL");
}

// --- 4. Zero guard (default): unbounded, byte-for-byte the pre-existing behavior ------------------
static void test_zero_guard_is_unbounded() {
    const int before = g_failures;
    Watcher::received = 0;
    auto built = ConfigBuilder{}.workers(2).shards(2).default_drain_budget(64).build();
    check(built.has_value(), "zero_guard: ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(256);

    auto watcher_id = eng.spawn<Watcher>(1);
    check(watcher_id.has_value(), "zero_guard: spawn<Watcher>");
    eng.set_node_supervisor<Watcher>(*watcher_id);  // no guard argument -> EscalationGuard{} default

    constexpr int kFaulty = 20;
    std::vector<ActorId> faulty_ids;
    for (int i = 0; i < kFaulty; ++i) {
        auto id = eng.spawn<Faulty>(static_cast<std::uint64_t>(200 + i));
        check(id.has_value(), "zero_guard: spawn<Faulty>");
        faulty_ids.push_back(*id);
    }

    eng.start();
    LocalRouter router(eng.post_courier(), pool);
    for (ActorId id : faulty_ids) router.tell<Faulty>(id, Boom{});

    check(wait_until([&] { return Watcher::received.load() >= kFaulty; }, std::chrono::seconds(2)),
          "zero_guard: every one of 20 escalations is admitted (unbounded, unchanged default)");
    check(eng.escalations_shed() == 0, "zero_guard: escalations_shed() stays 0");

    eng.stop();
    std::printf("  test_zero_guard_is_unbounded: %s\n", g_failures == before ? "OK" : "FAIL");
}

int main() {
    test_aggregate_cap();
    test_per_source_cap();
    test_ttl_shed();
    test_zero_guard_is_unbounded();

    std::printf("supervision_escalation_storm_test: %s (failures=%d)\n",
                g_failures ? "FAIL" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
