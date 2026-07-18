# ADR-001 — Mailbox MPSC hot path

- **Status:** Accepted
- **Date:** 2026-07-13
- **Scope:** The per-actor Mailbox — the MPSC queue that owns FIFO ordering,
  stores fixed-size `MessageHandle`s (never payloads), is enqueued by many
  producer threads and drained by exactly one worker (single-executor invariant).
- **Related specs:** `001-Actor-Execution-Model.md`, `002-Scheduler.md`,
  `003-Memory.md`, `015-Reentrancy-and-Quiescence.md`, `017-Delivery-Guarantees.md`.

## Question

Which mailbox design gives Quark a (fast) allocation-free, contention-scaling hot
path, (safe) data-race/UB/ABA-free operation under TSan+ASan+UBSan, and (correct)
strict FIFO with no lost/duplicated handles and tombstones skipped exactly once —
without bending a core invariant (at-most-one-executor, FIFO-by-default, no
heap on the steady hot path, no data race under sanitizers)?

## Candidate designs (one-line summaries)

- **A — Intrusive Vyukov MPSC (exchange-published, single-consumer, tombstone-lazy).**
  Producers publish with an unconditional atomic `exchange` on `tail_` (not CAS),
  so enqueue is wait-free per producer and ABA-free by construction. The single
  consumer's `head_` is a plain pointer whose cross-worker handoff rides the
  exec-state release/acquire. The Vyukov transient window (exchange done, link
  store pending) is surfaced as an explicit `Busy` result distinct from `Empty`.
- **B — Segmented single-use-slot MPSC (per-segment fetch-add + batch sweep).**
  Producers `fetch_add` a per-segment ticket, store the handle, release-publish a
  slot generation; consumer sweeps contiguous cache-friendly slots. Segments
  recycled via a shard freelist reclaimed by EBR/QSBR.
- **C — Treiber-Reverse (LIFO CAS push, single-consumer reverse-to-FIFO drain).**
  Producers push with a release-CAS onto a shared head; the consumer takes the
  whole chain with one acquire-`exchange` and reverses it into a FIFO list.
  Tag-free ABA-immunity, at the cost of an O(batch) reverse before dispatch.

## Evidence table

Claim kinds: F = fast, S = safe, C = correct. "Survived" = survived red-team
cross-examination (not conceded/withdrawn). "Proven" = executed-C++ verdict.
Numbers are best-of-3 on a 32-core x86-64 box, g++ 14.2 / clang 20.1.

### Design A — Vyukov exchange-published

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free, wait-free enqueue | yes | CORRECT | asm = `movq $0; xchg; mov; ret` — 1 exchange + 1 store, **no loop label**; 0 allocs, 0 retries at P=1..128 (8M each) |
| F2 scales under contention, no collapse | yes (x86 only) | CORRECT | enqueue **28.3 Mops/s @ P=32** vs CAS-loop 9.8, mutex 8.9; flat 16→64 (no turn-over); p99.9 plateaus (8565ns@16 → 5762ns@64) |
| S1 no race/UB incl. worker handoff | yes | CORRECT | TSan clean 60s/14.9M msgs; ASan/UBSan clean 6M; **positive control**: relaxed exec-CAS → TSan race on `head_` every run |
| S2 ABA-free (exchange, never compare) | yes | CORRECT | 16M same-address-reuse ops: dequeued=16M, missing=0, dup=0, fresh_fail=0 |
| C1 strict FIFO + single-in-flight drain | yes | CORRECT | 16M msgs inversion=0, dup=0, missing=0; coroutine test: B not dispatched while A's task suspended |
| C2 tombstone skipped/freed exactly once | yes | CORRECT | 8M msgs, skip==\|C\| exact, dispatched==M−\|C\| exact, ASan no double-free |
| C3 Busy≠Empty at boundary, no strand | yes (after 2-line fix) | CORRECT | whitebox: 100k boundary probes → Busy=100k, empty-misread=0; delay-injection 800k → 0 stranded |

Disproven/conceded: **none.** 7/7 survived and proven CORRECT.

