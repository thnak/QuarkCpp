// Tests 024-Streaming §FIFO-per-stream (GATE-2 / ADR-005 C1): frames drain in the monotone order of
// the producer's `head` cursor with 0 inversions, 0 torn payloads, 0 duplicates, over a BOUNDED frame
// count. Deterministic single-thread ping-pong over the bounded ring (capacity 256): fill to credit
// depletion, drain a budgeted batch through StreamBatch, repeat — exercising the batch-flush boundary
// every cycle. No threads, no wall clock, checksum seeded from detail::splitmix64.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
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
static_assert(sizeof(Frame) <= kStreamInlineMax, "frame fits the inline slot regime (024)");

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t kN = 400'000;
    constexpr std::uint32_t kBudget = 64;

    StreamChannel<Frame>::Config cfg;
    cfg.capacity = 256;
    std::pmr::monotonic_buffer_resource mr;  // cold, grows once for the ring
    StreamChannel<Frame> ch(cfg, &mr);

    std::vector<std::uint8_t> seen(kN, 0);
    std::uint64_t next_push = 0, expected = 0, processed = 0;
    std::uint64_t inversions = 0, torn = 0, dup = 0;
    std::uint64_t max_occupancy = 0;

    while (processed < kN) {
        // Fill until credit depletes or the stream is exhausted.
        while (next_push < kN) {
            Frame f{next_push, detail::splitmix64(next_push)};
            if (!ch.try_push(f)) break;  // credit depleted -> stall (lossless), drain some first
            ++next_push;
        }
        if (ch.occupancy() > max_occupancy) max_occupancy = ch.occupancy();

        // Drain a budgeted batch (the developer-surface StreamBatch drain overload).
        StreamBatch<Frame> batch(ch, kBudget);
        while (const Frame* f = batch.next()) {  // next() advances disp (dispatch)
            if (f->id != expected) ++inversions;
            expected = f->id + 1;
            if (f->checksum != detail::splitmix64(f->id)) ++torn;
            if (seen[f->id]) ++dup; else seen[f->id] = 1;
            ++processed;
            batch.retire();  // retire() advances tail (credit return)
        }
    }

    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kN; ++i)
        if (!seen[i]) ++missing;

    check(inversions == 0, "0 FIFO inversions (monotone head order)", ok);
    check(torn == 0, "0 torn payloads (checksum intact)", ok);
    check(dup == 0, "0 duplicate frames", ok);
    check(missing == 0, "0 lost frames", ok);
    check(processed == kN, "every frame delivered exactly once", ok);
    check(expected == kN, "drained the whole stream in order", ok);
    check(max_occupancy <= ch.capacity(), "occupancy never exceeded the credit window", ok);
    check(ch.head() == kN && ch.disp() == kN && ch.tail() == kN, "all three cursors reached N", ok);

    std::printf("stream_fifo_test: %s  (N=%" PRIu64 " inversions=%" PRIu64 " torn=%" PRIu64
                " dup=%" PRIu64 " missing=%" PRIu64 " max_occupancy=%" PRIu64 " cap=%u)\n",
                ok ? "OK" : "FAIL", kN, inversions, torn, dup, missing, max_occupancy, ch.capacity());
    return ok ? 0 : 1;
}
