# ADR-002 — Mailbox MPSC hot path (evidence round 2)

- **Status:** Accepted
- **Date:** 2026-07-13
- **Supersedes / confirms:** ADR-001 (same question, same three designs). This
  round re-ran the debate after each design conceded and *fixed* its red-team
  defects, and re-proved the fixed designs with fresh executed C++ (TSan + ASan +
  UBSan + microbenchmarks). The winner is unchanged; the margin and the
  load-bearing fixes are now backed by their own executed evidence.
- **Scope:** The per-actor Mailbox — the MPSC queue that owns FIFO ordering,
  stores fixed-size `MessageHandle`s (never payloads), enqueued by many producer
  threads and drained by exactly one worker (single-executor invariant).
- **Related specs:** `001-Actor-Execution-Model.md`, `002-Scheduler.md`,
  `003-Memory.md`, `015-Reentrancy-and-Quiescence.md`, `017-Delivery-Guarantees.md`,
  `022-Resource-Governance-and-Overload-Control.md`, `023-Performance-Targets-and-Budgets.md`.

## Question

Which mailbox design gives Quark a **(fast)** allocation-free hot path that does
not collapse under producer contention, **(safe)** data-race / UB / ABA-free
operation under TSan+ASan+UBSan, and **(correct)** strict FIFO with no lost or
duplicated handles and tombstones skipped exactly once — without bending a core
invariant (at-most-one-executor, FIFO-by-default, no heap on the steady hot path,
no data race under sanitizers)?

## Candidate designs (one-line summaries)

- **A — Intrusive Vyukov MPSC (exchange-published, single-consumer, tombstone-lazy).**
  The queue node *is* the pooled `Descriptor` (8-byte `MailNode next` as its first,
  pointer-interconvertible member). Producers publish with one unconditional atomic
  `exchange` on `tail_` (RELEASE) then one link store — wait-free per producer,
  ABA-free by construction (never compares an address). The single consumer walks a
  plain consumer-private `head_`; its cross-worker handoff carries no atomic of its
  own — visibility rides the actor exec-state CAS (release/acquire), which is also
  the single-executor gate. The transient exchange→link window is surfaced as a
  third drain result `Busy`, distinct from `Empty`.
- **B — Segmented single-use FAA ring with QSBR recycling.** Producers `fetch_add`
  a per-segment ticket, plain-store the 16-byte handle, release-publish a per-cell
  state word. The single owner batch-sweeps cells with acquire loads only (no RMW on
  the drain path). Single-use cells kill cell-level ABA; segments recycle through a
  shard free-list reclaimed under QSBR keyed on the worker set.
- **C — Reversing Treiber Mailbox (LIFO CAS push / FIFO-drain hybrid).** Producers
  prepend with one acq_rel CAS on a packed head word; the single owner detaches the
  whole chain with one acquire `exchange` and reverses it in place into a private
  FIFO drain list. Prepend-only push is ABA-immune; the reversal is paid
  thread-locally off the contended line.

## Evidence table

Claim kinds: **F** = fast, **S** = safe, **C** = correct. *Survived* = survived
red-team cross-examination without being conceded/withdrawn. *Proven* = the
executed-C++ verdict (CORRECT / WRONG / DISPROVEN). Numbers are from the executed
evidence: Design A on a 32-core x86-64 @ 2.095 GHz (pinned, warmup discarded);
Designs B/C on a Xeon Silver 4208 @ 2.1 GHz (non-isolated, shared host — tails are
jitter-sensitive there). g++ 14.2 / clang 20.1, `-std=c++23`.

### Design A — Intrusive Vyukov (exchange-published)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free, wait-free enqueue | yes | **CORRECT** | asm = 1 `lock xchg` + 2 plain stores + `ret`, **no loop label** (g++ & clang); 0 new, 0 pmr, 0 retries at P=1..128 (8M each) |
| F2 no collapse under contention, beats CAS-loop | yes | **CORRECT** | enqueue **34 Mops/s @ P=32** (g++); P=64 ≥ P=16 (no turn-over); > CAS-loop at every P (e.g. 34 vs 8.7 @ P=32) |
| F3 local tell latency in budget | yes | **CORRECT** | SPSC ping **p50 = 40–42 ns** (goal 100 / ceiling 250), p99 49–58 ns, p999 103–142 ns; occ-1 + cross-socket producer p999 14–17 µs (within 50 µs ceiling) |
| S1 no race/UB incl. `head_` worker handoff | yes | **CORRECT** | TSan clean 10.0M msgs (rotating consumer); ASan/UBSan clean 5.0M; **positive control**: relaxed exec-CAS → TSan `head_` race in `try_dequeue` every run |
| S2 ABA-free by construction | yes | **CORRECT** | 16M same-address-reuse @ P=32: dup=0, missing=0, stale=0 |
| C1 strict FIFO + single-in-flight, no loss | yes (after close-out fix) | **CORRECT** | 16M inversions=0, dispatched+reclaimed==produced; **drop-window control**: revised close-out dropped **0/200k**, original broken close-out dropped **200k/200k**; suspended-A → B not dispatched |
| C2 tombstone skipped/freed exactly once | yes (after epoch fix) | **CORRECT** | 2M 50%-cancelled: free_count==1, cancelled_handler_ran=0; epoch-guarded 8M clean; **contrast**: bare `Descriptor*` cancel → ASan heap-use-after-free |
| C3 Busy≠Empty, no strand, no lost wakeup | yes | **CORRECT** | 800k boundary probes → Busy every time, empty_misread=0, stranded=0; 20M park/enqueue races → lost_wakeups=0 |

