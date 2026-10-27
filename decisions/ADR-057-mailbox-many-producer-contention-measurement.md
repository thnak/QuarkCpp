# ADR-057 — Mailbox many-producer contention (issue #4): measurement; no hot-path change

Status: Accepted as a MEASUREMENT record. **No change to `mailbox.hpp` / `message_pool.hpp`.**
Evidence: `bench/mailbox_producer_sweep_bench.cpp` (new, informational; verifies per-(sender,receiver)
FIFO + exactly-once on every run; exits non-zero only on a correctness failure).

## 1. Setup (and its limits, stated honestly)
- Machine: AMD Ryzen 5 9600X, 6 cores / 12 logical, single socket, single NUMA node, Windows 11,
  MSVC Release (/O2), 1 consumer pinned to cpu0, producers pinned to cpu1.. while P+1 <= 12
  (P=16 is oversubscribed and unpinned).
- The box was NOT quiet: other work was building/running concurrently (total CPU ~57% when sampled).
  Run-to-run noise is therefore large (same configuration moved by +-30%, e.g. raw P=1 68..113 M/s
  across runs). Treat the numbers as shape/order-of-magnitude, not precise ratios.
- Not reproducible here: the Linux/Xeon cross-socket and p999 tail-latency numbers in the issue
  (this box has one socket; the bench does not record per-message latency). Those remain unverified
  by this ADR.
- TSan is unavailable on MSVC. Correctness of the experiment is covered by the in-bench FIFO /
  exactly-once checks (stress, not a race detector) plus the existing mailbox tests.

## 2. Measurements (M msg/s aggregate, median of 5 runs; three full-sweep invocations)
Columns: raw = shared `Mailbox`, preallocated descriptors (only `tail_.exchange` contended);
pool = `MessagePool::acquire`+`enqueue` / `try_dequeue`+`reclaim`, 1 partition; pool/part =
partitions == P; batch8/32 = bench-local Vyukov copy, producer links K nodes privately then ONE exchange.

| P  | raw (3 runs)        | pool (3 runs)      | pool/part (3 runs) | batch8 (3 runs)  | batch32 (3 runs) |
|----|---------------------|--------------------|--------------------|------------------|------------------|
| 1  | 68 / 106 / 107      | 16 / 29 / 33       | 19 / 28 / 33       | 303 / 264 / 112  | 281 / 164 / 303  |
| 2  | 62 / 64 / 62        | 22 / 23 / 26       | 21 / 22 / 19       | 280 / 291 / 183  | 206 / 251 / 222  |
| 4  | 69 / 68 / 73        | 18 / 14 / 21       | 18 / 13 / 27       | 227 / 230 / 302  | 209 / 227 / 264  |
| 8  | 78 / 77 / 73        | 15 / 16 / 12       | 18 / 21 / 13       | 168 / 256 / 178  | 239 / 171 / 208  |
| 11 | 80 / 62 / 77        | 13 / 15 / 15       | 16 / 17 / 17       | 220 / 250 / 210  | 206 / 233 / 210  |
| 16 | 56 / 76 / 73        | 8.5 / 11 / 10      | 12 / 13 / 13       | 231 / 265 / 238  | 218 / 231 / 238  |

(An earlier sweep with a more contended pool-window harness showed the same shape: raw 113 -> 55 ->
77 -> 82 -> 85 -> 80; pool 30 -> 30 -> 26 -> 17 -> 12 -> 8; pool/part 33 -> 41 -> 42 -> 29 -> 21 -> 17.)

## 3. Findings
1. **Raw mailbox: a step, not a collapse.** Going 1 -> 2 producers costs ~35-50% (the `tail_` line
   starts bouncing), then aggregate throughput is FLAT at ~60-80 M msg/s out to P=11 and P=16
   (oversubscribed). It does not decline monotonically toward zero with P on this machine. 60-80
   M msg/s is ~12-16 ns/message, far above any realistic per-message handler cost.
2. **Pool-inclusive path degrades 2-3x from P=1 to P=11-16** (~30 -> ~10-14 M msg/s), in all runs.
   This is the same "real send path" shape the issue's root cause names (`MessagePool` free-list
   under many producers + single-consumer `reclaim`). It sits an order of magnitude below the raw
   mailbox, so on the send path the pool, not `tail_`, is the dominant term. Note the single
   consumer also performs `reclaim`, so this number includes consumer-side cost that does not
   scale with P either.
3. **Existing `num_partitions` mitigation: inconclusive here.** Partitioning by lane shows 1.2-1.7x
   at P>=8 in some runs and 0.8-1.0x in others; within the +-30% noise this ADR cannot claim it helps.
   `Engine` does not currently construct partitioned pools (only `broker_pool_` use is visible in
   engine.hpp; the local router receives a caller-supplied pool).
4. **Producer-side batching (one exchange per K sends) is 3-4x faster than per-message exchange**
   (P>=2: raw 60-80 vs batch 170-300 M/s) and removes the P=1->2 step. It preserves
   per-(sender,receiver) FIFO (checked in-bench: each producer's chain is internally ordered and the
   exchange is atomic). But it is NOT a drop-in mailbox change: a `tell` is a single message today,
   so batching requires a sender-side buffer that delays delivery until K sends accumulate or a
   flush point, changing latency semantics, and it needs an additive `enqueue_chain` API. Measured
   benefit is only on the raw queue primitive; its effect on the full send path was NOT measured
   (the pool dominates there).

## 4. Decision
- No change to the mailbox hot path. The data on this machine does not show a collapse that the
  Vyukov `tail_.exchange` design causes: a one-time ~1.5-2x step at P=2 then a plateau. Invariants
  (0 alloc, 0 cross-core RMW on drain, 64 B descriptor, single executor) are untouched.
- The issue's collapse is plausibly real on the reporter's cross-socket Xeon (cache-line ping-pong
  costs more across sockets) and at the pool/ask-cell level; this ADR neither confirms nor refutes
  those and does not claim to.
- Recommended follow-ups, each needing its own measurement on a quiet multi-socket Linux box before
  any code change: (a) make `LocalRouter`'s pool partitioned by default and re-measure with a pinned,
  quiet run (finding 3 is inconclusive); (b) `ask` reply-cell pool mutexes (not covered by this
  bench); (c) an opt-in `enqueue_chain`/batched `tell` primitive (finding 4) as a distinct,
  explicitly-documented mode, never a silent change to `tell` (see the scope note in issue #4:
  FIFO is per (sender, receiver)); (d) p999 tail-latency measurement, which this bench lacks.
- Not done: no TSan/Linux run, no tail-latency data, no quiet-machine rerun.
