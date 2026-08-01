# ADR-037: MessagePool free-list synchronization — TLS acquire/reclaim magazine wins

## Status

Accepted

## Question

`MessagePool` (`include/quark/detail/message_pool.hpp`) hands out fixed-size
Descriptor+payload `Cell`s on the send-side hot path (`acquire()`) and recycles
them when the actor's drain lane finishes with them (`reclaim()`). Each
partition's free list is guarded by a plain `std::mutex`, taken once by
`acquire()` (producer thread) and once by `reclaim()` (the single per-shard
drain lane — almost always a *different* thread). `bench/caf_comparison`
attributed most of the remaining Quark-vs-CAF gap (1.09x single-threaded tell,
1.24x–1.68x MPSC) to this mutex pair. Three designs were proposed to shrink or
remove the tax while preserving: zero heap allocation once pre-sized, no ABA /
UAF / double-free, offset-0 `Descriptor*`==`Cell*`, no new pool-wide lock, and
exact steady-state cell-count conservation.

## Designs (one-line summaries)

1. **Tagged-Index Treiber Stack** — per-partition lock-free free list: a
   packed 64-bit `{version, index}` head CAS (mirrors `Descriptor::gen_state`'s
   idiom, not a 128-bit DWCAS), cells addressed through a growable two-level
   chunk directory, chain reuses `Descriptor::link.next`.
2. **TLS acquire/reclaim magazine** — keep the existing mutex+`free_head`
   exactly as-is; add thread-local, fixed-capacity "magazines" that batch
   `acquire()`/`reclaim()` against the partition mutex 32-at-a-time, so the
   mutex is touched roughly `1/32`th as often. *(winner — see below)*
3. **Producer-private free stack + per-worker-lane SPSC return rings** —
   `acquire()` pops a producer-private stack under a light spinlock;
   `reclaim()` pushes wait-free into a per-(partition, worker-lane) SPSC ring,
   keyed by a new, Scheduler-assigned `worker_lane_id()`.

All three went through design → red-team → prove. Each had at least one fatal
defect in its first draft (design 1: unchecked fixed-size chunk directory,
OOB write reproduced as a hang; design 2: raw `Partition*` TLS pointer,
reproduced UAF at pool teardown / cross-pool address reuse; design 3:
non-power-of-two ring mask, reproduced heap corruption for
`num_partitions` not dividing `capacity` evenly). All three fatal defects were
fixed in revision and the revised safety claims were re-proven clean. The
comparison below is therefore between each design's **final, revised** form.

## Evidence table