### Design B — Segmented fetch-add

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free hot path, bounded segments | yes | CORRECT | 1e8 cycles: hot-path new=0, steady OS-seg-alloc delta=0, live segs ≤3 |
| **F2 scales under producer contention** | **NO — conceded** | **DISPROVEN** | single ticket line **regresses to 0.31× at P=8** (131→52→43→40 Mops/s); intra-mailbox scaling withdrawn |
| F3 backlog drain low-ns | yes (backlog only) | CORRECT | **2.82 ns/handle = 354 M handles/s** on a full non-tail segment |
| S1 no race/UB | yes (after 2 fixes) | CORRECT | TSan clean 8.7–9.0M; ASan/UBSan clean 70–78M |
| S2 UAF-free reclaim (incl. non-worker producers) | yes (after EBR) | CORRECT | negative control (EBR off) → ASan UAF at `fetch_add` exit 134; EBR-on clean |
| S3 no ABA / stale-handle on recycle | yes (gen, no reset) | CORRECT | 2.4M msgs heavy recycle: 0 stale/dup, publish_stores=K, reset_stores=0 |
| C1 strict FIFO by claim order | yes | CORRECT | 2.4M msgs, ~2343 boundaries, inversions=0 |
| C2 no loss/dup + no lost wakeup | yes (seq_cst fence) | CORRECT | 4M multiset exact; 20M park/enqueue races, lost_wakeups=0 (x86-TSO; AArch64 litmus **not** machine-checked) |
| C3 tombstone exactly once | yes | CORRECT | 1M msgs ~50% cancelled, free_count==1, cancelled_handler_ran=0 |

Disproven/conceded: **F2** (its marquee scaling claim; measured regression, not a plateau).

### Design C — Treiber-Reverse

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free sync+async (pmr coro frames) | yes (after 2 fixes) | CORRECT | 1e7 sync+async: global-new delta=0, pmr-upstream delta=0; async thunk emits no `operator new` |
| F2 bounded advantage over mutex (not scaling) | yes (scaling retired) | CORRECT | **8.55 Mops/s @ P=8** > mutex 6.17; p99 = 0.26× MS-queue; plateaus ~8.5–10 Mops/s |
| **F3 reverse is cache-warm, <20% of drain** | **NO — conceded** | **DISPROVEN** | inverts under cross-socket producers: serialized coherence-miss pointer chase run before oldest dispatches |
| S1 no race/UB incl. reentrant completions | yes (after redesign) | CORRECT | TSan + ASan/UBSan clean, 32 producers + rotating consumer + MaxConcurrency<4> |
| S2 tag-free ABA-immunity | yes (after fix) | CORRECT | 4.8M MRU-recycle ops: missing=0 dup=0 torn=0, max_concurrent_drainers=1 |
| C1 per-sender FIFO | yes (bounded to actor model) | CORRECT | 3.2M msgs fifo_violations=0 |
| C2 no loss/dup, no lost wakeup, ≤1 executor | yes (fatal fix) | CORRECT | 1.6M msgs stranded=0, max_concurrent_drainers=1 |
| C3 tombstone exactly once, race-free cancel | yes (CAS fix) | CORRECT | invoke_after_cancel=0, dup=0, missing=0, sanitizers clean |

Disproven/conceded: **F3** (the reverse-cost claim). Also required a **fatal** fix
to the at-most-one-executor invariant under Reentrant (double-consumer via
completion re-entering `drain()`), fixed and re-proven.

## Decision

**Winner: Design A — Intrusive Vyukov MPSC (exchange-published).**

Rationale, in the ranking order:

1. **Safety gate — all three pass.** No design has a safe claim marked WRONG in
   the final verdicts; every S-claim is proven CORRECT. A alone backs its safety
   with a *load-bearing positive control*: downgrading the exec-state CAS to
   `relaxed` makes TSan report a `head_` race every run, proving the release/acquire
   handoff is the actual synchronization and not incidental.

2. **Proven beats claimed.** A is the **only design with a clean sweep — 7/7 claims
   survived red-teaming and were proven CORRECT, 0 disproven.** B lost its F2
   scaling claim (measured 0.31× regression at P=8). C lost F3 (reverse cost) and
   needed a *fatal* fix to the core at-most-one-executor invariant before its
   correctness claims held.

3. **Best measured hot-path numbers.** The mailbox hot path is dominated by
   multi-producer enqueue. A sustains **28.3 Mops/s at P=cores (~3× the CAS-loop
   and mutex baselines) and does not collapse past hardware threads**; enqueue is a
   single `lock xchg` + one store with zero retries and p99.9 that plateaus rather
   than inflating. B's enqueue *regresses* under contention (single hot ticket
   line, 40 Mops/s and falling — its win is consumer drain, not the contended
   producer path). C's enqueue plateaus at ~8.5–10 Mops/s (single contended head
   line, bounded-contention, explicitly not scaling).

