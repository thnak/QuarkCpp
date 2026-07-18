# ADR-003 — Mailbox MPSC hot path (evidence round 3: REX challenge)

- **Status:** Accepted
- **Date:** 2026-07-14
- **Confirms:** [ADR-002](ADR-002-mailbox-mpsc-hot-path-r2.md) (and ADR-001). The
  winner is **unchanged** for the third independent evidence round. What is new:
  a fresh challenger (**REX-Mailbox**) was built, red-teamed, and proven against
  the incumbent; and two load-bearing **memory-order corrections** plus one
  **cross-compiler portability fix** for the incumbent were discovered by
  executed C++ this round and are promoted to normative spec changes.
- **Scope:** The per-actor Mailbox — the MPSC queue that owns FIFO ordering,
  stores fixed-size `MessageHandle`s (never payloads), enqueued by many producer
  threads, drained by exactly one worker (single-executor invariant).
- **Related specs:** `001-Actor-Execution-Model.md`, `002-Scheduler.md`,
  `003-Memory.md`, `015-Reentrancy-and-Quiescence.md`, `022`, `023`.

## Question

Which mailbox design gives Quark a **(fast)** allocation-free hot path that does
not collapse under producer contention, **(safe)** data-race / UB / ABA-free
operation under TSan+ASan+UBSan, and **(correct)** strict FIFO with no lost or
duplicated handles and tombstones skipped exactly once — without bending a core
invariant (at-most-one-executor, FIFO-by-default, no heap on the steady hot path,
no data race under sanitizers)? This round asks specifically: **does any new
design clear the bar to reopen the Accepted ADR-002 incumbent?**

## Candidate designs (one-line summaries)

- **A — Intrusive Vyukov MPSC (exchange-published, stub-anchored, gen-gated)**
  *[incumbent].* The queue node *is* the pooled `Descriptor` (8-byte
  `MailNode next` as its first, pointer-interconvertible member). Producers publish
  with one unconditional `tail_.exchange` + one link store — wait-free per producer,
  ABA-free by construction (never compares an address). Single consumer walks a
  plain consumer-private `head_`; its handoff rides the actor exec-state CAS. The
  transient `exchange→link` window is a third drain result `Busy`. Cancellation is a
  generation-gated tombstone over type-stable pool memory.
