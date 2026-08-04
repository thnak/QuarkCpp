// Tests 006 §ask_stream (ADR-018) -- ADR-018's own named tie-breaking experiment ("Tie-breaking
// experiment" section): the OPEN handshake's co_await (on-lane, Activation::complete_parked()) resume
// proven END-TO-END through a REAL actor handling AskStream<Q,F> via `.accept()`, racing the ring's
// FIRST pushed item, on a real multi-worker Engine. StreamReplyCell = detail::ReplyCell<Opened> is the
// same template `ask`'s OPEN cell uses (proven generically in tests/ask_coawait_real_scheduler_test.cpp)
// -- what THAT proof did not cover, and what ADR-018 explicitly still names as the 006-outbound
// promotion blocker, is the ask_stream-SPECIFIC dispatch/addressing path (ActorRef::ask_stream /
// LocalRouter::ask_stream, wired this session) and the OPEN-vs-first-item race.
//
// REENTRANCY NOTE (found while writing this test, see ADR-015/ADR-018 residuals): `ReplyCell::
// resolve()`'s co_await path runs `Activation::complete_parked()` -- hence the ENTIRE resumed
// continuation -- SYNCHRONOUSLY, inline, on whichever thread calls `resolve()`. For `StreamResponder::
// accept()`, that thread is the CALLEE's own worker, and `accept()` does not return to the callee's
// handler until that inline resume finishes. So a callee handler must never busy-wait, inside the same
// call, for progress that its OWN later statements (e.g. the item pushes right after `accept()`) would
// produce -- the resumed caller's continuation can run to completion (or its next suspension) BEFORE
// the callee gets to push anything. This is why the Puller below hands the opened `ReplyStream<Row>`
// off to a plain drain-thread pool instead of draining synchronously inside its own coroutine turn:
// draining is decoupled from any actor's call stack, so it correctly races the Streamer's in-progress
// pushes across real threads (the same shape reply_stream_concurrency_test.cpp already proves for the
// ring itself) while the co_await/dispatch/complete_parked() path is still exercised for real.
//
// Streamer::handle(const AskStream<Query,Row>&) accepts and pushes items back-to-back with NO delay,
// so the drain threads genuinely race the producer's pushes. Asserts: exactly-once OPEN resume (no
// lost wakeup / no hang), every pushed item delivered in FIFO order with 0 loss/0 torn payload, clean
// Closed termination, no gap.
//
// MACHINE SAFETY: 4 engine workers / 4 shards + 4 plain drain threads (this project's stress cap),
// bounded iteration counts.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <condition_variable>
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
struct Row {
    std::uint64_t id;
    std::uint64_t check;
};

constexpr int kItemsPerStream = 8;
constexpr std::uint64_t kMix = 2654435761u;

struct Streamer : Actor<Streamer, Sequential> {
    using protocol = Protocol<AskStream<Query, Row>>;
    void handle(const AskStream<Query, Row>& m) noexcept {
        auto producer = m.respond.accept();
        const std::uint64_t base = static_cast<std::uint64_t>(m.query.tag) * 1000;
        for (std::uint64_t i = 0; i < kItemsPerStream; ++i) {
            const std::uint64_t id = base + i;
            // Default ring capacity (256) always holds kItemsPerStream items -- try_push cannot stall.
            (void)producer.try_push(Row{id, id ^ kMix});
        }
        producer.close();
    }
};

struct Trigger {
    int id;
};

constexpr int kStreamsPerPuller = 25;

// A plain thread-safe handoff queue: opened streams cross from an actor's coroutine turn onto a
// plain OS thread pool for draining (see the REENTRANCY NOTE above for why draining cannot happen
// inline inside the Puller's own coroutine turn).
struct PendingDrain {
    int tag;
    ReplyStream<Row> rs;
};

class DrainQueue {
public:
    void push(PendingDrain pd) {
        {
            std::lock_guard<std::mutex> g(mu_);
            q_.push_back(std::move(pd));
        }
        cv_.notify_one();
    }
    [[nodiscard]] bool pop(PendingDrain& out) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return !q_.empty() || stopped_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<PendingDrain> q_;
    bool stopped_ = false;
};

// Shared, engine-wide config set up BEFORE start() -- mirrors ask_coawait_real_scheduler_test.cpp.
struct Puller : Actor<Puller, Sequential> {
    using protocol = Protocol<Trigger>;
    static inline std::vector<ActorRef<Streamer>>* g_streamers = nullptr;
    static inline std::atomic<int>* g_mismatches = nullptr;
    static inline DrainQueue* g_queue = nullptr;

