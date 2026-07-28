// Memory footprint benchmark — the reporting half of dimension 21 ("Memory") of the mailbox
// benchmark suite. The CORRECTNESS half (0 heap allocations on the steady-state hot path) is
// already covered by the existing tests/mailbox_noalloc_test.cpp (a global-operator-new-replacing
// probe, per tests/CMakeLists.txt's TSan-exclusion convention) — this file does not repeat that; it
// answers the DIFFERENT, reporting-only question the brief also asks for: how many bytes does an
// idle mailbox/descriptor cost, and what is the actual bytes/partition tradeoff now that
// MessagePool (detail/message_pool.hpp, this session's commit 1964203) can be split into N
// partitions?
//
// Informational only — prints byte counts, no [MISS]/[goal] tokens; always exits 0. No threads, no
// timing — this is a static/structural report, safe to run anywhere.
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

int main() {
    std::printf("== Quark mailbox/pool memory footprint (dim 21) ==\n\n");

    std::printf("[per-idle-mailbox / per-descriptor bytes]\n");
    std::printf("  sizeof(Descriptor)      = %zu B   (hard ceiling: %zu B, 003/023 one-cache-line rule)\n",
                sizeof(Descriptor), quark::max_descriptor_size);
    std::printf("  sizeof(Mailbox)         = %zu B   (head_ + cache-line-padded tail_ + cache-line-\n"
                "                            padded stub_ Descriptor — an idle Mailbox's fixed cost)\n",
                sizeof(Mailbox));
    std::printf("  quark::cache_line_size  = %zu B\n\n", quark::cache_line_size);

    std::printf("[MessagePool per-partition bytes/partition tradeoff]\n");
    std::printf("  sizeof(detail::MessagePool)     = %zu B  (the pool object itself: num_partitions_ +\n"
                "                                    a heap-allocated Partition[] — see below)\n",
                sizeof(detail::MessagePool));
    // Cell size: Descriptor + destroy-thunk pointer + Partition* home pointer + kMaxPayload inline
    // bytes, padded to kPayloadAlign — this is what grow_one() allocates per cell, one per capacity
    // slot, wherever its home partition lands it.
    constexpr std::size_t kMaxPayload = detail::MessagePool::kMaxPayload;
    constexpr std::size_t kPayloadAlign = detail::MessagePool::kPayloadAlign;
    // Mirror message_pool.hpp's private Cell layout by hand (Descriptor + fn ptr + Partition* +
    // payload, padded to kPayloadAlign) since Cell itself is private — this is the same arithmetic
    // grow_one() performs, just recomputed here for the report.
    auto round_up = [](std::size_t n, std::size_t align) { return (n + align - 1) / align * align; };
    const std::size_t cell_unpadded =
        sizeof(Descriptor) + sizeof(void (*)(void*)) + sizeof(void*) + kMaxPayload;
    const std::size_t cell_bytes = round_up(cell_unpadded, kPayloadAlign);
    std::printf("  per-cell bytes (Descriptor + destroy-thunk + home-partition ptr + %zu B inline\n"
                "  payload, %zu B aligned) ~= %zu B/cell (one heap alloc per cell via grow_one())\n",
                kMaxPayload, kPayloadAlign, cell_bytes);

    // Fixed per-partition overhead (a QUARK_CACHE_ALIGNED Partition: mutex + free_head pointer +
    // a std::vector<std::unique_ptr<Cell>> control block — the vector's OWN backing storage is a
    // separate heap allocation whose size is capacity-dependent, counted with the cells below).
    struct ApproxPartition {  // structurally mirrors message_pool.hpp's private Partition for sizing
        alignas(quark::cache_line_size) unsigned char pad[sizeof(std::mutex) + sizeof(void*) +
                                                          sizeof(std::vector<int>)];
    };
    const std::size_t partition_fixed_bytes = sizeof(ApproxPartition);
    std::printf("  fixed per-partition overhead (QUARK_CACHE_ALIGNED{mutex, free_head, vector ctrl})\n"
                "  ~= %zu B/partition (cache-line-rounded so partitions never false-share each other)\n\n",
                partition_fixed_bytes);

    std::printf("[worked tradeoff: same total capacity, varying num_partitions]\n");
    const std::size_t capacity = 4096;
    for (std::size_t parts : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{16}}) {
        const std::size_t cells_total_bytes = capacity * cell_bytes;  // same regardless of parts
        const std::size_t fixed_overhead_bytes = parts * partition_fixed_bytes;
        const std::size_t unique_ptr_slots_bytes =
            capacity * sizeof(void*);  // std::vector<std::unique_ptr<Cell>> backing storage, ~1 ptr/cell
        const std::size_t total_bytes = cells_total_bytes + fixed_overhead_bytes + unique_ptr_slots_bytes;
        std::printf("  num_partitions=%-3zu  capacity=%zu  total ~= %zu B (%.1f KiB)   "
                    "[fixed partition overhead: %zu B, %.3f%% of total]\n",
                    parts, capacity, total_bytes, static_cast<double>(total_bytes) / 1024.0,
                    fixed_overhead_bytes, 100.0 * static_cast<double>(fixed_overhead_bytes) /
                                              static_cast<double>(total_bytes));
    }
    std::printf(
        "\n[info] the cell storage (capacity x per-cell bytes) DOMINATES total footprint regardless\n"
        "of num_partitions; the fixed per-partition overhead (one cache-line-rounded mutex+pointer+\n"
        "vector-control-block struct) is a small, near-constant tax that scales linearly with\n"
        "partition COUNT, not capacity — quantifying the actual bytes/partition tradeoff this\n"
        "session's MessagePool partitioning fix (commit 1964203) introduced. More partitions ==\n"
        "more of this fixed tax, in exchange for the contention removal measured in\n"
        "bench/mailbox_pool_partition_bench.cpp.\n");
    return 0;
}
