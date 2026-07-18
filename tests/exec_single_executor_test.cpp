// Tests 001-Actor-Execution-Model §Activation invariants under real contention — the single-executor
// protocol WITHOUT the full 002 scheduler. A minimal in-test harness spins <= 4 std::jthread workers
// (machine-safety cap) that each loop {try-acquire Scheduled->Running, drain_step, close_out} against
// ONE shared Activation fed by a producer. Asserts:
//   * a concurrent-executor counter NEVER exceeds 1 (single-executor — the exec-state CAS);
//   * every message is dispatched EXACTLY once;
//   * per-actor FIFO is preserved (dispatch order == enqueue order).
// The handler mutates PLAIN (non-atomic) actor fields — the exec-state release/acquire handoff is
// the only thing making that safe across worker handoffs, so TSan is load-bearing here.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {
constexpr unsigned kWorkers = 3;         // 3 workers + 1 producer = 4 threads (<= 4 cap)
constexpr std::uint64_t kN = 100'000;    // messages
constexpr std::uint32_t kBudget = 16;    // small budget => many handoffs => stress the protocol

struct Tick {
    std::uint64_t seq;
};

struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Tick>;

    // Concurrency witness (atomic): must never observe > 1 executor in the handler at once.
    std::atomic<int> live{0};
    std::atomic<int> max_live{0};

    // Serialized state — PLAIN fields, mutated only inside handle(). Correct iff single-executor +
    // the head_/state handoff give a happens-before across worker handoffs (TSan proves it).
    std::uint64_t expected = 0;
    std::uint64_t fifo_violations = 0;

    std::atomic<std::uint64_t> dispatched{0};  // atomic: the main thread polls it for termination

    // Exactly-once witness.
    std::vector<std::uint8_t>* seen = nullptr;
    std::uint64_t double_dispatch = 0;

    void handle(const Tick& t) noexcept {
        const int now = live.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = max_live.load(std::memory_order_relaxed);
        while (now > prev && !max_live.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
        }

        if (t.seq != expected) ++fifo_violations;  // per-actor strict FIFO (single producer stream)
        expected = t.seq + 1;
        if ((*seen)[t.seq]) ++double_dispatch;
        (*seen)[t.seq] = 1;

        dispatched.fetch_add(1, std::memory_order_release);
        live.fetch_sub(1, std::memory_order_acq_rel);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    Counter actor;
    std::vector<std::uint8_t> seen(kN, 0);
    actor.seen = &seen;

    // Pre-allocate messages + descriptors COLD (no pool contention: producer and workers touch
    // disjoint concerns — the producer only enqueues, workers only drain/reclaim in place).
    std::vector<Tick> msgs(kN);
    std::vector<Descriptor> descs(kN);
    for (std::uint64_t i = 0; i < kN; ++i) {
        msgs[i].seq = i;
        descs[i].payload = &msgs[i];
        stamp<Counter, Tick>(descs[i]);
    }

    Activation act{&actor, Counter::dispatch_table()};  // default reclaim = Descriptor::release()

    std::atomic<bool> stop{false};

    // Workers: busy-poll the acquire edge (the harness stands in for 002's targeted wakeup). Only
    // one worker can hold Running at a time (the CAS) — that is the invariant under test.
    std::vector<std::jthread> workers;
    workers.reserve(kWorkers);
    for (unsigned w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (!act.try_acquire()) {  // Scheduled->Running; skip if not ours / not scheduled
                    std::this_thread::yield();
                    continue;
                }
                for (;;) {
                    const auto o = act.drain_step(kBudget);
                    if (o == Activation::DrainOutcome::DrainedEmpty) {
                        if (act.close_out()) continue;  // re-acquired: more work arrived
                        break;                          // released to Idle
                    }
                    if (o == Activation::DrainOutcome::BudgetExhausted) {
                        act.yield_to_scheduled();  // Running->Scheduled (harness re-acquires via poll)
                        break;
                    }
                    if (o == Activation::DrainOutcome::Busy) {
                        std::this_thread::yield();  // producer mid-publish: bounded spin, keep Running
                        continue;
                    }
                    break;  // Suspended: no async handler in this test
                }
            }
        });
    }

    // Producer: enqueue seq 0..N-1 in order (single stream ⇒ total FIFO expectation). jthread joins.
    std::jthread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) act.post(&descs[i]);
    });
    producer.join();

    // Wait for every message to be dispatched, then stop the workers.
    constexpr std::uint64_t kStallLimit = 4'000'000'000ULL;
    std::uint64_t spins = 0;
    while (actor.dispatched.load(std::memory_order_acquire) < kN) {
        if (++spins > kStallLimit) {
            std::fprintf(stderr, "STALL: dispatched %" PRIu64 " / %" PRIu64 "\n",
                         actor.dispatched.load(), kN);
            stop.store(true, std::memory_order_release);
            return 1;
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();

    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kN; ++i)
        if (!seen[i]) ++missing;

    check(actor.max_live.load() == 1, "concurrent-executor counter never exceeded 1 (single-executor)",
          ok);
    check(actor.dispatched.load() == kN, "every message dispatched", ok);
    check(actor.double_dispatch == 0, "no message dispatched twice (exactly once)", ok);
    check(missing == 0, "no message lost", ok);
    check(actor.fifo_violations == 0, "per-actor FIFO preserved (dispatch order == enqueue order)", ok);
    check(actor.expected == kN, "drained the whole stream", ok);

    std::printf("exec_single_executor_test: %s  (workers=%u N=%" PRIu64 " max_live=%d dispatched=%" PRIu64
                " fifo_violations=%" PRIu64 " double=%" PRIu64 " missing=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kWorkers, kN, actor.max_live.load(), actor.dispatched.load(),
                actor.fifo_violations, actor.double_dispatch, missing);
    return ok ? 0 : 1;
}
