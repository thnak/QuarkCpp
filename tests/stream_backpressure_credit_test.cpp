// Tests 024-Streaming §Backpressure — the credit window IS the lever (GATE-3 / ADR-005 C2): a fast
// (4x-overproducing) producer STALLS via credit depletion — lossless, NO mid-stream drop — instead of
// shedding; occupancy is pinned at capacity; and the producer UN-STALLS on the next credit-return
// (the reverse-Dekker rendezvous, 024 §Producer un-stall). Two threads (producer + consumer) — machine
// safe. Deterministic: no wall clock; checksum seeded from detail::splitmix64.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <thread>
#include <vector>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {
struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
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
    constexpr std::uint64_t kN = 200'000;
    constexpr std::uint32_t kCap = 256;

    StreamChannel<Frame>::Config cfg;
    cfg.capacity = kCap;
    std::pmr::monotonic_buffer_resource mr;
    StreamChannel<Frame> ch(cfg, &mr);

    std::atomic<std::uint64_t> max_occupancy{0};
    std::atomic<bool> producer_done{false};

    // Producer: full speed (4x the consumer's drain shape). On credit depletion it stalls and WAITS on
    // the credit-return edge — never drops. push_blocking counts the stall episodes (real backpressure).
    std::jthread producer([&] {
        for (std::uint64_t id = 0; id < kN; ++id) {
            ch.push_blocking(Frame{id, detail::splitmix64(id)});  // lossless stall on credit depletion
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer: drains in small budgeted batches (slower than the producer -> the ring pins at capacity
    // and the producer stalls). Verifies no loss / no dup / no tear; wakes stalled producers at each
    // batch boundary via poll_unstall.
    std::vector<std::uint8_t> seen(kN, 0);
    std::uint64_t processed = 0, torn = 0, dup = 0, inversions = 0, expected = 0;
    std::jthread consumer([&] {
        while (processed < kN) {
            const std::uint64_t occ = ch.occupancy();
            if (occ > max_occupancy.load(std::memory_order_relaxed))
                max_occupancy.store(occ, std::memory_order_relaxed);

            StreamBatch<Frame> batch(ch, 8);  // small budget -> the producer laps and stalls
            bool any = false;
            while (const Frame* f = batch.next()) {
                any = true;
                if (f->checksum != detail::splitmix64(f->id)) ++torn;
                if (f->id != expected) ++inversions;
                expected = f->id + 1;
                if (seen[f->id]) ++dup; else seen[f->id] = 1;
                ++processed;
                batch.retire();
            }
            ch.poll_unstall();  // reverse-Dekker: wake a credit-starved producer on credit-return
            if (!any) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kN; ++i)
        if (!seen[i]) ++missing;

    // The window bound: credit_available == capacity - (head - tail), exactly (definitional invariant).
    const bool credit_ok = ch.credit_available() == ch.capacity() - (ch.head() - ch.tail());

    check(processed == kN, "every frame delivered (0 drops — lossless backpressure)", ok);
    check(missing == 0, "0 lost frames", ok);
    check(dup == 0, "0 duplicate frames", ok);
    check(torn == 0, "0 torn frames", ok);
    check(inversions == 0, "FIFO preserved under stall/un-stall", ok);
    check(max_occupancy.load() <= kCap, "occupancy pinned at/under the credit window (bounded memory)", ok);
    check(max_occupancy.load() >= kCap, "occupancy actually reached capacity (producer outran consumer)", ok);
    check(ch.stall_events() > 0, "the producer STALLED on credit depletion (real backpressure, not shedding)", ok);
    check(producer_done.load(), "the producer UN-STALLED and completed (credit-return rendezvous woke it)", ok);
    check(credit_ok, "credit == window - (head - tail), exactly (derived, race-free by construction)", ok);

    std::printf("stream_backpressure_credit_test: %s  (N=%" PRIu64 " cap=%u stalls=%" PRIu64
                " max_occupancy=%" PRIu64 " missing=%" PRIu64 " dup=%" PRIu64 " torn=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", kN, kCap, ch.stall_events(), max_occupancy.load(), missing, dup, torn);
    return ok ? 0 : 1;
}
