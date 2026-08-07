// Tests ADR-047 — quark::task<T> for non-void T: a genuinely awaitable, nested, value-returning
// coroutine that a handler's own task<void> frame (or another task<T>) co_awaits to get a T back.
//
// Three invariants proven by the design-debate-prove workflow that produced ADR-047, re-verified
// here against the real repo build (the workflow's own proof ran in a scratch dir under MSVC only):
//
//   1. Cross-lane resume routing (the ADR's load-bearing fix). A nested task<int> that itself
//      co_awaits a real ask() genuinely parks on a DIFFERENT worker's lane than its Asker's own —
//      exactly the shape that exposed the pre-ADR-047 bug (Activation::complete_parked() /
//      ParkedResumeSink unconditionally resuming the top-level task<void> handle instead of the
//      leaf handle ReplyCell::suspend() actually captured). Mirrors
//      ask_coawait_real_scheduler_test.cpp's harness, with the ask routed THROUGH a nested task<T>
//      instead of awaited directly.
//   2. A throw inside a nested task<T> is contained (never std::terminate) and observed, as an
//      ordinary rethrown C++ exception, at the awaiting task<void> handler.
//   3. Dropping a task<T> that is never awaited never runs its body and never leaks the frame.
//
// MACHINE SAFETY: 4 workers / 4 shards (this project's stress cap), bounded iteration counts.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/task.hpp"
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

// --- Claim 3: drop-unawaited safety — no Engine needed at all. ----------------------------------
bool g_body_entered = false;
task<int> never_awaited_body() {
    g_body_entered = true;  // would only run if the frame were ever resumed
    co_return 7;
}

void test_drop_unawaited() {
    g_body_entered = false;
    {
        task<int> t = never_awaited_body();
        check(t.valid(), "never_awaited_body() returns a valid (suspended-at-initial) task<int>");
        // t destructs here without ever being co_await'd.
    }
    check(!g_body_entered, "dropping an unawaited task<int> never runs its body");
}

// --- Claims 1 + 2: real Engine, cross-lane nested task<T>. --------------------------------------
struct Query {
    int tag;
};

struct Answerer : Actor<Answerer, Sequential> {
    using protocol = Protocol<Ask<Query, int>>;
    void handle(const Ask<Query, int>& m) noexcept { m.respond(m.query.tag * 2 + 1); }
};

// Nested task<int>: co_awaits a real cross-lane ask() itself, so ReplyCell::suspend() captures
// THIS frame's handle, not the outer Asker::handle task<void> frame's — exactly the case ADR-047's
// leaf-threading fix targets. Throws for a tag ending in 9 (Claim 2's exception-containment path).
task<int> nested_ask(ActorRef<Answerer> target, int tag) {
    result<int> r = co_await target.ask<int>(Query{tag});
    if (!r.has_value()) co_return -1;
    if (tag % 10 == 9) throw std::runtime_error("nested_ask: injected fault");
    co_return *r;
}

struct Trigger {
    int id;
};

constexpr int kAsksPerAsker = 25;

struct Asker : Actor<Asker, Sequential> {
    using protocol = Protocol<Trigger>;
    static inline std::vector<ActorRef<Answerer>>* g_responders = nullptr;
    static inline std::atomic<int>* g_completed = nullptr;
    static inline std::atomic<int>* g_mismatches = nullptr;
    static inline std::atomic<int>* g_faults_caught = nullptr;