- **B — REX-Mailbox (Reversing Exchange-Stack)** *[challenger].* Producers publish
  onto a **LIFO** stack with one unconditional `head_.exchange` (not a CAS loop — the
  fix for ADR-002 Design C's collapse). The single owner detaches the whole backlog
  with one acquire `exchange(nullptr)` and **reverses** it thread-locally into a
  private FIFO drain list. Marketed wins: linearizable emptiness on the push side and
  a one-atomic bulk detach.
- *SegRing (Segmented FAA ring)* was tabled: it produced a design but no
  cross-examination survivors and **no executed evidence** this round, so it does not
  advance. (Its lineage lost as Design B in ADR-002 on producer scaling.)

## Evidence table

Claim kinds **F**/**S**/**C** = fast/safe/correct. *Survived* = survived red-team
cross-examination without being conceded/withdrawn. *Proven* = executed-C++ verdict.
Numbers from executed evidence, x86-64 32-core pinned, ~2.095/2.10 GHz (throttled;
ratios are machine-independent, absolute ns are conservative vs the 3–4 GHz 023
reference). g++ 14.2 / clang 20.1, `-std=c++23`, TSan+ASan+UBSan.

### Design A — Intrusive Vyukov (incumbent, re-proven with round-3 fixes)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 alloc-free, wait-free enqueue | yes | **CORRECT** | asm = 1 `xchg` + 2 stores + `ret`, no loop (both compilers); 0 new / 0 pmr / 0 retries, P=1..128, 8M each |
| F4 drain does 0 cross-core RMW | yes | **CORRECT** | objdump of `try_dequeue` loop: `grep lock/xchg/cmpxchg = 0` on both compilers; `head_` = plain `mov`; 10M drain clean |
| **F2 producer scaling** | **NO — "beats CAS-loop at every P" withdrawn** | **WRONG (overclaim)** | exchange loses to CAS-loop at P=1/P=2 (uncontended). Revised floor holds: ≥20 Mops for P≥4 (min 21.8), no collapse P16→P128 (32→25), dominates CAS-loop for P≥4 |
| F3 local-tell latency (023 budget: sequential same-shard) | yes (scoped) | **CORRECT** | **p50=31 p99=44 p999=91 ns** (budget 100/250/50000). Cross-core occ-1 p50=241/p999=5051 ns = inter-core physics, within 50µs ceiling |
| S1 no race/UB incl. `head_` handoff | yes | **CORRECT** | TSan 4/4 clean (rotating consumer, arena/NullPool); ASan/UBSan clean 5M; **control**: relaxed exec-CAS → `head_` race in `try_dequeue` every run |
| S2 ABA-free by construction | yes (x86-TSO) | **CORRECT** | static audit: no `compare_exchange` on `tail_`/`MailNode*`; 16M same-addr reuse → dup/miss/stale=0. Weak-memory acq_rel-vs-release distinction NOT tool-proven |
| S3 gen-gated cancel = defined no-op, not UAF | yes (guard fix) | **CORRECT** | 8M cancel-vs-reclaim ASan clean; produced 8M = dispatched 7.64M + tombstoned 0.36M; **control**: bare `Descriptor*` cancel → 2× heap-use-after-free. Compile guard needs `#ifdef` for clang |
| C1 strict FIFO, no loss/dup | yes | **CORRECT** | 16M, K=8/16: inversions=0, dup=0, missing=0, produced==dispatched |
| C2 tombstone skipped/freed exactly once | yes | **CORRECT** | 2M 50%-cancel: free_count==1, handler_ran=0, double-free=0, lingering-anchor=1 |
| C3 no lost wakeup / Busy≠Empty | yes (**seq_cst fix**) | **CORRECT** | seq_cst rendezvous → lost_wakeups=0; **control**: release/acquire StoreLoad → 41–48% lost (dekker 144807/300000); 800k Busy probes, mis-Empty=0 |
| C4 close-out loses zero nodes | yes | **CORRECT** | rotating workers, 2M: dups=0, missing=0, 6/6 runs; **control**: mutating recheck → drops 1–22 nodes/run |

**Design A: 10/11 survived and proven CORRECT; 1 WRONG — F2, a *fast* overclaim
("beats CAS-loop at every P"), whose safety/correctness-neutral revised form
(saturation-without-collapse, ≥20 Mops for P≥4) is proven.** No safety/correctness
claim WRONG.

### Design B — REX-Mailbox (challenger)

| Claim | Survived? | Proven? | Number / evidence |
|---|---|---|---|
| F1 exchange-push, no collapse, ≈parity w/ Vyukov | yes | **CORRECT** | asm = 1 `xchg` / 0 cmpxchg; producer-scale FLAT ~44 Mops @ P=8/16/32, no turn-over; REX ≥ Vyukov in its own harness (min ratio 121%, no drain coupling) |
| F2 1 RMW / batch, 0 RMW/msg on serve path | yes (scoped) | **CORRECT** | serve loop objdump = 0 lock-prefixed; detach-atomics/msg = 1/B → 0 as B grows; drain ≥20 Mops at B≥64 (124/62/47). **At B=1 (latency op-point) = 1 RMW/msg = 17 Mops (below floor by construction)** |
| F3 local-tell latency | yes | **CORRECT** | folded sentinel = 2 stores. Same-core sequential **p50=38 p99=50 p999=147 ns** (meets budget). Cross-core SPSC p50=225/p999=525 ns (beats Vyukov cross-core) |
| S1 no race/UB | yes (weak-mem **INCONCLUSIVE**) | **CORRECT** | TSan clean 3M rotating consumer + ASan/UBSan 5M; **control**: relaxed handoff → 4 races on `fifo_/pending_`. Controls on (B)/(E) relaxed stayed clean on x86-TSO → weak-memory sub-claim needs AArch64/herd7 |
| S2 ABA-free (found+fixed cancel TOCTOU) | yes | **CORRECT** | push/detach never compare; 8M same-addr reuse dup/miss=0; **required a fix**: gen+state must be ONE packed CAS (naive 2-step TOCTOU) |
| C1 strict FIFO via reversal | yes | **CORRECT** | ≤5M rotating consumer, injected Busy: inversions=0, dup=0, missing=0 |
| C2 tombstone exactly once | yes | **CORRECT** | 2M 50%-cancel: free_count==1, handler_ran=0, double-free=0 (atomic dispatch-claim resolves race) |
| C3 linearizable emptiness; mid-gap no loss/reorder | yes (**tail conceded**) | **CORRECT (correctness only)** | 1M gap events: Busy every time, Empty-with-live=0, order_bug=0, loss=0. **Conceded tail**: one mid-gap producer froze 3 ready newer msgs ~708µs (≫ 5µs ceiling) — head-of-line block of the *whole actor* |

**Design B: 8/8 survived and proven CORRECT; 0 disproven.** But S1's weak-memory
sub-claim is **INCONCLUSIVE** (carries no weight), and C3's correctness came bundled
with a **conceded, executably-confirmed structural tail defect**.

## Decision

**Winner: Design A — Intrusive Vyukov MPSC. ADR-002 is confirmed for a third round.
Design B (REX) does not clear the bar to reopen it.**

Rationale, in the required ranking order:

1. **Safety gate — both pass; neither has a safe/correct claim WRONG.** Design A's
   only WRONG verdict is F2, a *fast* overclaim, and its revised form is proven.
   Design A's two round-3 memory-order defects (release/acquire on the wakeup
   StoreLoad; release-only enqueue exchange) each have a **stated cheap fix**
   (seq_cst rendezvous; acq_rel exchange) whose effect is executed and proven — the
   dekker control shows release/acquire loses **~48% (144807/300000)** of wakeups
   and seq_cst loses **0/300000**. The gate's "unless a cheap fix exists" escape is
   discharged with evidence, not a promise. Design B is also safety-clean, but its S1
   weak-memory sub-claim is INCONCLUSIVE and its S2 needed a TOCTOU fix.

