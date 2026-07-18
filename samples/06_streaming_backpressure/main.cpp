// Quark sample 06 — Streaming with credit-based backpressure (024).
//
// A stream is a bounded, ordered channel of frames with a producer and a consumer. Its defining
// property is BACKPRESSURE: the ring has a fixed capacity, and the producer can only push while it
// holds credit. When credit runs out `try_push` returns false — the producer must wait for the
// consumer to drain (which returns credit) instead of allocating unboundedly. That is what keeps a
// fast producer from overwhelming a slow consumer, losslessly and with zero heap churn on the path.
//
// The consumer drains in budgeted batches through a StreamBatch:
//   * next()   — hand out the next frame in FIFO order (advances the "dispatch" cursor)
//   * retire() — mark a frame consumed, RETURNING one unit of credit to the producer (advances "tail")
//
// This sample is a deterministic single-thread producer/consumer ping-pong: fill to credit depletion,
// drain a batch, repeat — proving FIFO order, no loss, no duplication, and bounded occupancy.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 06_streaming_backpressure
// Run  :  taskset -c 0-3 build/samples/06_streaming_backpressure
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <vector>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};
static_assert(sizeof(Frame) <= kStreamInlineMax, "frame fits the 024 inline slot");

int main() {
    constexpr std::uint64_t kFrames = 100'000;  // total frames to stream through
    constexpr std::uint32_t kCapacity = 256;    // ring size == max credit == max in-flight frames
    constexpr std::uint32_t kDrainBudget = 64;  // frames drained per consumer turn

    StreamChannel<Frame>::Config cfg;
    cfg.capacity = kCapacity;
    std::pmr::monotonic_buffer_resource mr;  // cold: the ring is allocated once, here
    StreamChannel<Frame> ch(cfg, &mr);

    std::vector<std::uint8_t> seen(kFrames, 0);
    std::uint64_t next_push = 0, expected = 0, processed = 0;
    std::uint64_t inversions = 0, torn = 0, dup = 0, max_occupancy = 0, backpressure_stalls = 0;

    while (processed < kFrames) {
        // PRODUCER: push until credit depletes (backpressure) or the source is exhausted.
        bool stalled = false;
        while (next_push < kFrames) {
            Frame f{next_push, detail::splitmix64(next_push)};  // checksum ties payload to id
            if (!ch.try_push(f)) { stalled = true; break; }     // no credit -> must drain first
            ++next_push;
        }
        if (stalled) ++backpressure_stalls;
        if (ch.occupancy() > max_occupancy) max_occupancy = ch.occupancy();

        // CONSUMER: drain up to kDrainBudget frames; retire() returns credit to unblock the producer.
        StreamBatch<Frame> batch(ch, kDrainBudget);
        while (const Frame* f = batch.next()) {
            if (f->id != expected) ++inversions;              // FIFO: ids arrive strictly in order
            expected = f->id + 1;
            if (f->checksum != detail::splitmix64(f->id)) ++torn;  // payload integrity
            if (seen[f->id]) ++dup; else seen[f->id] = 1;     // exactly once
            ++processed;
            batch.retire();
        }
    }

    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kFrames; ++i)
        if (!seen[i]) ++missing;

    std::printf("streamed %llu frames through a %u-slot ring\n",
                (unsigned long long)kFrames, kCapacity);
    std::printf("  FIFO inversions : %llu  (expected 0)\n", (unsigned long long)inversions);
    std::printf("  torn payloads   : %llu  (expected 0)\n", (unsigned long long)torn);
    std::printf("  duplicates      : %llu  (expected 0)\n", (unsigned long long)dup);
    std::printf("  missing frames  : %llu  (expected 0)\n", (unsigned long long)missing);
    std::printf("  max occupancy   : %llu  (bounded by capacity %u — backpressure held)\n",
                (unsigned long long)max_occupancy, kCapacity);
    std::printf("  backpressure stalls: %llu  (>0 — the producer was throttled, losslessly)\n",
                (unsigned long long)backpressure_stalls);

    const bool ok = inversions == 0 && torn == 0 && dup == 0 && missing == 0 &&
                    max_occupancy <= kCapacity && backpressure_stalls > 0;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
