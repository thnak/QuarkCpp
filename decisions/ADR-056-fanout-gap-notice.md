# ADR-056 — In-band gap notice (count + id range) for `FanOut<EvictAfter<N>>` (#10)

Status: Accepted (x86-64; MSVC Release evidence here, see "Not done"). Evidence:
`tests/fanout_gap_notice_test.cpp`, `bench/fanout_gap_bench.cpp`. Spec: 006 §Publish/Subscribe.
Builds on ADR-039 (which already shipped `FanOut<M,Policy>` with `EvictAfter<N>` / `Block`).

## What issue #10 still lacked
ADR-039 settled block-vs-evict as a compile-time policy, and the issue thread settled that AgentEngine
needs only `EvictAfter<N>`. But AgentEngine's stated requirement is that the gap notice "carry enough to
know a resync is needed". ADR-039 shipped only an aggregate counter (`LaneEntry::evicted()`): the
subscriber cannot tell WHERE in its stream the loss happened or WHICH ids were lost. It had a count, not
a notice. This ADR closes that.

## Block vs evict (re-checked, not re-litigated)
Kept as ADR-039 decided. Issue comment 2 cites A2A 2.3/2.4: gap-free delivery is explicitly not required,
so `EvictAfter` is the default and `Block` stays opt-in. No new evidence contradicts this.

## Design A (chosen): consumer-derived gap from id discontinuity
Every envelope already carries the per-publisher sequence id (`FanOutEnvelope::id`). A dropped message
leaves a hole in the ids a lane delivers. The consumer keeps `next_expected_` and, in
`try_pop(out, FanOutGap&)`, reports `{next_expected_, id-1, count}` when the popped id exceeds it. Trailing
drops (nothing queued behind them) cannot be seen by discontinuity, so `take_trailing_gap()` computes them
from `evicted_ - gap_reported_` and is only valid when the lane is observed empty. All gap state is
consumer-thread-only; the producer path and the 16 B envelope are untouched.

## Design B (rejected): producer-stamped gap
Producer keeps `{pending_first, pending_count}` per lane and stamps the next successful push's envelope
with it. Attacks:
- Envelope grows 16 B -> 32 B (measured `sizeof` 16 vs 32): doubles slot memory per lane and the
  ring footprint for every subscriber, healthy or not, to serve a rare event.
- Needs per-lane producer-owned mutable state in the membership snapshot's lane objects (new producer
  write on the evict path), more code on the hot path than A's zero.
- Still cannot report a trailing gap (no later push to carry the stamp), so it needs A's
  `take_trailing_gap()` fallback anyway. Strictly more mechanism for the same coverage.
- Injecting a separate gap-marker slot instead is worse: the lane is full exactly when a gap occurs, so the
  marker itself can fail to enqueue.
B has no correctness advantage; its only plausible benefit (notice survives id reuse) does not apply
because ids are a monotone per-`FanOut` sequence.

## Measured evidence (MSVC 19, Release, x64, single thread, best of 5, 5M ops, cap 4; `fanout_gap_bench`)
Publish + pop per op, three runs. "A-old-pop" = pre-ADR `try_pop(out)`; "A-gap-pop" = new
`try_pop(out, gap)` through the real `FanOut` (shared payload pool, refcounts, snapshot load);
"raw" rows isolate just the lane (no payload/refcount) so the envelope-size effect is not drowned out:

| regime | A-old-pop | A-gap-pop | A-raw 16 B derived | B-raw 32 B stamped |
|---|---|---|---|---|
| healthy (drain each) | 71-80 ns | 70-76 ns | 8.3-9.3 ns | 8.8-9.8 ns |
| overflowing (drain /8) | 66-69 ns | 66-71 ns | 4.3-5.2 ns | 4.5-5.3 ns |

Read honestly: A-gap vs A-old is within run-to-run noise (the new compare is one branch per pop). In
isolation B costs about 3-6% more than A and doubles slot size; B's number is a lower bound (it omits the
refcount work A's real path pays). Allocation: none added; the only new state is three scalar fields in
`LaneEntry`. The ~70 ns FanOut figure is dominated by the existing payload pool/snapshot machinery and is
not changed by this ADR.

## Verification (`fanout_gap_notice_test`, PASS)
1. Cap 4, publish 10 undrained: pops 0..3 carry no gap; `take_trailing_gap()` == {4,9,6}; reported once
   (second call empty); next message after is gap-free (no double report).
2. In-band: gap {4,5,2} appears on exactly the message following the hole; `evicted()` agrees.
3. Mid-stream attach: pre-attach ids are not a gap; a healthy peer lane gets 200/200, 0 gaps, 0 evictions
   while a sibling lane overflows (slow subscriber does not affect healthy ones).
4. Two-thread stress, 2,000,000 publishes against a deliberately lagging consumer: delivered 1,706,455 +
   gap-covered 293,545 == 2,000,000; ranges contiguous and non-overlapping; gap total == `evicted()`.
Ordering per subscriber and no loss for a healthy subscriber are additionally covered by the ADR-039
tests, which still pass. Bounded memory: a lane never exceeds its `N`-slot ring by construction; the gap
state is O(1).

## Residual risks / limits
- `take_trailing_gap()` is only meaningful when the lane is observed empty; a consumer that never polls it
  learns of trailing drops only via `evicted()`.
- The notice reports dropped messages, not "evicted subscriber": the subscriber stays attached (ADR-039).
  Block policy never produces gaps.
- Single-producer precondition unchanged; x86-TSO basis unchanged.

## Not done
No GCC/Clang/ASan/TSan run (Windows MSVC host only); the stress test is written to be TSan-clean (gap
state consumer-only) but that is unproven here.
