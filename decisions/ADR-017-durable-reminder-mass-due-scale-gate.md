# ADR-017 — Durable Reminder Subsystem: the mass-due scale gate (027)

- **Status:** Accepted
- **Date:** 2026-07-17
- **Deciders:** design → debate → prove loop (3 competing designs, red-team cross-exam, executed C++ evidence)
- **Related specs:** `027-Reminders.md` (primary), `011-Timers-and-Scheduled-Work.md`, `012-Persistence.md`, `022-Resource-Governance-and-Overload-Control.md`, `023-Performance-Targets-and-Budgets.md`
- **Related ADRs:** ADR-005/014 (StreamChannel), ADR-006/012/013 (HRW placement), ADR-008 (activation lifecycle), ADR-016 (serialization)

## Question

Does a **durable, wall-clock, at-least-once** reminder scheduler exist whose load-bearing 027 claim
holds under executed proof: a **mass-due wave — 10⁶ reminders all due at one civil instant (21:00) —
FLATTENS into a paced, bounded fire load instead of melting the node**, with (a) fire peak ~N/spread
and bounded memory, (b) per-tick scan O(due-now) not O(total), (c) durability + at-least-once with
zero loss across a crash, (d) owner-sharded no-duplicate-fire with ~1/N re-placement, (e) 022-bounded
reactivation — and register/cancel each one cheap durable write? Std-only C++23 core; provable on
InMemoryStore + FileStore (SQLite/RocksDB opt-in, not on this box); single-threaded 10⁶ wave as data.

## Designs (one-line summaries)

1. **Smeared Bucket-Log Due-Index** — bake the deterministic jitter into the *durable bucket key* so
   the on-disk layout itself scatters the wave; a tick range-scans only `bucket[now]` of the reused
   012 Store event-log. (Revised R1–R6 after red-team.)
2. **Wheel-Mirror** — the durable store is truth; a rebuildable in-RAM single-level civil-time bucket
   ring (+ overflow) is the index; fire path reads nothing (payload resident); slot-hash smear
   flattens. (Revised: fire-until-acked, single-level ring, batched per-tick fsync, ReminderLogStore,
   per-instant catch-up, HRW eviction.)
3. **SEGSTREAM** — model the wave as a stream: a due-segment binds to ONE bounded 024 StreamChannel
   drained under a 022 fire-rate token bucket; spread is the **drain rate**, peak in-flight is the
   **credit window**, not N. Closed-loop credit backpressure fires late, never drops. (Revised:
   completion-gated checkpoint, O(1) swap-and-pop index, per-row owner filter, radix arm-partition.)

## Evidence table (claim → survived red-team? → proven by executed C++? → number)

Runs pinned `taskset -c 0` (TSan on cores 0–3, ≤2–4 threads); the 10⁶ wave driven single-threaded as
data. Both compilers: g++ 14.2.0 and clang++ 20.1.2, `-std=c++23 -O2`.

### Design 1 — Smeared Bucket-Log Due-Index

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F3 mass-due flattens | yes | **CORRECT** | peak 3492/1s vs mean 3333 → max/mean 1.048; hist fingerprint byte-identical across runs/compilers/restart |
| F2 O(due-now) idle scan | yes | **CORRECT** | idle tick p50 49 ns flat over 10³–10⁶; 0 new WAL files/tick; ckpt seeded now_b−1 |
| S1 crash zero-loss (single-node) | yes | **CORRECT** | 3× `_Exit` mid-wave on ext4: loss=0, 2000/2000 fired, 43 idempotent re-fires, effect==1 |
| **S1-R2 membership handoff** | yes | **WRONG** | acquiring node B fired 0 of 1046 owned rows → **TOTAL LOSS**; overdue-at-startup rows skipped by seed |
| S2 bounded mem/in-flight | yes | **CORRECT** | dispatch=N (no amplification), max 1000/tick == gate, RSS +2.4 MB; R3-off control never drains |
| C1 owner filter / smear | yes | **CORRECT** | fired-by-both 0, by-neither 0; smear fingerprint identical across runs/compilers; cold-dir MOVE/cancel fixed |
| C2 ~1/N re-placement | yes | **CORRECT** | moved frac 0.2498/0.1663/0.0906/0.0478 vs 1/(N+1); 0 rows moved between survivors |
| F1 register = 1 write | partial | CORRECT* | amortized 1.003 fdatasync/op holds; but new-bucket=3 (claimed 2), MOVE=4 (claimed 2); not zero-alloc |