2. **Proven beats claimed — A carries more proven weight.** A = **10 survived +
   proven CORRECT, 1 disproven** (fast overclaim). B = **8 survived + proven CORRECT,
   0 disproven** — but one of B's eight (S1) is weakened by an INCONCLUSIVE
   weak-memory sub-claim, and B's headline structural wins were shown *operationally
   inert* under cross-examination (it still returns `Busy` and still rides the
   exec-state machine for wakeup, so "linearizable emptiness" removes none of the
   incumbent's machinery). INCONCLUSIVE items carry no weight for either design.

3. **Best measured hot-path numbers on the budget-defining path.** The 023 local-tell
   budget is the **sequential same-shard** path. A wins it: **p50=31 / p99=44 /
   p999=91 ns** vs B's **p50=38 / p99=50 / p999=147 ns**. And A's steady-state drain
   is **0 cross-core RMW** (F4), whereas B pays **1 detach-RMW per message at
   occupancy-1** (F2's own scope) — exactly the latency operating point. B's genuine
   wins are elsewhere: better *cross-core* tail (p999 525 vs 5051 ns) and a
   RMW-free *deep-backlog* bulk drain (47–124 Mops at B≥64) — neither is the metric
   the mailbox hot path is graded on, and both are harvestable as optional draining
   optimizations without adopting B.

4. **No core invariant bent — and B carries a strictly-worse tail.** A upholds
   at-most-one-executor (`head_` fenced by the exec CAS), FIFO-by-default (`tail_`
   exchange mod order), zero-alloc (disassembly). B is FIFO-*correct* but recovers it
   by LIFO-then-reverse, and its confirmed head-of-line defect — a single producer
   preempted in the ~1 ns publish gap **freezes the entire actor's dispatch**,
   including fully-linked newer messages, for ~708µs — is *structurally worse* than
   A, whose equivalent window stalls only the newest node while everything older
   drains from `head_` unobstructed. By REX's own falsification criterion ("if REX is
   even slightly slower, Vyukov strictly dominates"), parity-plus-worse-tail is a
   losing trade against an Accepted ADR with no fatal flaw.

**What B earns:** its bulk-detach, RMW-free reversed drain is the fastest measured
deep-backlog consumer path and is worth keeping on file as an optional draining mode
behind the same `MessageHandle` contract — the same disposition ADR-002 gave Design
B's drain sweep.

### Confidence and the one residual tie-breaker

Decisive on x86-64, now the **third** independent round to reach it. The single
experiment that could still move the *margin* (not the winner) is unchanged from
ADR-002: **A vs B on a weakly-ordered AArch64 box, with the enqueue publication edge
and the exec-state handoff encoded as herd7/GenMC litmus.** herd7/GenMC/AArch64 were
absent; all sanitizer evidence is x86-TSO, which is structurally blind to a missing
acquire. A's contention advantage is expected to *widen* on ARM.

## Spec-update recommendations

See `specRecommendations` in the structured output. Summary:

- **003-Memory.md** — (1) Correct the enqueue memory order: line 55 currently says
  `tail_.exchange(desc, release)`; change to **`acq_rel`**. On weakly-ordered ISAs
  the acquire half is load-bearing for publication ordering (predecessor node-init
  happens-before the successor's link store); release-only permits a lost-newest-node
  execution that is invisible under x86-TSO sanitizers. (2) Make the compile guard
  portable: wrap `is_pointer_interconvertible_with_class(&Descriptor::link)` in
  `#ifdef __cpp_lib_is_pointer_interconvertible` (it does **not** build on clang 20.1 +
  libstdc++), keep the unconditional `is_standard_layout_v<Descriptor>` +
  `offsetof(...)==0`, and **add** `static_assert(is_standard_layout_v<std::atomic<void*>>)`
  so a non-conforming stdlib fails loudly instead of miscompiling `desc_of()`. Gate CI
  on both compilers building the guard.
- **002-Scheduler.md** — Split the release close-out's memory orders. The `head_`
  handoff is a single-writer publish and remains **release/acquire**. The
  **wakeup rendezvous is a StoreLoad (Dekker) and must be `seq_cst` on both sides**:
  consumer `exec_state.store(Idle, seq_cst)` then `tail_.load(seq_cst)`; waking
  producer `tail_.exchange(seq_cst)` then `exec_state.load(seq_cst)` (or a
  `seq_cst` fence between the store and the load on each side). Reconcile §41–46
  (currently `store(Idle, release)`), and record the dekker control
  (release/acquire → ~48% lost windows; seq_cst → 0) as a **permanent CI guard**
  alongside the existing relaxed-CAS `head_` guard.
- **001-Actor-Execution-Model.md** — In §Mailbox, state that the exec-state CAS
  carries **two distinct obligations with different orders**: (a) the consumer-private
  `head_`/`fifo_` handoff = **release/acquire** (sufficient; S1 proven), and (b) the
  wakeup/close-out rendezvous with producers = **seq_cst StoreLoad** (C3). The current
  text names only release/acquire and would, taken literally, lose wakeups.
- **015-Reentrancy-and-Quiescence.md** — No invariant change. Reaffirm (§70–74) that
  the drain **never spins unbounded** on `Busy`; the `Paused` seal + reschedule is the
  mitigation. Add a note that the round-3 REX head-of-line result (a single mid-publish
  producer can freeze the whole actor's dispatch for a scheduler quantum) is the
  **rejected alternative**, and that the incumbent's mid-publish window stalls only the
  newest node — a property to preserve in any future mailbox change.

## Residual risks

1. **seq_cst wakeup rendezvous is now a load-bearing correctness fix.** Reverting the
   StoreLoad to release/acquire silently loses ~48% of wakeups (dekker
   144807/300000). Encode seq_cst normatively (002) and keep the dekker control as a
   CI regression guard. This is *distinct* from the `head_` handoff, which is
   correctly release/acquire — do not "simplify" them into one order.
2. **Enqueue exchange must be acq_rel, and weak-memory is unproven by tool.** 003
   line 55 (`release`) is wrong for AArch64; the acq_rel fix's publication ordering is
   argued, not machine-checked (no herd7/GenMC/AArch64). Run the litmus + an ARM
   produced-vs-dispatched bitset before committing cross-platform latency numbers.
3. **Compile guard portability.** `is_pointer_interconvertible_with_class` does not
   build on clang 20.1; a maintainer who deletes the assert to make clang build turns
   `desc_of()` into an unchecked UB cast. The `#ifdef` wrap + added
   `is_standard_layout_v<atomic<void*>>` + two-compiler CI are mandatory, not optional.
4. **Producer preemption in the exchange→link window strands the newest node** for a
   scheduler quantum under P≫cores oversubscription. Out of the local-tell budget;
   delegated to 002 bounded-spin/reschedule + 022 admission. A system-level tail risk,
   not a correctness bug.
5. **Cancellation requires the packed single-CAS gen+state.** The REX round exposed a
   TOCTOU when generation and state are two separate atomics; the gate must be one
   packed CAS. 003's gen-gated cancel is safe only with an atomic read-modify-write of
   the combined word.
6. **Unbounded queue = no backpressure** (unchanged; 022 companion policy).
7. **Harvestable, not adopted:** REX's RMW-free bulk-detach drain (47–124 Mops at
   B≥64) is left as a future optional deep-backlog draining mode; if pursued it must be
   re-proven on the isolated 023 reference core and on AArch64.