4. **No core invariant bent.** A upholds at-most-one-executor (consumer-private
   `head_` fenced by the exec-state CAS), FIFO (exchange linearization point), and
   zero-alloc (proven by disassembly). Its one true coupling — emptiness is not
   linearizable, so wakeup must ride the exec-state machine per 002 — is a
   *documented protocol obligation*, already how 002 §Wakeup works, not an
   invariant violation. By contrast C's LIFO push means physical order is reversed
   and the oldest message of a burst cannot dispatch until the whole batch is
   reversed (tail-latency inversion), and its correctness leaned on an invariant
   (single-consumer) that broke under Reentrant until patched.

**Runner-up: Design B** for the *consumer/drain* path — its 2.82 ns/handle
contiguous batch sweep is the fastest measured drain and is worth harvesting as a
future optimization for deep-backlog draining. It is not the winner because its
producer hot path (the thing the mailbox contends on) does not scale.

### Confidence and the one residual tie-breaker

The decision is **decisive on x86-64**. The single experiment that could still
move it: **A and B on a weakly-ordered ARM (AArch64) box.** A's F2 contention
advantage is expected to *widen* on ARM (exchange vs. CAS-retry storms), but
neither A's ARM breadth sub-claim nor B/C's AArch64 lost-wakeup litmus (herd7/GenMC)
was machine-checked — the tools were absent. This does not threaten the winner on
the tested platform; it bounds the *margin* and the formal weak-memory backing.

## Spec-update recommendations

See the `specRecommendations` in the structured output; summary:

- **003-Memory.md** — Add a "Mailbox structure" subsection fixing the MPSC as an
  intrusive Vyukov queue: a 8-byte `MailNode { std::atomic<MailNode*> next; }` as
  the **first member** of `Descriptor` (pointer-interconvertible; guarded by
  `static_assert(is_standard_layout_v<Descriptor>)` +
  `is_pointer_interconvertible_with_class`). State the single-membership invariant
  (a descriptor is in the mailbox at most once) and note the unbounded queue
  delegates backpressure to 022.
- **002-Scheduler.md** — In §Wakeup, state normatively that wakeup/deschedule ride
  the exec-state `Idle→Scheduled` / `Running→Idle` machine and **never** mailbox
  emptiness (Vyukov emptiness is non-linearizable). Specify the consumer
  double-check barrier: on `Empty`, CAS `Running→Idle` then re-check `try_dequeue`
  once; on `Got`/`Busy` re-arm `Scheduled`. Add `Busy` as a third drain result and
  require bounded-spin-then-`Running→Scheduled` reschedule (never unbounded spin)
  so consumer progress can't collapse under same-core oversubscription.
- **001-Actor-Execution-Model.md** — In §Mailbox, name the exec-state CAS memory
  orders as load-bearing (release on release-ownership, acquire on acquire-ownership;
  they carry `head_` across worker handoff). In §Hybrid handler execution, require
  the drain loop to **not advance the mailbox** past a suspended handler for
  `Sequential` actors (dispatch returns Completed/Suspended; park on Suspended).
- **015-Reentrancy-and-Quiescence.md** — Add that the single-executor drain
  invariant holds across async completion: a completion **re-schedules through the
  admission gate**, it does not re-enter `drain()` on the completing thread. Tie
  the drain-budget `Paused` seal to the `Busy`/bounded-spin park path.

## Residual risks

1. **Producer preemption in the exchange→link-store window** stalls the consumer
   (Busy) behind an already-linked tail; Vyukov gives the consumer no wait-free
   escape. Mitigated by bounded-spin-then-reschedule; real under heavy
   oversubscription. Tail-latency, not correctness.
2. **head_ handoff safety is entirely load-bearing on the exec-state CAS orders.**
   A future refactor to `relaxed` reintroduces a stress-only race. The relaxed
   positive-control test must be a permanent CI regression guard.
3. **Weak-memory (ARM) not machine-checked.** A's ARM margin and the lost-wakeup
   litmus are unproven by tool; run herd7/GenMC + an AArch64 stress box before
   committing to cross-platform latency numbers.
4. **Non-linearizable emptiness couples the mailbox to the exec-state protocol.**
   Any code path wiring wakeup to mailbox emptiness reintroduces lost wakeups.
5. **Unbounded queue = no backpressure.** A runaway producer exhausts the shard
   descriptor pool; admission/overload control is out of mailbox scope (022) and
   must exist as a companion policy.
6. **Model-checking (CDSChecker/Relacy) of S2/C3 was not run** (tool absent);
   those rest on same-address-reuse stress + a deterministic white-box probe.