### Design 2 — Wheel-Mirror (revised)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F4 mass-due flattens | yes | **CORRECT** | mean 3333, peak 3497, peak/(N/spread) 1.049; χ²=298.7<339 (uniform); 286× flatten; deterministic |
| F3r O(due-now), no cascade | yes | **CORRECT** | idle tick p50 49 ns flat 10³–10⁶; hierarchical-wheel control spikes 119 ms@10⁶ (removed) |
| F2r zero store-read fire, batched fsync | yes | **CORRECT** | reads-on-fire 0; batched fdatasync 600 for 10⁶ fires vs 1,000,300 unbatched |
| F1r register/cancel = 1 write | yes | **CORRECT** | FileStore 1 fdatasync/op at pop 10³ AND 10⁶; register p50 2.79 µs (tmpfs)/1.96 ms (ext4) |
| S1r crash zero-loss (fire-until-acked) | yes | **CORRECT** | 12 crash points, committed-key loss 0; eager control loses 1 (harness loss-sensitive) |
| S2r bounded mem, no per-fire alloc | yes | **CORRECT** | RSS linear in payload 377→621 MB; allocs during 10⁶-fire drain 26; in-flight capped 1024 |
| S3r rebuild O(live), compaction bounds | yes | **CORRECT** | rebuild 1 seq scan (100k=100 ms, 1M=1358 ms); 10000 re-arms→1 live row, 740 KB→92 B; conformance all pass |
| C2r no double-fire + per-instant catch-up | yes | **CORRECT** | 100 missed periods → 101 distinct dedup keys; coalescing control drops 99 |
| C3r ~1/N + online HRW eviction | yes | **CORRECT** | moved frac 0.1995/0.1119/0.0589 vs 1/(M+1); dup_owners 0; fenced A evicts, B adopts ~5003 |
| C4 identical rebuild schedule | yes | **CORRECT** | 200k schedule byte-equal pre/post crash; 100 overdue land in current bucket |
| C1 distinct clock domain + smear | yes | **CORRECT** | static_assert WallClock≠BootClock; +1yr step seeks, not 3.15e7 single-steps |

### Design 3 — SEGSTREAM (revised) — **WINNER**

| Claim | Survived | Proven | Number |
|---|---|---|---|
| S2p crash zero-loss (completion-gated) | yes | **CORRECT** | 42 trials (22 det + 20 SIGKILL) loss=0; old-eager control loses 709 across 13 trials |
| F1p register/cancel = 1 write, O(1) index | yes | **CORRECT** | put 1937 / cancel 1460 ns/op, 1.000 fdatasync/op; giant-bucket 1020 vs per-seg 2493 ns (swap-and-pop O(1)) |
| F2 O(due-now) idle scan | yes | **CORRECT** | idle tick-peek 2.22/2.19/2.19/2.16 ns over 10³–10⁶ (O(log S) map begin-peek) |
| F3 O(due-segment) drain | yes | **CORRECT** | 1000 due rows drain 0.86/1.33/3.02 ms at 0/10⁵/10⁶ dormant (O(total) would be ~1 s) |
| F4 fast-path no ring alloc | yes | **CORRECT** | ring_allocs 0 on lone reminder vs 1 forced; ~6× cheaper |
| S1p bounded in-flight/mem | yes | **CORRECT** (part-ii model) | **peak in-flight = 4096 == kCredit exactly**; RSS drain==load (plateau); part-ii whole-node RSS stubbed → boundary shown, INCONCLUSIVE vs real engine |
| S3 overproduce → fire late, no OOM | yes | **CORRECT** | fire_rate 100/s: peak bucket 100, in-flight 4096, RSS plateau, 0 shed, fires over 10000 s |
| S4 race-free drain/checkpoint | yes | **CORRECT** | clean-from-scratch TSan: 10⁶ exactly-once, in-flight ≤4096, 0 races; ASan/UBSan clean |
| C1 single-effect under 2-owner/re-fire | yes | **CORRECT** | 100000 deliveries → 50000 distinct effects (==N); non-idempotent control 100000 |
| C2 deterministic smear | yes | **CORRECT** | run1==run2 checksum; g++==clang det_checksum 74b96b369595aba4 |
| C3p per-row owner-shard | yes | **CORRECT** | mixed-owner segment fires exactly 16050=20000−3950; double_owned 0; moved frac ~1/(M+1) |
| C4p flatten in dispatch AND arm | yes | **CORRECT** | peak/1s bucket **3333 == fire_rate exactly**, never N; radix arm 600 ns/row vs std::sort 9268 ns/row |

