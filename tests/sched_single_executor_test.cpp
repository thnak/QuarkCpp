// Tests 002-Scheduler + 001 single-executor UNDER THE REAL SCHEDULER (multi-worker, work-stealing).
// Multiple actors spread across shards are fed FIFO substreams by one producer while 3 worker lanes
// steal across shards. Asserts, per actor:
//   * a concurrent-executor witness NEVER exceeds 1 (single-executor — the exec-state CAS holds even
//     as different lanes hand the actor off);
//   * every message is dispatched EXACTLY once (no lost / no double);
//   * per-actor FIFO is preserved (dispatch order == enqueue order within the actor).
// The handler mutates PLAIN (non-atomic) per-actor fields — only the exec-state release/acquire
// handoff makes that safe across lane handoffs, so this is the load-bearing TSan test.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

constexpr std::uint32_t kWorkers = 3;   // 3 lanes + 1 producer = 4 threads (≤ 4)
constexpr std::uint32_t kShards = 4;
constexpr std::uint32_t kActors = 6;
constexpr std::uint64_t kPerActor = 50'000;
constexpr std::uint32_t kBudget = 16;   // small ⇒ many handoffs ⇒ stress the protocol

struct Tick {
    std::uint64_t seq;  // per-actor sequence
};

struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Tick>;

    std::atomic<int> live{0};
    std::atomic<int> max_live{0};

    std::uint64_t expected = 0;        // PLAIN — FIFO witness
    std::uint64_t fifo_violations = 0;  // PLAIN
    std::vector<std::uint8_t>* seen = nullptr;
    std::uint64_t double_dispatch = 0;  // PLAIN
    std::atomic<std::uint64_t> dispatched{0};

    void handle(const Tick& t) noexcept {
        const int now = live.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = max_live.load(std::memory_order_relaxed);
        while (now > prev && !max_live.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
        }
        if (t.seq != expected) ++fifo_violations;
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
    const std::uint64_t total = kPerActor * kActors;

    std::vector<Counter> actors(kActors);
    std::vector<std::vector<std::uint8_t>> seen(kActors, std::vector<std::uint8_t>(kPerActor, 0));
    for (std::uint32_t a = 0; a < kActors; ++a) actors[a].seen = &seen[a];

    Engine<> eng(EngineConfig{kWorkers, kShards, kBudget, 64});
    std::vector<std::unique_ptr<Activation>> activations;
    std::vector<Schedulable*> sched(kActors);
    activations.reserve(kActors);
    for (std::uint32_t a = 0; a < kActors; ++a) {
        activations.push_back(std::make_unique<Activation>(&actors[a], Counter::dispatch_table()));
        sched[a] = eng.register_activation(ActorId{TypeKey{7}, a}, *activations[a]);
    }

    // Pre-allocate a distinct descriptor + payload per message (targeting actor i%kActors, per-actor
    // seq i/kActors) so the producer and the reclaiming workers touch disjoint descriptors.
    std::vector<Tick> msgs(total);
    std::vector<Descriptor> descs(total);
    for (std::uint64_t i = 0; i < total; ++i) {
        msgs[i].seq = i / kActors;
        descs[i].payload = &msgs[i];
        stamp<Counter, Tick>(descs[i]);
    }

    eng.start();

    // Single producer streams round-robin: each actor sees its substream in strict FIFO order.
    std::jthread producer([&] {
        for (std::uint64_t i = 0; i < total; ++i) eng.post(sched[i % kActors], &descs[i]);
    });
    producer.join();

    // Wait for the whole stream to drain.
    constexpr std::uint64_t kStallLimit = 20'000'000'000ULL;
    std::uint64_t spins = 0;
    for (;;) {
        std::uint64_t d = 0;
        for (std::uint32_t a = 0; a < kActors; ++a) d += actors[a].dispatched.load();
        if (d >= total) break;
        if (++spins > kStallLimit) {
            std::fprintf(stderr, "STALL: dispatched %" PRIu64 " / %" PRIu64 "\n", d, total);
            eng.stop();
            return 1;
        }
    }
    eng.stop();

    std::uint64_t missing = 0, dtot = 0, fifo = 0, dbl = 0;
    int max_live = 0;
    for (std::uint32_t a = 0; a < kActors; ++a) {
        for (std::uint64_t i = 0; i < kPerActor; ++i)
            if (!seen[a][i]) ++missing;
        dtot += actors[a].dispatched.load();
        fifo += actors[a].fifo_violations;
        dbl += actors[a].double_dispatch;
        if (actors[a].max_live.load() > max_live) max_live = actors[a].max_live.load();
    }

    check(max_live == 1, "per-actor concurrent-executor witness never exceeded 1 (single-executor)", ok);
    check(dtot == total, "every message dispatched", ok);
    check(dbl == 0, "no message dispatched twice (exactly once)", ok);
    check(missing == 0, "no message lost", ok);
    check(fifo == 0, "per-actor FIFO preserved (dispatch order == enqueue order)", ok);

    std::printf("sched_single_executor_test: %s  (workers=%u shards=%u actors=%u total=%" PRIu64
                " max_live=%d dispatched=%" PRIu64 " fifo=%" PRIu64 " double=%" PRIu64 " missing=%" PRIu64
                ")\n",
                ok ? "OK" : "FAIL", kWorkers, kShards, kActors, total, max_live, dtot, fifo, dbl, missing);
    return ok ? 0 : 1;
}
