// Dimensions 2 ("FIFO per sender") and 3 ("Exactly once") of the mailbox test suite, exercised
// through the REAL production path: Engine::spawn + ActorRef::tell (MessagePool acquire -> Mailbox
// enqueue -> Activation drain -> handler dispatch -> MessagePool reclaim), not just the raw Mailbox
// primitive (that is tests/mailbox_mpsc_test.cpp, which this test complements rather than
// duplicates: mailbox_mpsc_test.cpp proves the QUEUE contract with pre-allocated cold descriptors
// and no pool in the loop; this test proves the same contract survives the WHOLE stack, including
// the (partitioned) MessagePool a real `tell()` goes through).
//
// SOUND METHODOLOGY (mandatory — see 015-Reentrancy-and-Quiescence.md "Methodology warning
// (ADR-031)" and decisions/ADR-031-mailbox-mpsc-hot-path-r8-judgment.md, REX-CAS/C claim C1):
// tagging messages with a value read from a SEPARATE shared monotonic counter (sampled before or
// after the actual publish) produced real, reproducible false-positive "inversions" (2.9-7.3% of
// 20M messages) purely from the verification method, not a real bug. This test NEVER samples a
// shared/cross-thread counter. Each producer thread owns a PRIVATE, purely-local `int seq` that it
// increments itself before every send; the assertion is per-sender strictly-increasing delivery,
// matching Quark's actual documented contract (006-Messaging-and-Addressing.md, FIFO per
// (sender, receiver) pair) — NOT a global cross-sender total order, which Quark does not claim.
//
// Exactly-once is checked with a full seen[] bitmap over the (sender, seq) space — every one of the
// producers*per_producer unique ids must be observed exactly once (no loss, no duplicate), which is
// stronger than a bare producer==consumer count match.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

constexpr unsigned kProducers = 4;         // machine-safety cap: <= 4 producer threads
constexpr int kPerProducer = 50'000;       // 200k messages total through the full production path
constexpr int kTotal = static_cast<int>(kProducers) * kPerProducer;

struct Tagged { int producer; int seq; };

struct Collector : Actor<Collector, Sequential> {
    using protocol = Protocol<Tagged>;

    // Single-executor invariant (Sequential) => these are touched by exactly one drain lane at a
    // time, so plain (non-atomic) state is race-free by construction — no shared counter is read
    // by the PRODUCERS here, only written by the one consumer lane.
    std::vector<int> expected_seq = std::vector<int>(kProducers, 0);
    std::vector<std::uint8_t> seen = std::vector<std::uint8_t>(static_cast<std::size_t>(kTotal), 0);
    std::uint64_t dup = 0, torn = 0, fifo_violation = 0;
    std::atomic<int> delivered{0};

    void handle(const Tagged& t) noexcept {
        if (t.producer < 0 || t.producer >= static_cast<int>(kProducers) || t.seq < 0 ||
            t.seq >= kPerProducer) {
            ++torn;
            delivered.fetch_add(1, std::memory_order_release);
            return;
        }
        const std::size_t gidx = static_cast<std::size_t>(t.producer) * kPerProducer +
                                 static_cast<std::size_t>(t.seq);
        if (seen[gidx]) ++dup;
        seen[gidx] = 1;
        if (t.seq != expected_seq[static_cast<std::size_t>(t.producer)]) ++fifo_violation;
        expected_seq[static_cast<std::size_t>(t.producer)] = t.seq + 1;
        delivered.fetch_add(1, std::memory_order_release);
    }
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // Sized generously (kTotal + slack) so partition growth/backpressure never masks the invariant
    // under test; 4 partitions spreads the 4 producers across independent MessagePool free-lists
    // (the exact partitioning this session shipped in message_pool.hpp — commit 1964203).
    detail::MessagePool pool(static_cast<std::size_t>(kTotal) + 1024, /*num_partitions=*/4);
    Collector actor;
    auto act = std::make_unique<Activation>(&actor, Collector::dispatch_table(), pool.sink());

    // 2 worker lanes, 1 shard (a Sequential actor is single-executor regardless of shard/worker
    // count) — producers + 2 workers stays within the <=4-core machine-safety budget for the
    // consumer side; producer threads are a separate, also-capped-at-4 set below.
    Engine<> eng(EngineConfig{2, 1, 64, 64});
    eng.register_activation(actor_id_of<Collector>(7), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Collector> ref = router.get<Collector>(7);
    eng.start();

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    for (unsigned p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            // PRIVATE, purely-local sequence counter — never shared, never read by another thread.
            for (int seq = 0; seq < kPerProducer; ++seq)
                ref.tell(Tagged{static_cast<int>(p), seq});
        });
    }
    while (ready.load(std::memory_order_acquire) < kProducers) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();

    constexpr std::uint64_t kStall = 10'000'000'000ULL;
    std::uint64_t spins = 0;
    while (actor.delivered.load(std::memory_order_acquire) < kTotal) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: delivered %d / %d\n", actor.delivered.load(), kTotal);
            eng.stop();
            return 1;
        }
    }
    eng.stop();  // clean drain: no in-flight handler races the checks below

    std::uint64_t missing = 0;
    for (int i = 0; i < kTotal; ++i)
        if (!actor.seen[static_cast<std::size_t>(i)]) ++missing;

    check(actor.delivered.load() == kTotal, "delivered count == produced count", ok);
    check(missing == 0, "every (producer, seq) id observed at least once (no loss)", ok);
    check(actor.dup == 0, "no (producer, seq) id observed twice (no duplication)", ok);
    check(actor.torn == 0, "no torn/out-of-range tag observed", ok);
    check(actor.fifo_violation == 0,
          "per-sender strictly-increasing delivery (FIFO per (sender, receiver) pair)", ok);

    std::printf("mailbox_engine_fifo_exactly_once_test: %s  (producers=%u, total=%d, missing=%"
                PRIu64 ", dup=%" PRIu64 ", torn=%" PRIu64 ", fifo_violation=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kProducers, kTotal, missing, actor.dup, actor.torn,
                actor.fifo_violation);
    return ok ? 0 : 1;
}