**Disproven/conceded: none. 8/8 survived and proven CORRECT.** Two originally-fatal
defects (the mutating `try_dequeue` in the Empty→release recheck window, dropping a
node; and touching `head_` after releasing exec-ownership → two-consumer race) were
conceded, fixed with a **stated cheap fix** (read-only `tail_` probe + reacquire
ownership before any mailbox mutation), and the fix was *proven* by the executed
drop-window and TSan controls above.

### Design B — Segmented FAA + QSBR

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 zero heap alloc/msg (incl. reclamation bookkeeping) | yes | **CORRECT** | seg allocs FLAT at 2 and global-new delta=0 across 1M→50M msgs (CAP=64 forced turnover); intrusive free-list + retire list |
| **F2 scales ≥0.6·N into one mailbox** | **NO — conceded** | **DISPROVEN** | single `fetch_add` line **122→40 Mops/s from N=1→N=8** (~0.04× floor); scaling headline withdrawn |
| F2r drain path 0 cross-core RMW | yes | **CORRECT** | drain-path mailbox-RMW count = 0 over 10M drains (both compilers); objdump = plain `mov` acquire loads, no lock prefix |
| F3 same-shard sequential latency | yes (uncontended) | **CORRECT** | quiet-core p50 ~20–40 ns, p99 raw 220–246 ns (<250 on quiet cores), p999 1.6 µs; p999 ≤50 µs **not** claimed under oversubscription |
| S1 no race/UB (corrected reinit) | yes (after reinit fix) | **CORRECT** | TSan clean (CAP=8 constant recycle, 20% cancels); ASan/UBSan clean |
| S2 no ABA / UAF on recycle | yes (after reinit + gen + epoch-enqueuer) | **CORRECT** | QSBR-on clean 100M; **positive controls fire**: no-QSBR → UAF at `seg->next` CAS; unguarded non-worker producer → UAF at `fetch_add` |
| S3 no lost wakeup | yes | **CORRECT** | 10M produced==consumed, stranded=0, lost_wakeup_events=0 |
| C1 strict FIFO | yes | **CORRECT** | 4M multi-producer, per-pid inversions=0 (>1000 segment boundaries) |
| C2 no loss/dup | yes | **CORRECT** | 8M bitset exact: missing=0, dup=0 (forced recycle) |
| C3 tombstone exactly once, gen-gated cancel | yes | **CORRECT** | stale cross-gen cancel rejected, new msg dispatched, double-free=0 |

**Disproven/conceded: F2** (its marquee producer-scaling claim; a single FIFO ticket
line serialises and *regresses* with N). Peak *sequential* drain throughput is the
highest measured of any design (56–59 M msg/s/core), but that is the consumer path,
not the contended producer path.

### Design C — Reversing Treiber

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| **F1 1-CAS + 0-alloc + ≥0.8·N scaling + p50≤100/p99≤250** | partial | **WRONG** | 1 `lock cmpxchg` ✓ and 0 alloc/msg ✓, but **aggregate enqueue collapses** 57.98→4.69 Mops/s from N=1→N=16 (**0.08× vs required 0.8·N**) and 1P/1C **p50=446 ns, p99=994 ns** (budget 100/250 ns) |
| F2 O(1) drain atomics/batch, ≥20M/s/core floor | yes | **CORRECT** | 37.5 M msg/s/core single-thread; detach/msg → 0.0003 as batch grows; **caveat proven**: p999 400 µs–115 ms under *unbounded* overload (single consumer < producer; no bound exercised) |
| S1 no race/UB (incl. weak-memory sub-claim) | yes (after atomic-link fix) | **CORRECT** | TSan clean 10.4M — but **only after fixing the sketched plain `mbox_next` store, which TSan flagged as a real read/write race**; ARM/herd7 sub-claim NOT tested |
| S2 tag-free ABA-immunity (link scope) | yes (scope narrowed) | **CORRECT** | 2.4M heavy-recycle: dup_link=0, reentry=0, double_pop=0; single-consumer reentrancy guard never fired under work-stealing |
| C1 per-sender FIFO + single-executor under async | yes (after drain-loop fix) | **CORRECT** | 1.6M in-order; async no-overlap: msg k+1 starts strictly after k ends, fifo head frozen while suspended |
| C2 no loss/dup, gen-checked cancel | yes | **CORRECT** | 2M: free==enq, double-free=0, stale cancels are no-ops |
| C3 no lost wakeup, ≤1 Scheduled | yes | **CORRECT** | **exhaustive** reachable-state model check NP=2..4 all PASS; eager-release stress stranded=0 |

