# ADR-064 — Hot-path counter-guided optimisations: no `div` on acquire/resolve (003/006; #4)

Status: **Accepted** (2026-10-28; owner: "dive deeper, we are pre-1.0 and can ship a large performance
improvement"). Implemented in `include/quark/detail/message_pool.hpp` (`lane_mask_`),
`include/quark/detail/flat_actor_map.hpp` (new), `include/quark/core/engine.hpp` (`by_id_`);
tested by `tests/flat_actor_map_test.cpp`, `tests/message_pool_*`, `tests/engine_send_pool_partitions_test.cpp`.

## Method
Hardware counters were enabled on the Linux host (Xeon E5-2680 v4, bare metal, `perf_event_paranoid=1`, CPU
governor `performance` for the runs, restored afterwards). A scratch steady-state bench (`1 producer -> 1
worker`, bounded in-flight window so neither side idles in an unbounded backlog, threads pinned to distinct
physical cores, 2M messages, median of 5 runs, variants interleaved over 3 rounds) measured msgs/s. `perf
record -g` + `perf annotate -l` located hot instructions; every candidate was then A/B'd as a *throwaway
patch on a copy of the headers* before being kept. Baseline (commit 39734f1): **~4.9 M msg/s = ~205 ns/msg,
~600 instructions and ~980 cycles per message across both threads (IPC 0.6).**

## What the counters found, and what survived an A/B

| Candidate (profile evidence) | A/B result (1 producer -> 1 worker) | Verdict |
|---|---|---|
| `MessagePool::acquire`: `lane % num_partitions_` compiles to a 64-bit `div` (~40 cycles, Broadwell) on EVERY acquire, even N = 1 | 4.4-5.1 -> **5.5-6.1 M/s** (+17-30%) | **KEPT** |
| `Engine::resolve_courier`: `unordered_map::find` = hash + bucket `div` + pointer chase, 3 `div` instrs in the function, ~5% of cycles | with the mask: 5.1-6.1 -> **6.3-6.4 M/s** (a further ~+8%) | **KEPT** |
| Producer `lock addq` on the per-actor `mailbox_enqueued` metric (34% of `post_checked` samples) | 6.38 vs 6.31, 6.37 vs 6.27, 6.27 vs 6.16 (none) | rejected |
| Consumer `lock cmpxchg` claim in `Descriptor::try_claim` as a plain store (upper bound, unsafe vs cancel) | 5.3 / 6.2 / 5.0 vs 6.3 (**slower**) | rejected |
| `local_cache_for` weak_ptr identity compares skipped (upper bound, unsafe) | 6.37 / 6.42 / 6.04 vs 6.4 (none) | rejected |
| Software prefetch of the next descriptor in `try_dequeue` | none (ADR-063 follow-up run, 3 x 3 configs) | rejected |
| `rq_next` / `activation` on separate cache lines in `Schedulable` (`perf c2c` false-sharing suspect) | none (+-3%) | rejected |
| Longer `pre_park_spin_limit` | none at window 8192; +17% (5.4 -> 6.3) only at window 64 (latency-bound), costs CPU | left to the existing knob (ADR-035) |

Result kept: **4.9 -> 6.3 M msg/s single producer (+27%, 205 -> 158 ns/msg)**. Multi-producer, window 65536,
unpinned: P=2 6.0 -> 6.35 (+4-8% over repeats), P=4 7.9 -> 8.7 (+9%), P=8 8.7 -> 9.3 (+7%).

## Decisions
1. `MessagePool` selects a partition with `lane & mask` when the partition count is a power of two (the new
   ADR-063 auto default always is) and falls back to `%` otherwise. Behaviour is identical; only the
   arithmetic changed.
2. `Engine::by_id_` is a `detail::FlatActorMap`: power-of-two open addressing, linear probing, load factor
   <= 1/2, contiguous 16-byte slots, `hash & mask`. Same concurrency contract as the `unordered_map` it
   replaces: mutated cold, before `start()`; read concurrently without locks afterwards.

## What is left, and why the micro-level is exhausted
At ~158 ns/msg the producer thread and the consumer thread are balanced (~47% / ~45% of samples) and the
remaining cycles sit on **cache-line migration**, not instructions: the `xchg` on `tail_` (48% of
`post_checked`) is a full barrier that waits for the producer's just-issued stores into a recycled cell
that the consumer last dirtied; the `try_claim` CAS (50% of `drain_step`) is the first touch of the
descriptor the producer just wrote. Each message moves roughly two descriptor/payload lines producer ->
consumer -> producer. Removing individual RMWs or prefetching does not change that (the table above).
Meaningful further gains need an architectural change that reduces lines moved per message, e.g.:
- batched `tell` (a producer publishes k descriptors with one `tail_` exchange; amortises the barrier);
- consumer-driven cell return to the producer's own partition in bulk (fewer dirtied-line round trips);
- a payload-in-descriptor-line layout for small messages (one line instead of two).
Each touches the mailbox hot path (ADR-002/003/004/031) and needs the debate/prove cycle.

## Limits
Linux x86-64, g++ 15.2 -O2 only; single socket; 1 actor in the bench so `by_id_` was always a 1-entry
table (a larger table adds cache misses to BOTH the old and new lookup; the flat table's probe is one line);
Windows numbers not re-measured.
