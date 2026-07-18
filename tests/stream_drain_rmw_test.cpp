// Tests 024-Streaming §The drain path has zero cross-core RMW (S1 / ADR-005, the load-bearing 023
// gate). The consumer inner loop is plain acquire-load + release-store ONLY — no lock/cmpxchg/xadd/
// xchg/mfence. Two complementary proofs:
//   (1) STRUCTURAL: the drain drives `MonotoneCursor`, whose API is load/store ONLY (no fetch_add/
//       exchange/compare_exchange exists), so a consumer CANNOT issue a cursor RMW even by accident.
//   (2) OBJDUMP: the `extern "C"` [[gnu::noinline]] `quark_stream_drain_probe` symbol below is
//       disassembled by tests/stream_drain_rmw_check.sh (a separate CTest, non-sanitizer builds) and
//       grep-verified to contain 0 lock/cmpxchg/xadd/xchg-mem/mfence — the ADR-005 method verbatim.
// Running THIS binary is also a functional drain test (correct checksum). Deterministic, single-thread.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory_resource>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {
struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};
}  // namespace

// The drain inner loop under objdump inspection: pure StreamBatch next()/retire() over MonotoneCursor
// cursors. Marked noinline + extern "C" so the symbol name is stable and the body is not folded away.
extern "C" [[gnu::noinline]] std::uint64_t quark_stream_drain_probe(StreamChannel<Frame>* ch,
                                                                    std::uint32_t budget) noexcept {
    std::uint64_t checksum = 0;
    StreamBatch<Frame> batch(*ch, budget);
    while (const Frame* f = batch.next()) {  // dispatch: plain acquire-load + release-store
        checksum += f->checksum;
        batch.retire();                      // credit-return: plain relaxed-load + release-store
    }
    return checksum;
}

int main() {
    StreamChannel<Frame>::Config cfg;
    cfg.capacity = 4096;
    std::pmr::monotonic_buffer_resource mr;
    StreamChannel<Frame> ch(cfg, &mr);

    constexpr std::uint32_t kFill = 4000;
    std::uint64_t expect = 0;
    for (std::uint32_t i = 0; i < kFill; ++i) {
        Frame f{i, detail::splitmix64(i)};
        if (!ch.try_push(f)) {
            std::fprintf(stderr, "fill failed at %u\n", i);
            return 1;
        }
        expect += f.checksum;
    }

    const std::uint64_t got = quark_stream_drain_probe(&ch, kFill);
    const bool ok = (got == expect) && ch.occupancy() == 0 && ch.tail() == kFill;

    std::printf("stream_drain_rmw_test: %s  (drained=%u checksum=%" PRIu64 " structural-0-RMW="
                "MonotoneCursor has no RMW API; objdump proof = stream_drain_zero_rmw CTest)\n",
                ok ? "OK" : "FAIL", kFill, got);
    return ok ? 0 : 1;
}