    task<> handle(const Trigger& t) {
        ActorRef<Streamer>& target =
            (*g_streamers)[static_cast<std::size_t>(t.id) % g_streamers->size()];
        for (int i = 0; i < kStreamsPerPuller; ++i) {
            const int tag = t.id * 1000 + i;
            result<ReplyStream<Row>> rs = co_await target.ask_stream<Row>(Query{tag});
            if (!rs.has_value()) {
                g_mismatches->fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            g_queue->push(PendingDrain{tag, std::move(*rs)});
        }
        co_return;
    }
};

void drain_worker(DrainQueue& q, std::atomic<int>& completed, std::atomic<int>& mismatches) {
    PendingDrain pd;
    while (q.pop(pd)) {
        const std::uint64_t base = static_cast<std::uint64_t>(pd.tag) * 1000;
        std::uint64_t expect = base;
        std::uint64_t received = 0;
        bool ok = true;
        int spins = 0;
        for (;;) {
            if (auto item = pd.rs.next()) {
                if (item->id != expect || item->check != (item->id ^ kMix)) ok = false;
                ++expect;
                ++received;
                continue;
            }
            if (pd.rs.done()) break;
            if (++spins > 2'000'000) {  // bounded -- no infinite spin (MACHINE SAFETY)
                ok = false;
                break;
            }
            std::this_thread::yield();
        }
        if (ok && received == kItemsPerStream && pd.rs.terminal() == ReplyStreamTerminal::Closed &&
            !pd.rs.gap_detected())
            completed.fetch_add(1, std::memory_order_relaxed);
        else
            mismatches.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

int main() {
    constexpr int kPullers = 100;
    constexpr int kStreamers = 4;
    constexpr int kDrainThreads = 4;
    constexpr int kExpected = kPullers * kStreamsPerPuller;

    auto built = ConfigBuilder{}.workers(4).shards(4).default_drain_budget(64).build();
    check(built.has_value(), "ConfigBuilder produces a valid EngineConfig");
    Engine<> eng(*built);
    detail::MessagePool pool(1 << 16);

    for (int i = 0; i < kStreamers; ++i) {
        auto id = eng.spawn<Streamer>(static_cast<std::uint64_t>(i));
        check(id.has_value(), "spawn<Streamer>");
    }
    std::vector<ActorId> puller_ids;
    for (int i = 0; i < kPullers; ++i) {
        auto id = eng.spawn<Puller>(static_cast<std::uint64_t>(i));
        check(id.has_value(), "spawn<Puller>");
        puller_ids.push_back(*id);
    }

    eng.start();
    LocalRouter router(eng.post_courier(), pool);

    std::vector<ActorRef<Streamer>> streamer_refs;
    for (int i = 0; i < kStreamers; ++i)
        streamer_refs.push_back(router.get<Streamer>(static_cast<std::uint64_t>(i)));
    std::atomic<int> completed{0};
    std::atomic<int> mismatches{0};
    DrainQueue queue;
    Puller::g_streamers = &streamer_refs;
    Puller::g_mismatches = &mismatches;
    Puller::g_queue = &queue;

    std::vector<std::thread> drain_threads;
    for (int i = 0; i < kDrainThreads; ++i)
        drain_threads.emplace_back(drain_worker, std::ref(queue), std::ref(completed), std::ref(mismatches));

    for (int i = 0; i < kPullers; ++i)
        router.tell<Puller>(puller_ids[static_cast<std::size_t>(i)], Trigger{i});

    check(wait_until([&] { return completed.load(std::memory_order_acquire) +
                                   mismatches.load(std::memory_order_acquire) >=
                               kExpected; },
                     std::chrono::seconds(15)),
          "every puller's every ask_stream resolved and drained (no lost wakeup / no hang)");
    check(completed.load(std::memory_order_acquire) == kExpected,
          "every ask_stream drained exactly its pushed items, in order, no loss, Closed, no gap");
    check(mismatches.load(std::memory_order_acquire) == 0, "no failed/mismatched streams");

    queue.stop();
    for (auto& th : drain_threads) th.join();

    eng.stop();

    std::printf("ask_stream_coawait_real_scheduler_test: %s  (completed=%d mismatches=%d expected=%d)\n",
                g_failures ? "FAIL" : "OK", completed.load(), mismatches.load(), kExpected);
    return g_failures ? 1 : 0;
}