**Disproven/conceded: F1** (both halves — scaling collapse *and* latency miss). Also
required fixing a **real data race in the sketched code** (plain intrusive-link
store) and a **single-executor violation** (drain loop advanced past a suspended
async handler) before S1/C1 held.

## Decision

**Winner: Design A — Intrusive Vyukov MPSC (exchange-published).**

Rationale, in the required ranking order:

1. **Safety gate — all three pass; A passes cleanest.** No design has a *safe* or
   *correct* claim marked WRONG in the final verdicts (Design C's only WRONG is F1,
   a *fast* claim). Every S/C claim across all three designs proved CORRECT. A backs
   its safety with a **load-bearing positive control**: relaxing the exec-state CAS
   makes TSan report a `head_` race every run, proving the release/acquire handoff is
   the *actual* synchronization, not incidental. Critically, A's two originally-fatal
   defects had a **stated cheap fix** whose effectiveness was itself executed and
   proven (200k/200k dropped on the broken close-out → 0/200k on the revised one), so
   the gate's "unless a cheap fix exists" escape applies and is discharged with
   evidence, not a promise.

2. **Proven beats claimed — A is the only clean sweep.** A = **8/8 survived and
   proven CORRECT, 0 disproven.** B lost its F2 producer-scaling claim (measured
   122→40 Mops/s regression) — 9 proven / 1 disproven. C had F1 proven **WRONG**
   (scaling collapse *and* latency miss) — 6 proven / 1 disproven — *and* shipped a
   sketch that data-raced and bent the single-executor invariant until patched.
   INCONCLUSIVE items (the un-run AArch64/herd7 sub-experiments) carry no weight for
   any design.

3. **Best measured hot-path numbers among safe survivors.** The mailbox hot path is
   defined by multi-producer enqueue and local-tell latency. A sustains **34 Mops/s
   at P=32 with no collapse to P=128 and ~4× the CAS-loop**, with **p50 = 40–42 ns
   and a tight tail (p99 ≈ 50 ns)**. B's contended producer path *regresses* (single
   FAA line) — its strength is drain throughput (56–59 M/s/core), the consumer side,
   not the thing the mailbox contends on — and its latency tail is looser (p99 raw
   220–246 ns). C's enqueue *collapses* (0.08× at 16 cores) and misses the latency
   budget outright (p50 = 446 ns). A wins the metric that matters and owns the
   tightest tail.

4. **No core invariant bent (in the winning, fixed form).** A upholds
   at-most-one-executor (consumer-private `head_` fenced by the exec-state CAS;
   the revised close-out never touches `head_` without ownership), FIFO-by-default
   (the `tail_` exchange modification order), and zero-alloc (proven by disassembly).
   Its one true coupling — Vyukov emptiness is non-linearizable, so wakeup rides the
   exec-state machine (002 §Wakeup), never mailbox emptiness — is a *documented
   protocol obligation*, exactly how 002 already works, not a violation. By contrast
   C's original drain loop *broke* single-executor for async handlers and its LIFO
   push inverts physical order (oldest-of-burst waits for the whole reversal).

**Runner-up: Design B**, purely for its **consumer/drain path** — the RMW-free
batch sweep (F2r proven: 0 cross-core RMW on drain; 56–59 M msg/s/core peak) is the
fastest measured drain and is worth harvesting as a future deep-backlog draining
optimization behind the same handle contract. It is not the winner because its
*producer* hot path does not scale (F2 disproven) and it carries materially more
machinery (QSBR liveness dependency, ingress epoch reservations for non-worker
producers, 64 B/cell → ~4× drain bandwidth, a proven head-of-line stall on producer
preemption that it explicitly does not claim to keep within the p999 ceiling).

### Confidence and the one residual tie-breaker

The decision is **decisive on x86-64** and is now the *second independent evidence
round* to reach it. The single experiment that could still move the *margin* (not the
winner): **A vs. B on a weakly-ordered AArch64 box, with the exec-state
release-sequence handoff encoded as a herd7/GenMC litmus.** herd7/GenMC were absent
on the host, so all sanitizer evidence is x86-TSO. A's contention advantage is
expected to *widen* on ARM (exchange vs. CAS-retry storms), but the formal
weak-memory backing of the `head_` handoff and the lost-wakeup edges is unproven by
tool. This bounds the cross-platform latency margin; it does not threaten the winner
on the tested platform.

