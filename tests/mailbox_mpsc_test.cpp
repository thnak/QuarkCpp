// Tests 003-Memory §Mailbox — exactly-once MPSC and per-producer FIFO under real contention.
// Many producers (<= 4 std::jthread), single consumer (main). ADR-002/004: no loss, no dup,
// no torn/reordered handle. TSan is load-bearing here (the MPSC has cross-thread edges).
//
// Descriptors are pre-allocated COLD (one per message) so the hot loop makes 0 heap allocations
// and producers never contend a pool — the queue is the only shared structure under test.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"

using namespace quark;

namespace {
constexpr unsigned kProducers = 4;                     // <= 4 threads (machine-safety cap)
constexpr unsigned kPerProducer = 100'000;             // 400k messages total
constexpr std::uint64_t kTotal = static_cast<std::uint64_t>(kProducers) * kPerProducer;

// message_id encodes {producer:24, seq:40} so the consumer can verify per-producer FIFO.
constexpr std::uint64_t encode(unsigned producer, std::uint64_t seq) {
    return (static_cast<std::uint64_t>(producer) << 40) | seq;
}
constexpr unsigned producer_of(std::uint64_t id) { return static_cast<unsigned>(id >> 40); }
constexpr std::uint64_t seq_of(std::uint64_t id) { return id & ((1ULL << 40) - 1); }
}  // namespace

int main() {
    // Cold: pre-allocate every descriptor (raw vector, not the pool — no free/reuse needed here).
    std::vector<Descriptor> descs(kTotal);
    for (unsigned p = 0; p < kProducers; ++p)
        for (std::uint64_t s = 0; s < kPerProducer; ++s)
            descs[p * kPerProducer + s].message_id = MessageId{encode(p, s)};

    Mailbox mb;
    std::atomic<bool> go{false};

    // Producers: each enqueues its slice in seq order. jthread auto-joins.
    std::vector<std::jthread> producers;
    producers.reserve(kProducers);
    for (unsigned p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) { /* spin to line producers up */ }
            for (std::uint64_t s = 0; s < kPerProducer; ++s)
                mb.enqueue(&descs[p * kPerProducer + s]);
        });
    }

    // Single consumer: main thread. Exactly-once accounting + per-producer FIFO.
    std::vector<std::uint8_t> seen(kTotal, 0);
    std::vector<std::uint64_t> expected_seq(kProducers, 0);
    std::uint64_t received = 0;
    std::uint64_t dup = 0, torn = 0, fifo_violation = 0;

    go.store(true, std::memory_order_release);

    std::uint64_t idle_spins = 0;
    constexpr std::uint64_t kStallLimit = 2'000'000'000ULL;  // watchdog: fail instead of hang
    while (received < kTotal) {
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) {
            // Empty or Busy while producers still run: keep polling (never treat Busy as done).
            if (++idle_spins > kStallLimit) {
                std::fprintf(stderr, "STALL: received %" PRIu64 " / %" PRIu64 "\n", received, kTotal);
                return 1;
            }
            continue;
        }
        idle_spins = 0;
        const std::uint64_t id = r.desc->message_id.value;
        const unsigned p = producer_of(id);
        const std::uint64_t s = seq_of(id);
        if (p >= kProducers || s >= kPerProducer) { ++torn; ++received; continue; }
        const std::uint64_t gidx = p * kPerProducer + s;
        if (seen[gidx]) ++dup;
        seen[gidx] = 1;
        if (s != expected_seq[p]) ++fifo_violation;  // per-producer strict FIFO
        expected_seq[p] = s + 1;
        ++received;
    }

    // Every message seen exactly once.
    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kTotal; ++i)
        if (!seen[i]) ++missing;

    const bool ok = (received == kTotal) && dup == 0 && missing == 0 && torn == 0 &&
                    fifo_violation == 0;
    std::printf("mailbox_mpsc_test: %s  (producers=%u, total=%" PRIu64 ", dup=%" PRIu64
                ", missing=%" PRIu64 ", torn=%" PRIu64 ", fifo_violation=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kProducers, kTotal, dup, missing, torn, fifo_violation);
    return ok ? 0 : 1;
}
