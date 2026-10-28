# ADR-063 — Send-pool partitions default to AUTO, resolved by the engine (003; supersedes ADR-060's "default unchanged"; #4)

Status: **Accepted** (2026-10-28; owner: "make the default an engine rule, but measure it carefully — what
happens with two partitions? trace, etc."). Implemented in `include/quark/core/engine_config.hpp`
(`pool_partitions = 0`, `resolve_pool_partitions()`), `Engine::make_send_pool()`;
tested by `tests/engine_send_pool_partitions_test.cpp`.

## Decision
`EngineConfig::pool_partitions` defaults to **0 = auto**. The engine resolves it:
1. explicit value >= 1: used verbatim;
2. auto: the smallest power of two >= `hardware_concurrency`, capped at **16**;
3. then halved while a partition would hold **< 1024 cells** of `pool_capacity x shard_count`
   (default 4096 cells => at most 4 partitions; 4 shards => up to 16).

`hardware_concurrency` is read only to size this index space; it creates no threads, so the
"never hardware_concurrency for `worker_count`" machine-safety rule is untouched.
`detail::MessagePool`'s own constructor default stays 1: only the engine-built pool changes behaviour.

## Measurements (Xeon E5-2680 v4, 28 threads, 1 socket, g++ 15.2 -O2, idle host; median of 3-5 runs)
Harness: a scratch sweep (producers P x partitions N x workers W x actors A), `tell()` to a Sequential
actor through `LocalRouter`; records send-side M msg/s, sampled `tell()` latency (1 in 128) and the number of
heap allocations in the timed region (cold pool growth). Producers outrun the consumer, so *backlog* is
unbounded and the pool must hold it: the pool is either pre-sized (no cold growth) or default-sized.

**Pre-sized pool (isolates lock contention), W=1, send M msg/s and tell p99 (ns):**

| P | N=1 | N=2 | N=4 | N=8 | N=16 |
|---:|---:|---:|---:|---:|---:|
| 4 | 6.9 / 18.7k | 7.6 / 11.2k | **8.1 / 4.8k** | 7.5 / 5.4k | 6.2 / 5.5k |
| 8 | 10.0 / 39k | 8.5 / 15k | **11.7 / 11k** | 10.1 / 5.4k | 8.4 / 6.1k |
| 16 | 10.4 / **175k** | 10.7 / 28k | **14.2 / 20k** | 12.7 / 14k | 12.4 / 7.1k |

With 4 workers / 8 actors, P=16: N=1 10.0 M/s p99 **241 us** -> N=16 24.8 M/s p99 **6.7 us**.
**The robust win is the tail, not the mean**: p99 falls 4-35x; mean throughput gains 1.0-1.4x at P<=8 and
up to 2.5x at P=16 (W=4).

**Default-sized pool (what a user gets), 4 workers / 8 actors, 4096 cells x 4 shards, old N=1 vs new auto:**

| P | N=1 send M/s (p99 ns) | auto = 16 send M/s (p99 ns) |
|---:|---:|---:|
| 2 | 3.84 (13.9k) | 4.07 (5.9k) |
| 4 | 6.96 (25.8k) | 8.01 (5.4k) |
| 8 | 5.39 (38.7k) | 10.40 (5.5k) |
| 16 | 4.72 (56.9k) | **15.88 (5.1k)** |

An earlier headline in ADR-060 (1.05 -> 9.3 M msg/s, 8x) was measured with an UNDER-sized pool, where the
free list grows (heap allocation) **while holding the partition mutex**: at N=1 that serialises every producer
behind a malloc. Pre-sized, the same comparison is 1.0-2.5x on throughput. The tail improvement is real in
both. Do not read 8x as the steady-state gain.

## What happens with two partitions (N=2) — measured
- **N=2 helps, but only about half as much.** P=8 pre-sized: p99 39k -> 15k ns (N=4: 11k, N=8: 5.4k); futex
  calls over the run: **16304 (N=1) -> 10561 (N=2) -> 4064 (N=4) -> 1448 (N=8)** (`strace -c -f -e futex`,
  P=8, 100k msgs each; the contended subset, 6309 -> 3886 -> 1369 -> 512). Contention falls roughly as 1/N.
- **Producers collide.** A thread's partition is `lane_id % N`, lane ids come from a process-wide counter
  (shared with worker threads and every other pool), so with P > N several producers share a partition and
  still contend. With P=4: N=2 gives 5.0 vs N=4 5.4 M/s and p99 8.9k vs 4.2k ns (default-sized, W=1).
- **Odd N is worse than the next lower power of two.** P=4, N=3: 3.86 M/s vs N=2: 5.02 (default-sized, W=1);
  P=16, N=3 4.0 vs N=4 5.7. Consecutive lane ids spread unevenly modulo an odd N. Hence powers of two only.
- **The consumer side is not the cost.** A cell is returned to its *home* partition by whichever worker
  drained it, through that worker's per-partition TLS magazine (ADR-037); a single consumer touches all N
  partitions' magazines, and N is far below the 64-slot direct-mapped magazine table.

## Costs of more partitions (why the cap and the floor exist)
- **Capacity fragmentation.** Capacity is split evenly, but a producer only draws from its own partition.
  Pre-sized pool, P=8: N=16 cold-allocated 119k cells (N<=8: 20) because each partition held 50k and one
  producer's backlog needed 100k. The 1024-cell floor bounds this; at the default 4096 cells it limits auto to
  4 partitions rather than splitting to 256-cell shards.
- **Cold growth is more frequent under overload** (default-sized, P=16 W=1: 1.0M allocs at N=16 vs 0.58M at
  N=1) simply because the faster producers queue more messages; steady state with a keeping-up consumer shows
  no cold allocations (P<=4, W=4: 11 or fewer).
- **1 producer:** within noise (3.4 vs 2.9-3.3 M/s across N, run-to-run spread wider than the gap).
- **Memory:** N x one `Partition` (mutex + vector); the cells themselves are the same total.

## Not measured / limits
- Hardware counters (`perf stat` cache-misses, `perf record`) are unavailable on the host
  (`perf_event_paranoid=4`, no root); the futex counts above are the contention trace.
- Single socket; NUMA same-socket vs cross-socket is skipped by the bench on this box.
- Backlog-heavy producer/consumer imbalance only; a bounded-in-flight workload was not swept.
- g++ only on Linux; the Windows numbers in the issue are from a different (noisy) machine.

## Residual idea (not done)
When a partition is exhausted, refill could first take cells from a sibling partition (`try_lock`) before
heap-growing; that would remove the fragmentation cost and allow a higher auto cap. Left for a future ADR.