## Spec-update recommendations

See `specRecommendations` in the structured output. Summary:

- **003-Memory.md** — Fix the MPSC as an intrusive Vyukov queue: an 8-byte
  `MailNode { std::atomic<MailNode*> next; }` as the **first member** of
  `Descriptor` (pointer-interconvertible; guarded by `is_standard_layout_v` +
  `offsetof(...)==0` + `is_pointer_interconvertible_with_class`). State the
  **single-membership** invariant (a descriptor is in the mailbox at most once).
  Upgrade `MessageHandle` to **`{Descriptor*, u32 generation}`** and make
  cancellation *generation-gated* (cancel writes only if `handle.gen ==
  descriptor.gen`; `release()` bumps the generation) — the executed contrast proved
  a bare `Descriptor*` cancel racing reclaim is a use-after-free of pooled memory.
  Note the unbounded queue delegates backpressure to 022.
- **002-Scheduler.md** — In §Wakeup, state normatively that wakeup/deschedule ride
  the exec-state `Idle→Scheduled` / `Running→Idle` machine and **never** mailbox
  emptiness (Vyukov emptiness is non-linearizable). Add `Busy` as a third drain
  result and require **bounded-spin-then-`Running→Scheduled`** (never unbounded
  spin). Specify the release close-out precisely: on `Empty`, `store(Idle, release)`
  then a **read-only `tail_` probe**; only on a non-empty/`Busy` probe re-acquire via
  `CAS(Idle→Running, acquire)` **before any `head_` mutation**. Add a note that the
  mutating `try_dequeue`-in-window variant *loses and leaks a message* (executed:
  200k/200k dropped) and must never be reintroduced.
- **001-Actor-Execution-Model.md** — In §Mailbox, name the exec-state CAS memory
  orders as **load-bearing** (release on release-ownership, acquire on
  acquire-ownership; they carry the consumer-private `head_` across worker handoff).
  In §Hybrid handler execution, require the drain loop to **not advance the mailbox**
  past a suspended handler for `Sequential` actors (dispatch returns
  Completed/Suspended; park on Suspended — Design C's proven single-executor bug was
  advancing on Suspended).
- **015-Reentrancy-and-Quiescence.md** — State that the single-executor drain
  invariant holds across async completion: a completion **re-schedules through the
  admission gate**, it does not re-enter `drain()` on the completing thread. Tie the
  drain-budget `Paused` seal to the `Busy`/bounded-spin park path so consumer
  progress cannot collapse under same-core oversubscription.

## Residual risks

1. **The revised release close-out is the load-bearing correctness fix.** A future
   refactor that reverts to a mutating `try_dequeue` in the Empty→release recheck
   window silently loses and leaks messages (executed: 200k/200k). Encode the
   read-only-`tail_`-probe + reacquire-before-mutate normatively (002) and keep a
   regression test.
2. **`head_` handoff safety is entirely load-bearing on the exec-state CAS orders.**
   Downgrading to `relaxed` reintroduces a stress-only race. The relaxed
   positive-control test must be a **permanent CI regression guard**.
3. **Weak-memory (AArch64) not machine-checked.** herd7/GenMC absent; ARM margin and
   the lost-wakeup/publication litmus are unproven by tool. Run them before
   committing to cross-platform latency numbers.
4. **Cancellation requires the epoch-stamped handle.** A bare flag-flip on a
   `Descriptor*` is a proven use-after-free against a late cancel racing reclaim; the
   `{Descriptor*, generation}` gate is mandatory, not optional.
5. **Non-linearizable emptiness couples the mailbox to the exec-state protocol.** Any
   code path wiring wakeup to mailbox emptiness reintroduces lost wakeups (002 §Wakeup).
6. **Unbounded queue = no backpressure.** A runaway producer exhausts the shard
   descriptor pool; admission/overload control is out of mailbox scope (022) and must
   exist as a companion policy.
7. **Occupancy-0/1 consumer-side `tail_` exchange (stub re-arm).** At the design's own
   steady-state occupancy the last-node dequeue does a consumer-side `tail_.exchange`,
   so under a concurrent cross-socket producer the round-trip p999 inflates to 14–17 µs
   (within the 50 µs ceiling, over the 5 µs goal). Real, measured, tail-latency only.
8. **Single-membership is a global lifecycle invariant** the mailbox cannot cheaply
   check on the hot path; a double-tell of one descriptor corrupts the intrusive
   chain. Guaranteed by descriptor allocation lifecycle + a debug assertion only.