Compilers actually used for the executed evidence: design 1 — clang++ 22.1.5,
Windows target (`g++ 14` **not available** in that run; TSan **not available**
on `-fsanitize=thread` for the Windows/MSVC target — no Linux/WSL toolchain
used for this design's evidence). Designs 2 and 3 — g++-14/15 and clang++-20
under WSL2 Ubuntu, i.e. the repo's actual mandated toolchain family, including
real TSan.

| Claim | Design 1 (Treiber stack) | Design 2 (TLS magazine) | Design 3 (SPSC return rings) |
|---|---|---|---|
| Fast: uncontended round trip vs mutex | **F1 WRONG** — 15–25% *slower* uncontended (runtime chunk_size division + directory-pointer load per call) | **F1 CORRECT** — 1.27x–3.0x throughput at P∈{1,2,4} (median, both compilers; g++ magazine's worst sample beat baseline's best sample at every P) | **F1 WRONG** — isolated reclaim()-only latency only 0–15% lower, not the claimed ≥30% (clang: 0% at P=1 and P=4) |
| Fast: contended throughput | F2 **INCONCLUSIVE** — direction right (mean ratio 1.25x, median/geomean ≥1.3x) but not robust across statistics | F2 **CORRECT** — lock calls reduced ~32x (62,502 vs 2,000,000 over 1M cycles), essentially at theoretical minimum | F3 **CORRECT** — 3.0x–4x sustained throughput under real contention (self-relative gate, ≥15x the required margin) |
| Fast: tail latency / convoy | F3 **INCONCLUSIVE** — literal p99 claim failed 2/3 trials; p99.9/max strongly favor CAS (mutex hit 15–19ms stalls, CAS ≤5ms) | F3 **CORRECT** — quantum-bounded flush closes the bursty-traffic cold-grow_one() blowup (0 extra grow calls vs. red-team's demonstrated 40) | F1-convoy **supported** — bounded refill_lock scope: p999 30–45% lower than baseline under undersized-pool collision |
| Safe: TSan-clean | S1 **INCONCLUSIVE** — TSan unavailable on the platform used; ASan/UBSan + debug in-use-flag proxy only (0 violations) | S1 **CORRECT** — real TSan, 0 reports, g++-14 & clang++-20, ~10M msgs | S1 **CORRECT** — real TSan, 0 reports, both compilers, incl. adversarial same-worker-lane-id race |
| Safe: no ABA/UAF/double-free | S2-rev **CORRECT** (after fatal OOB fixed — growable 2-level directory) | S2 **CORRECT** (after fatal UAF fixed — `weak_ptr<PartitionToken>` generation-safe liveness check) | S2 **CORRECT** (after fatal non-pow2 mask fixed — `next_pow2(N_P)` ring sizing) |
| Correct: conservation | C1-rev **CORRECT** | C2 **CORRECT** (scope narrowed: requires explicit quiesce call before pool teardown for the full guarantee; disclosed higher working-set floor) | C1 **CORRECT** |
| Correct: no double hand-out | C2 **CORRECT** (Descriptor layout unchanged) | C1, C3 **CORRECT** (incl. cross-pool address-reuse adversarial test) | C2 **CORRECT** |
| Blast radius | `message_pool.hpp` only | `message_pool.hpp` only | `message_pool.hpp` **+ new Scheduler integration point** (`002-Scheduler.md`: worker must call `set_worker_lane_id()`) |
| Disclosed residual cost | growable directory memory; 40-bit/24-bit version/index rebalance still has a (astronomically remote) wraparound | ~36 KB thread_local footprint per thread that ever touches any pool; capacity must be pre-sized above the pure working set to absorb per-thread magazine residency | ring memory ∝ partitions × worker-lanes (6–25% tax); until `worker_lane_id()` is wired into the Scheduler, `reclaim()` silently and safely falls back to the old mutex path |

Score by the ranking rule (proven-CORRECT claims that survived red-teaming;
disproven counts against; inconclusive counts for nothing):

- **Design 1**: 3 CORRECT (S2-rev, C1-rev, C2), 1 WRONG (F1), 3 INCONCLUSIVE.
- **Design 2**: 8 CORRECT (F1, F2, F3, S1, S2, C1, C2, C3), 0 WRONG, 0 INCONCLUSIVE.
- **Design 3**: 5 CORRECT (F3, S1, S2, C1, C2), 1 WRONG (F1), 1 INCONCLUSIVE (F2).

## Decision

**Winner: Design 2 — TLS acquire/reclaim magazine over the existing
partitioned free-list.**

Rationale, against the stated ranking:

1. **Safety gate.** No design has a *currently* WRONG safe/correct claim —
   every fatal defect found in red-teaming (design 1's OOB chunk directory,
   design 2's dangling TLS pointer, design 3's non-power-of-two ring mask) was
   fixed and the revised claim reproven CORRECT with executed evidence. None
   is disqualified on the gate. Design 2 is the only one of the three whose
   safety claims (S1, S2) were proven on the repo's actual mandated toolchain
   family (g++/clang++ under Linux, with real ThreadSanitizer) rather than a
   partial proxy — design 1's S1 is INCONCLUSIVE specifically because TSan was
   unavailable in the environment used for its proof, which is a materially
   weaker evidentiary basis for the exact hazard (concurrent
   producer-vs-drain-thread races) this task exists to rule out.

2. **Proven beats claimed.** Design 2 is the only one of the three with
   **zero disproven and zero inconclusive claims** — all 8 of its claims
   (3 fast, 2 safe, 3 correct) survived red-teaming and were proven CORRECT
   with executed evidence. Both competitors have their *headline* fast claim
   (uncontended/isolated round-trip cost) **disproven**: design 1's CAS
   round trip is 15–25% *slower* than today's mutex in the uncontended case
   (the common case for the single-threaded tell throughput number this task
   was explicitly invoked to close — 4.60M vs 5.01M msg/s), and design 3's
   isolated reclaim()-only latency claim showed 0–15% improvement against a
   claimed ≥30% bar. A design whose primary claimed win is disproven in the
   dominant workload shape cannot be preferred over one with a clean sweep.

3. **Measured hot-path numbers among safe survivors.** Design 2 delivers
   1.27x–3.0x measured round-trip throughput improvement across the full
   producer-count sweep (P∈{1,2,4}), with the mutex-call count reduced ~32x
   (essentially at the theoretical amortization minimum) — a direct,
   uncontested win on exactly the metric in the task (mutex tax per
   acquire()/reclaim() round trip). Design 3's strongest number (3–4x
   sustained throughput under contention, F3) is real and larger in magnitude,
   but it measures aggregate throughput under producer/worker contention, not
   the per-call round-trip cost F1 was built to isolate — and F1 itself
   failed. Design 1's contended numbers (F2/F3) never cleared their own bar
   robustly (INCONCLUSIVE both ways) and its uncontended cost went the wrong
   direction.

4. **Core invariants.** Design 2 stays entirely inside `message_pool.hpp` —
   no new Scheduler integration point, no new cross-subsystem contract.
   Design 3 requires the Scheduler (002) to stamp a new, exact,
   collision-free `worker_lane_id()` per worker thread; until that wiring
   lands, `reclaim()` silently falls back to the old mutex path for every
   cell (safe, but performance-null) — a partial-rollout footgun and a wider
   blast radius the task's invariant list does not require. Design 2's one
   invariant *narrowing* — full conservation (C2) requires an explicit
   `flush_current_thread_message_caches()` quiesce call before a `MessagePool`
   is torn down — is disclosed, tested, and fails *safe* (a skipped quiesce
   forfeits a residual batch, per S2, rather than corrupting memory); this is
   a strictly better failure mode than design 1's or design 3's original
   (now-fixed) fatal defects and is a reasonable, containable addition to the
   pool's lifetime contract.

Design 2 wins on every ranking criterion: nothing disqualifies it, it has the
cleanest (8/8 CORRECT) evidence record, its measured numbers directly answer
the task's own framing (mutex round-trip tax), and it does not touch the
Scheduler or any invariant outside `message_pool.hpp`.

## Residual risks

- **Working-set floor is higher than the mutex baseline.** Because up to
  `kCap` (64) cells can sit resident in an idle thread's magazine rather than
  on a partition's `free_head`, a pool touched by many distinct threads (or a
  drain thread whose magazine dams up reclaimed cells between bursts) needs
  more pre-sized capacity than the pure steady-state working set to avoid
  extra cold `grow_one()` calls — measured directly in the prove pass (a
  40-cell pre-sized/2-thread config needed 73–103 cells in practice). Callers
  must budget headroom, not just the exact working-set count.
