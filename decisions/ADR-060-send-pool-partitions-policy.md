# ADR-060 — Send-pool partitions as an engine config policy (#4, follow-up (a) of ADR-057)

Status: Accepted (opt-in; default unchanged). Evidence: `tests/engine_send_pool_partitions_test.cpp`
(exactly-once at 1/2/8 partitions; passes on MSVC Release, and on Linux under TSan and ASan+UBSan),
`bench/mailbox_pool_partition_bench.cpp`, `bench/mailbox_producer_sweep_bench.cpp`.

## Why
ADR-057 left finding 3 ("does `num_partitions` help?") inconclusive: the Windows box was noisy
(+-30%). A quiet Linux host (Xeon E5-2680 v4, 1 socket, 28 logical cores, load 0.00, g++ 15.2,
Release) settles it.

## Measurements (Linux, quiet, full Engine tell path, 1 worker, 200k msgs/producer)
| producers | num_partitions=1 | num_partitions=P | gain |
|---|---|---|---|
| 4  | 1.47 M/s | 7.01 M/s  | 4.8x |
| 8  | 1.05 M/s | 9.29 M/s  | 8.8x |
| 12 | ~1 M/s   | 10.11 M/s | ~10x |

Sweep bench (pool-inclusive send path, P=8): pool 6.0 M/s, pool/part 15.6 M/s. The raw mailbox
(`tail_.exchange`) scales UP with producers on this box (11 -> 29 M/s, P=1 -> 16), i.e. no collapse:
the shared pool free-list is the bottleneck, as ADR-057 finding 2 suspected. Producer-side enqueue
latency under P=4 contention: p50 219 ns, p99 438 ns, p999 584 ns (`mailbox_enqueue_latency_bench`).

## Decision
Expose the existing `MessagePool(capacity, num_partitions)` knob as policy:
`EngineConfig::pool_partitions` (default 1 = today's behavior, byte for byte), `ConfigBuilder::
pool_partitions(n)`, and `Engine::make_send_pool()` which builds the pool from `pool_capacity *
shard_count` and `pool_partitions`. No change to `mailbox.hpp` / `message_pool.hpp`; no hot-path code
changes; the knob only selects an existing constructor argument. Tune it to ~ the number of
concurrent sender threads.

## Not done
- Not made the default: the gain needs measuring on a multi-socket box and against the
  1-producer case before flipping it (partitions cost memory and the P=1 path is unchanged here).
- Follow-ups (b) `ask` reply-cell pool and (c) opt-in batched `tell` / `enqueue_chain` remain open.
- End-to-end `tell` p999 (issue's 41.8 us) not re-measured; the number above is enqueue-only.
- Cross-socket Xeon from the issue still unreproduced (this host is single-socket).
