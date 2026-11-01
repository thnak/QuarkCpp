# ADR-078 — MessagePool magazine fast hit (pool-serial guard) (003)

Status: **Proposed.** Companion to ADR-077; extends ADR-037's TLS magazine without changing its sizing.

## Problem

ADR-037 fronts each partition's mutex+free-list with a 64-cell TLS magazine, cutting mutex traffic to
~1/32. Every `acquire`/`reclaim` still pays `local_cache_for()`:

```cpp
LocalCache& lc = t_cache_mag.slots[slot_for(&p)];           // hash the Partition*
bool same = lc.bound == &p && !lc.bound_token.owner_before(p.self)
                         && !p.self.owner_before(lc.bound_token);  // two weak_ptr comparisons
```

The two `owner_before` calls are the generation-safe UAF guard (issue #15: a freed-and-recycled
`Partition*` at the same address must not be accepted). They are load-bearing on a **rebind**, but they
run on every hot hit too. `MessagePool::acquire`/`refill` is **24.4% inclusive** (5.7% self + 5.5%
mutex) on the 14-lane stress soak.

## Decision

1. `LocalCache` gains the owning pool's **`serial`** (already unique, never reused:
   `g_next_pool_serial`). `local_cache_for(p, pool_serial)` returns on
   `lc.serial == pool_serial && lc.bound == &p` without touching the weak_ptr.
2. `PoolLaneSlot` (the 8-entry per-thread, per-pool table) caches the acquiring `Partition*` and
   `LocalCache*`. `acquire` calls `lane_slot()` once, then `cache_for_acquire()`:
   `sl.partition == &p && cached->bound == &p && cached->serial == serial_`.
   The `bound` + `serial` re-validation is required because the 64-slot `LocalCacheTable` can evict this
   magazine to another partition/pool on a hash collision.
3. `acquire` fast-paths `num_partitions_ == 1` (partition 0; no mask/lane arithmetic). `reclaim` uses
   `local_cache_for(*cell->home, serial_)`, keeping its home-partition routing for P > 1.
4. No change to `kCap` (64), `kRefillBatch` (32), `kReturnBatch` (32), or `kGrowBatch` (1). ADR-037
   measured those; larger cold-growth batches hurt p50, and the lock-free freelist lost 15–25%
   uncontended. A 64/32 vs 128/64 sweep is optional and expected to be neutral.

## Why the fast hit is safe (issue #15)

- Pool serials are unique for the process lifetime and never reused, so a stale slot from a destroyed
  pool can never satisfy the serial match.
- A fast hit means the **same live pool** (the call is on that pool) and the same partition, so the
  cached `Partition*`/magazine are valid.
- Any mismatch (different pool, address reuse, 64-slot eviction) takes the existing cold path: flush the
  old magazine through its weak_ptr token (`lock()` fails safely if the pool is gone; residual cells are
  forfeited), then bind the new partition and set its serial. The `owner_before` identity check still
  runs on every rebind.
- Thread exit still flushes every bound, non-empty magazine (`LocalCacheTable::~LocalCacheTable`).

## Evidence

- New `tests/message_pool_cache_serial_test.cpp`: 9 pools (> the 8-slot TLS table) interleaved on one
  thread, acquire/reclaim across pools, destroyed-and-recreated pools, and a 4-partition pool whose
  cells are acquired on four threads and reclaimed on one. Asserts cell conservation and one destroy
  thunk per reclaimed message.
- Existing: `message_pool_lane_and_growth_test`, `message_pool_partition_concurrency_test`,
  `message_pool_flush_pins_partition_test`, `message_pool_magazine_dangling_test` (issue #15 UAF),
  `mailbox_pool_aba_stress_test`, `engine_send_pool_partitions_test`; TSan + ASan.
- A/B (28-core Linux host): `mailbox_pool_partition_bench` — presized P1 7.03→7.67, P2 6.97→7.36,
  P3 7.30→7.48 M msg/s; undersized P1 5.31→5.83, P2 5.54→6.20, P3 5.18→5.78; tell p50 down on every
  undersized config. The whole-engine 14-lane stress A/B (shared with ADR-077) is **+6.4% median
  throughput**, p50 280→212 ns, p99 +4.3%, zero loss/dup; see ADR-077's table for the full run.

## Limits / rollback

- The fast hit assumes the caller is invoking `acquire`/`reclaim` on a live pool (the existing lifetime
  contract). Calling into a destroyed pool remains UB, as before.
- A wrong serial match would bypass the UAF guard; serials are never reused, and the new multi-pool test
  covers eviction/destroy/recreate. Rollback is removing the serial predicate and the cached pointers.
- If the A/B is neutral, this is reverted independently of ADR-077.
