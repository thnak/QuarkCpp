// Tests 006 §ask (ADR-007 §5 reply routing) — the co_await (on-lane) resume path against the REAL
// multi-worker Engine + 015 admission gate, closing the residual OpenQuestions.md named ("the 015
// OPEN-cell re-admit... clears an ADR-014-grade real-scheduler run" — the SAME `ReplyCell` mechanism
// underlies both an ordinary `ask` and `ask_stream`'s OPEN handshake, see reply_stream.hpp's
// `StreamReplyCell = detail::ReplyCell<Opened>`).
//
// Before this session, `ReplyCell::resolve()`'s co_await path was a raw, un-gated `h.resume()` —
// AND (found while scoping this test) the mechanism it now routes through, `Activation::
// complete_parked()`, had its own lost-wakeup/UB race against `drain_step`'s suspend tail (fixed
// separately, see decisions/ADR-015-...md residual #7 and tests/activation_park_race_test.cpp).
// This test proves the END-TO-END result: many Asker actors each run several SEQUENTIAL
// `co_await ref.ask<int>(...)` cycles against a small pool of Answerer actors, on a 4-worker/
// 4-shard engine, so a large fraction of replies resolve on a DIFFERENT worker's lane than their
// asker's own — exactly the cross-thread shape that exposed the original race. Asserts EXACTLY-ONCE
// resume: every one of N*kAsksPerAsker asks is observed exactly once (a lost wakeup hangs — caught
// by the bounded wait below; a double-resume touches an already-destroyed coroutine frame, UB,
// caught under ASan/a crash) and the tag/value round-trips correctly (no cross-wired replies).
//
// MACHINE SAFETY: 4 workers / 4 shards (this project's stress cap), bounded iteration counts.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
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

struct Query {
    int tag;
};

struct Answerer : Actor<Answerer, Sequential> {
    using protocol = Protocol<Ask<Query, int>>;
    void handle(const Ask<Query, int>& m) noexcept { m.respond(m.query.tag * 2 + 1); }
};

struct Trigger {
    int id;
};

constexpr int kAsksPerAsker = 25;

// Shared, engine-wide config set up BEFORE start() — avoids needing per-instance construction-time
// injection (Engine::spawn<A>() default-constructs A). Every Asker reads the SAME pool of Answerer
// refs and the SAME shared counters; each Asker's own identity travels in the Trigger message itself.
struct Asker : Actor<Asker, Sequential> {
    using protocol = Protocol<Trigger>;
    static inline std::vector<ActorRef<Answerer>>* g_responders = nullptr;
    static inline std::atomic<int>* g_completed = nullptr;
    static inline std::atomic<int>* g_mismatches = nullptr;

    task<> handle(const Trigger& t) {
        ActorRef<Answerer>& target =
            (*g_responders)[static_cast<std::size_t>(t.id) % g_responders->size()];
        for (int i = 0; i < kAsksPerAsker; ++i) {
            const int tag = t.id * 1000 + i;
            result<int> r = co_await target.ask<int>(Query{tag});
            if (r.has_value() && *r == tag * 2 + 1)
                g_completed->fetch_add(1, std::memory_order_relaxed);
            else
                g_mismatches->fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    }
};

}  // namespace

int main() {
    constexpr int kAskers = 100;
    constexpr int kResponders = 4;
    constexpr int kExpected = kAskers * kAsksPerAsker;

    auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(1 << 16);

    for (int i = 0; i < kResponders; ++i) {
        auto id = eng.spawn<Answerer>(static_cast<std::uint64_t>(i));
        check(id.has_value(), "spawn<Answerer>");
    }
    std::vector<ActorId> asker_ids;
    for (int i = 0; i < kAskers; ++i) {
        auto id = eng.spawn<Asker>(static_cast<std::uint64_t>(i));
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
    Asker::g_responders = &responder_refs;
    Asker::g_completed = &completed;
    Asker::g_mismatches = &mismatches;

    for (int i = 0; i < kAskers; ++i) router.tell<Asker>(asker_ids[static_cast<std::size_t>(i)], Trigger{i});

    check(wait_until([&] { return completed.load(std::memory_order_acquire) +
                                   mismatches.load(std::memory_order_acquire) >=
                               kExpected; },
                     std::chrono::seconds(10)),
          "every asker's every ask resolved (no lost wakeup / no hang)");
    check(completed.load(std::memory_order_acquire) == kExpected,
          "every ask resumed with the CORRECT reply, exactly once, no cross-wiring");
    check(mismatches.load(std::memory_order_acquire) == 0, "no mismatched/failed replies");

    eng.stop();

    std::printf("ask_coawait_real_scheduler_test: %s  (completed=%d mismatches=%d expected=%d)\n",
                g_failures ? "FAIL" : "OK", completed.load(), mismatches.load(), kExpected);
    return g_failures ? 1 : 0;
}