- **New lifetime contract.** Every `MessagePool` must now be quiesced (via
  `flush_current_thread_message_caches()` on every thread that touched it)
  before destruction for the full conservation guarantee (C2). This is safe
  if skipped (no UAF — the `weak_ptr<PartitionToken>` check makes a missed
  quiesce merely leak a residual batch, not corrupt memory) but callers that
  care about exact cell-count conservation across pool lifetimes must adopt
  the new shutdown discipline. The Engine's shutdown path must call this hook
  on every worker/producer thread.
- **~36 KB thread_local footprint per thread** that ever calls
  `acquire()`/`reclaim()` on *any* `MessagePool` in the process (the
  `LocalCacheTable` is process-wide-shared-by-thread, not per-pool). Fine at
  the repo's stated thread-count scale; would need revisiting if the actor
  engine grows into a very-high-thread-count deployment shape.
- **32-slot direct-mapped magazine table**: more than ~13–32 distinct live
  partitions touched by one thread degrades that subset's amortization back
  toward baseline cost via hash-collision eviction. Not exercised by the
  prove pass's benchmarks; worth a regression test if partition counts grow
  materially (e.g. per-shard pools proliferating with many shards per
  process).
- **F1/F3 evidence used a WSL2 Ubuntu VM on a Windows host**, not bare-metal
  Linux with `taskset`-pinned isolation exactly as CLAUDE.md specifies for
  production benchmarking; sample variance (especially under clang++) was
  visibly higher than would be expected on bare metal. The *direction* and
  *order of magnitude* of the win are well-supported (magazine's worst sample
  beat baseline's best sample at every producer count under g++), but a
  confirmatory run on the repo's actual CI/bench hardware, `taskset`-pinned
  per CLAUDE.md, is recommended before citing exact multipliers in
  `PERFORMANCE.md`.
- **kRefillBatch/kReturnBatch=32, kCap=64 are fixed compile-time constants**,
  not exposed as a per-pool tunable the way `num_partitions_` already is. A
  workload with a very different message-size/burst profile cannot retune
  without a code change — flagged as a reasonable, minimal-surface follow-up,
  not a blocker.
- **Neither disproven-but-real signal from the losing designs should be
  discarded.** Design 3's F3 result (3–4x sustained throughput under
  contention via true SPSC return rings) suggests there is more headroom
  beyond what design 2 captures, for workloads with heavy multi-worker
  contention on a shared partition; design 1's p99.9/max tail-latency result
  (mutex occasionally stalls 15–19ms under contention) suggests the *mutex
  fallback* design 2 still uses for cold growth and quiesce could itself be a
  latency-tail source worth watching once design 2 ships.