    task<> handle(const Trigger& t) {
        ActorRef<Answerer>& target =
            (*g_responders)[static_cast<std::size_t>(t.id) % g_responders->size()];
        for (int i = 0; i < kAsksPerAsker; ++i) {
            const int tag = t.id * 1000 + i;
            try {
                // co_await a NESTED task<int> (not the ask directly) — the inner frame is the one
                // that actually suspends on the cross-lane ReplyCell.
                int v = co_await nested_ask(target, tag);
                if (v == tag * 2 + 1)
                    g_completed->fetch_add(1, std::memory_order_relaxed);
                else
                    g_mismatches->fetch_add(1, std::memory_order_relaxed);
            } catch (const std::runtime_error&) {
                // Claim 2: the throw inside nested_ask() propagated here as an ordinary C++
                // exception — contained, never std::terminate, and correctly attributed to the
                // right (tag % 10 == 9) iteration.
                check(tag % 10 == 9, "exception surfaced on exactly the injected-fault tag");
                g_faults_caught->fetch_add(1, std::memory_order_relaxed);
            }
        }
        co_return;
    }
};

void test_cross_lane_nested_task() {
    constexpr int kAskers = 100;
    constexpr int kResponders = 4;
    constexpr int kExpectedTotal = kAskers * kAsksPerAsker;
    // i in [0, kAsksPerAsker) with i % 10 == 9 (tag % 10 == i % 10 since t.id*1000 is a multiple
    // of 10) — for kAsksPerAsker == 25 that is i == 9 and i == 19, i.e. 2 per asker.
    constexpr int kFaultsPerAsker = (kAsksPerAsker + 0) / 10;
    constexpr int kExpectedFaults = kAskers * kFaultsPerAsker;

    auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(1 << 16);

    for (int i = 0; i < kResponders; ++i) {
        auto id = eng.spawn<Answerer>(static_cast<std::uint64_t>(i), pool.sink());
        check(id.has_value(), "spawn<Answerer>");
    }
    std::vector<ActorId> asker_ids;
    for (int i = 0; i < kAskers; ++i) {
        auto id = eng.spawn<Asker>(static_cast<std::uint64_t>(i), pool.sink());
        check(id.has_value(), "spawn<Asker>");
        asker_ids.push_back(*id);
    }

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    std::vector<ActorRef<Answerer>> responder_refs;
    for (int i = 0; i < kResponders; ++i)
        responder_refs.push_back(router.get<Answerer>(static_cast<std::uint64_t>(i)));
    std::atomic<int> completed{0};
    std::atomic<int> mismatches{0};
    std::atomic<int> faults_caught{0};
    Asker::g_responders = &responder_refs;
    Asker::g_completed = &completed;
    Asker::g_mismatches = &mismatches;
    Asker::g_faults_caught = &faults_caught;

    for (int i = 0; i < kAskers; ++i) router.tell<Asker>(asker_ids[static_cast<std::size_t>(i)], Trigger{i});

    const int kExpectedOk = kExpectedTotal - kExpectedFaults;
    check(wait_until([&] {
              return completed.load(std::memory_order_acquire) +
                         mismatches.load(std::memory_order_acquire) +
                         faults_caught.load(std::memory_order_acquire) >=
                     kExpectedTotal;
          }, std::chrono::seconds(10)),
          "every asker's every nested-task<T> ask resolved (no lost wakeup / no hang)");
    check(completed.load(std::memory_order_acquire) == kExpectedOk,
          "every non-faulting nested task<T> resumed with the CORRECT value, exactly once");
    check(mismatches.load(std::memory_order_acquire) == 0, "no mismatched/cross-wired replies");
    check(faults_caught.load(std::memory_order_acquire) == kExpectedFaults,
          "every injected fault surfaced exactly once, contained, never lost");

    eng.stop();

    std::printf(
        "task_value_return_test[cross_lane]: %s  (completed=%d mismatches=%d faults=%d "
        "expected_ok=%d expected_faults=%d)\n",
        (completed.load() == kExpectedOk && mismatches.load() == 0 &&
         faults_caught.load() == kExpectedFaults)
            ? "OK"
            : "FAIL",
        completed.load(), mismatches.load(), faults_caught.load(), kExpectedOk, kExpectedFaults);
}

}  // namespace

int main() {
    test_drop_unawaited();
    test_cross_lane_nested_task();

    std::printf("task_value_return_test: %s\n", g_failures ? "FAIL" : "OK");
    return g_failures ? 1 : 0;
}
