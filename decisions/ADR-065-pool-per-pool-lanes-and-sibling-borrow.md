# ADR-065 — MessagePool: per-pool lane assignment and sibling-borrow before heap growth (003; #30, #31)

Status: **Accepted** (2026-10-28). Implemented in `include/quark/detail/message_pool.hpp`; tested by
`tests/message_pool_lane_and_growth_test.cpp` (+ the existing pool tests).

## Problems
- #31: a thread's partition was `process-wide lane id & mask`; the counter is shared with worker threads and every other pool, so P producers on a P-partition pool could collide by luck.
- #30: an empty partition heap-grew (`grow_one`) while holding its mutex, serialising producers behind malloc, while sibling partitions sat on free cells (fragmentation when over-partitioned).

## Decision
1. Each pool keeps its own `next_lane_`; a thread gets a dense lane on first touch, cached in an 8-entry direct-mapped thread-local keyed by a never-reused pool serial (one compare on the hot path; no address-reuse hazard).
2. `refill`: own partition; else `try_lock` each sibling and take a batch (never holds two locks, never blocks); else allocate ONE cell outside every lock and register it under the own partition's lock. Cells keep their `home`, so `reclaim()` is unchanged.
3. Growth batch is 1 (a larger batch moved malloc cost into every acquire's p50 with no throughput gain).

## Evidence (Linux Xeon E5-2680 v4, g++ 15.2 -O2, governor `performance`, bench `mailbox_pool_partition_bench 4 1`)
| Config | old | new |
|---|---|---|
| pre-sized, 1..4 partitions | 9.7-10.0 M msg/s, heap_allocs 0 | 9.3-10.3 M msg/s, heap_allocs 0 (unchanged) |
| undersized, 1 partition | 3.9 M msg/s | 6.2 M msg/s (p50 ~215 ns vs 160) |
| undersized, 2 partitions | 6.4 | 6.9 |
| undersized, 4 partitions | 8.0 (p99 2.8 us) | 7.0 (p99 5.1 us) |
Grow batch 4/8/32 measured and rejected (p50 up, no throughput gain). TSan and ASan+UBSan: pool tests x5, 0 reports. Windows: pool tests 5/5.

## Limits
The supported configuration (pre-sized pool) is unchanged; the gain is for undersized pools. At 4 partitions undersized, throughput is ~12% lower and p99 higher than before (the sibling probe costs on the cold path). Linux x86-64 only.