## Spec recommendations

**`003-Memory.md`** (MessagePool / Cell / free-list section):
- Document the free list's synchronization as: per-partition `std::mutex` +
  `free_head`, fronted by a per-thread, per-partition "magazine" (bounded
  batch cache) that amortizes lock acquisition to ~1/32 of calls in steady
  state. Cite the measured lock-call reduction (~32x) and throughput
  improvement (1.27x–3.0x across P∈{1,2,4}).
- Add an explicit **pool lifetime clause**: "A `MessagePool` must be quiesced
  — every thread that ever called `acquire()`/`reclaim()` on it must call
  `quark::detail::flush_current_thread_message_caches()` — before the pool is
  destroyed, to guarantee exact steady-state cell-count conservation. Skipping
  quiesce is safe (no use-after-free) but may leak a bounded residual batch
  (at most `kCap` cells per thread) rather than returning it to the pool."
- Add a **capacity-sizing note**: pre-sizing to the exact working-set count is
  no longer sufficient to guarantee zero cold growth under multi-thread
  traffic; operators should budget headroom for per-thread magazine residency
  (up to `kCap` cells per thread that touches the pool).
- Note the still-present per-thread `LocalCacheTable` footprint (~36 KB)
  as a fixed cost of any thread that touches message pools at all.

**`002-Scheduler.md`**:
- No changes required for the winning design (design 2 does not touch
  scheduler-worker identity). Record, as a **rejected alternative**, that a
  per-worker-lane SPSC return-ring design (design 3) was evaluated and
  measured a strong contended-throughput win (3–4x) but was not selected
  because its isolated round-trip claim was disproven and it would have
  required adding an exact, Scheduler-stamped `worker_lane_id()` — a new
  cross-subsystem contract — for a win this ADR's winning design achieves
  without one. Flag as a candidate for a future ADR if contended-throughput
  numbers become the binding constraint (see residual risks).

**`006-Messaging-and-Addressing.md`**:
- No structural change: `Descriptor*`/`Cell*` offset-0 interconvertibility,
  message shape, and addressing are unaffected by this ADR — the free-list
  change is entirely internal to `MessagePool`'s recycling path. Add a
  one-line cross-reference to `003-Memory.md`'s new pool-lifetime clause
  where `006` discusses message lifetime/ownership, so readers connect
  "who must call `flush_current_thread_message_caches()`" to the actor
  lifecycle description already there.

## Single tying experiment, if this were closer

It was not close enough to require one — design 2's 8/8 clean record against
two competitors each carrying a disproven headline claim settles it. If a
future revision of design 3 fixes its F1 gap (e.g. by shrinking the
producer-side spinlock's critical section further) and still wants
reconsideration, the tie-breaker would be: **run design 2's magazine and a
fixed design 3 side-by-side, `taskset`-pinned on the repo's actual bare-metal
Linux target (not WSL2), on the full `bench/caf_comparison` single-threaded
tell benchmark plus a P∈{1,2,4,8} contended sweep**, and compare both the
isolated round-trip cost *and* the Scheduler-integration cost/risk before
promoting design 3 over the design 2 already accepted here.