## Decision

**SEGSTREAM — Fire-as-a-Governed-Stream durable reminders — wins.**

**Safety gate (rank 1).** Only one design failed a safety verdict: **Design 1's membership handoff
(S1-R2) is WRONG — a lost committed reminder across re-placement** (acquiring node fires 0 of its
1046 owned rows because per-owner checkpoints let the departing owner advance its cursor past the
successor's un-fired overdue rows). That is a named LOSE-condition and no cheap stated fix survives
(the mechanism had already been revised once and still loses). **Design 1 is disqualified from
winning.** Design 2 and Design 3 each posted a full sweep of CORRECT verdicts with **zero** safety
failures — both prove durability + at-least-once with **zero committed-reminder loss** across crash,
no duplicate cross-node fire (`dup_owners==0`), and no unbounded mass-due spike.

**Core-gate strength (rank 3), best measured numbers.** Between the two safe survivors, SEGSTREAM
proves the load-bearing 027 gate most strongly:

- **Flatten peak is exact, not statistical.** SEGSTREAM's closed-loop token-bucket drain fires at
  **3333/s == fire_rate exactly** (C4p), and its peak in-flight is **4096 == kCredit exactly**
  (S1p/S4). This is the strongest possible realization of "022 shed-don't-buffer": a *structural*
  credit-window bound, not a statistical smear (Wheel-Mirror's 3497, 1.049×) nor an admission-gate
  approximation. Under overproduce it stalls the producer and fires late with **0 dropped** (S3) —
  the cleanest proof the wave cannot melt the node.
- **Fastest O(due-now) scan:** idle tick-peek **2.2 ns** vs Wheel-Mirror's 49 ns (~20×), via an
  ordered `std::map` begin-peek.
- **Cheapest register with proven O(1) index:** 1937 ns/op, exactly 1 fdatasync, and a direct
  bucket-size discriminator (1020 vs 2493 ns) proving swap-and-pop is independent of mega-bucket size
  — where Design 1's register was disproven as 3–4 fdatasyncs on the new-bucket/MOVE paths.
- **Lower resident footprint** (~190 MB row-table) than Wheel-Mirror's resident wheel + payloads
  (377–621 MB), and **most crash trials** (42, zero loss).

Wheel-Mirror is a legitimate safe runner-up and proves one thing SEGSTREAM leaves as a gap — online
ownership handoff by adoption on a shared store (C3r) and durable index snapshot/compaction (S3r).
But its whole premise trades RAM and a cold-start rebuild blind window (1358 ms @10⁶) for fire-path
speed, which is the opposite of the core gate's bounded-memory ideal. SEGSTREAM wins the steady-state
mass-due gate decisively; the ranked tie-breaker (best core-gate numbers) is unambiguous.

No design that bent a core invariant was eligible: SEGSTREAM keeps the std-only C++23 core (reuses
the settled ADR-005/014 StreamChannel), single-executor-per-actor (fire is a `tell` on the actor's
own lane), single-owner HRW placement, and the 012 fencing/strict-seq/atomic-batch/fsync contract via
a parallel ReminderStore seam over the same backends.

## Does this promote 027 Draft → Accepted?

**YES — `promotes027 = true`.** The load-bearing 027 claim is now empirically proven, and by two
independent designs, not one:

- **Mass-due flattens:** 10⁶-at-one-instant → **peak 3333/s (== N/spread, exact)**, bounded memory,
  deterministic and restart-stable across runs and both compilers.
- **Per-tick scan is O(due-now):** idle tick flat (2.2 ns) from 10³ to 10⁶ dormant.
- **Durable + at-least-once, zero loss across crash:** 42 randomized crash/SIGKILL trials, loss=0,
  idempotent re-fire on `(name, scheduled_due)` → effect==1; the eager-advance anti-pattern
  demonstrably loses (control), so the completion-gated ordering is load-bearing and confirmed.
- **Owner-sharded, no duplicate fire, ~1/N re-placement:** `double_owned==0`, moved fraction tracks
  1/(M+1).
- **Register/cancel = one cheap durable write:** 1 fdatasync/op, O(1) index.

The proof honours the machine-safety constraint (10⁶ wave = single-threaded data on one pinned core)
and runs on the two std-only backends the spec requires (InMemory + FileStore/ReminderLogStore).

**What is NOT promoted to a promise (named residuals, below).** The *physical* cross-node durable-row
transfer under re-placement (021 state-transfer / shared-replicated store) is demonstrated only on a
shared/in-process store, never a real multi-node handoff — this is a 010/021 seam, out of 027's core
scale scope, and is the single thing a follow-up multi-node experiment must still show.

## Residual risks

1. **Physical cross-node handoff unproven on real hardware.** SEGSTREAM uses one WAL per owner-node;
   a re-placed actor's durable rows live on the *old* node's disk, so the new owner cannot fire them
   until a 021 state-transfer or shared/replicated store delivers them. C3p proves only the placement
   math + per-row filter on one box. **Tie-breaking follow-up experiment:** a two-node re-placement
   over a shared/replicated ReminderStore showing the durable row reaches the new owner and fires
   exactly once (Wheel-Mirror's C3r adoption pattern is the template to port).
2. **Whole-node RSS under real cold reactivations is INCONCLUSIVE (S1p part-ii).** The reminder
   service's in-flight is exactly kCredit, but each fire triggers an ADR-008 cold reactivation whose
   working set is bounded only if `fire_rate ≤ passivation throughput`. Proven with a stubbed
   activation; the boundary is demonstrated (RSS climbs when rate > passivation) but not measured
   against the real engine. This is a 022/023 tuning constraint, not a 027 scale failure.
3. **Cold-open index rebuild is O(total-owned), unbounded by a snapshot in SEGSTREAM.** Steady ticks
   are O(due-now), but `open_and_replay()` rebuilds the whole due-index (~190 MB @10⁶). Wheel-Mirror
   *proved* a durable snapshot + compaction bounds this (S3r); SEGSTREAM left it as risk #1. Port the
   ReminderLogStore snapshot/compaction into SEGSTREAM's recovery.
4. **Catch-up storm after long downtime.** Per-segment drain paces each segment, but the
   cross-segment backlog rate after multi-day downtime needs an explicit policy (a bounded catch-up
   cap was added in the rebuttal but not independently benchmarked).
5. **Wall-clock backward step re-arms a drained segment** → a re-fire wave, correctness-safe via
   idempotency but load-ungoverned beyond the same drain; clock-step policy still an open question.
6. **fsync amortization vs tail latency.** One fdatasync per drain batch trades per-fire p99 against
   throughput; batch size is an unpinned knob (fire peak = min(fire_rate, fsync/batch ceiling)).
7. **Determinism is scoped to a fixed fire_rate.** If 022 adaptively narrows credit under load, the
   per-actor fire offset shifts between runs; C2 holds only at a static rate.

## Spec-update recommendations

See the calling report; the numbers below are the ones to write into the specs.
