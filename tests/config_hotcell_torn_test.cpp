// Implements the 013/ADR-008 S2 + 023 "no torn config read under concurrent publish" GATE: with one
// writer live-reconfiguring the HotCell and up to 3 readers hot-reading it, EVERY read observes a
// COHERENT published word — never a mix of an old low half and a new high half. Because a single
// 8-byte-aligned word is hardware-indivisible on x86-64, a tear is impossible; this proves it, and
// under TSan it also proves the relaxed load/store pair is race-free (no data race reported).
//
// The two published words differ in BOTH their low and high 32-bit halves, so any hypothetical tear
// would produce a word in neither set — the reader asserts membership, catching a tear immediately.
//
// Pin: taskset -c 0-3. 1 writer + 3 readers = 4 threads (the machine-safety cap, CONVENTIONS.md).
// Best run under TSan (build-tsan) AND ASan/UBSan.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/hot_cell.hpp"

using namespace quark;

int main() {
    // Two fully-distinct coherent configs — every packed field differs, so low and high halves both
    // change together (a tear would mix halves into a word equal to neither).
    const OperationalConfig cfgA{.drain_budget = 100,  .mailbox_bound = 0x000001, .overflow = Overflow::Block,
                                 .idle_ticks = 0x0001, .log_level = 1, .shed_level = 1};
    const OperationalConfig cfgB{.drain_budget = 16000, .mailbox_bound = 0xFFFFFE, .overflow = Overflow::DropNewest,
                                 .idle_ticks = 0xFFFE, .log_level = 6, .shed_level = 30};
    const std::uint64_t wA = pack_operational(cfgA);
    const std::uint64_t wB = pack_operational(cfgB);

    HotCell cell{wA};

    constexpr int kReaders = 3;
    constexpr std::uint64_t kIters = 5'000'000;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> total_reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin to the start line */ }
            std::uint64_t seen = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t w = cell.raw();  // the hot read (single relaxed load)
                if (w != wA && w != wB) torn.fetch_add(1, std::memory_order_relaxed);
                ++seen;
            }
            total_reads.fetch_add(seen, std::memory_order_relaxed);
        });
    }

    // Writer: alternate the two words with a single relaxed store (the publish) as fast as it can.
    std::thread writer([&] {
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        for (std::uint64_t i = 0; i < kIters; ++i)
            cell.publish((i & 1) ? wB : wA);
        stop.store(true, std::memory_order_relaxed);
    });

    go.store(true, std::memory_order_release);
    writer.join();
    for (auto& t : readers) t.join();

    const std::uint64_t torn_n = torn.load();
    const std::uint64_t reads = total_reads.load();
    std::printf("readers=%d reads=%llu torn=%llu\n", kReaders,
                static_cast<unsigned long long>(reads), static_cast<unsigned long long>(torn_n));
    if (torn_n != 0) {
        std::fprintf(stderr, "FAIL: %llu torn reads observed\n", static_cast<unsigned long long>(torn_n));
        return 1;
    }
    std::printf("PASS: 0 torn over %llu reads under concurrent publish\n",
                static_cast<unsigned long long>(reads));
    return 0;
}
