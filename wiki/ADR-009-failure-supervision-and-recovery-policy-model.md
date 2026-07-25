# ADR-009 — Failure, Supervision & Recovery Policy Model

Status: **Accepted**
Date: 2026-07-15
Deciders: design-debate-prove judge (closing the 007/004/015/012 failure-supervision-recovery debate)
Supersedes/relates: builds on ADR-002/003/004 (mailbox MPSC hot path), ADR-007
(authoring & dispatch API + pooled `ReplyCell` protocol), ADR-008 (configuration &
activation-lifecycle policy); resolves the standing open questions in
`007-Failure-and-Supervision.md`, `004-Resources.md`, honours the quiesce model in
`015-Reentrancy-and-Quiescence.md`, binds `Restart` semantics to `012-Persistence.md`,
and is gated by the `023-Performance-Targets-and-Budgets.md` local-tell budget.

---

## Question

Resolve, **as one coherent configurable policy surface**, the standing 007/004 open
questions for how an actor's failure is contained, supervised, and recovered — every
choice a compile-time policy/config knob (CRTP, 005: `OnFailure<Restart, MaxRestarts<…>,
Within<…>>` and friends) with **no cost on the success path**, no reflection, no
RTTI/virtual on the hot path, std-only C++23 (`std::expected` error *values* not
exceptions across the `ask` boundary):

- **(007)** State rollback on `Resume` — transactional snapshot, or strictly "developer
  asserts state intact"?
- **(007)** Escalation granularity — per-actor-type supervisors vs. a single node
  supervisor vs. a configurable hierarchy.
- **(007)** `ask` reply on `Restart` — does the in-flight `ask` always fail, or can it be
  retried against the fresh actor?
- **(007/012)** Does `Restart` reload persisted state?
- **(004)** `PerMessage<T>` factory failure mid-message — fail the message (007) or
  degrade?

The **provable core**: (a) the handler-boundary exception guard is **zero-cost on the
no-throw path** (objdump: no added work; tell/ask latency within the 023 ≤100 ns
local-tell budget); (b) `Restart` **keeps the mailbox** and removes the poison message
**exactly once** under concurrent enqueue (TSan/ASan); (c) an `ask` that triggers a
`Restart` is **UAF/ABA-clean** on its reply cell; (d) any transactional `Resume` rollback
is **atomic w.r.t. the failure**; (e) escalation is **bounded** (no restart storm /
unbounded recursion). x86-64 Linux, GCC 14.2 + Clang 20.1, -O2/-O3+LTO. Percentiles, not
means.

---

## Designs debated (one-line summaries)

- **D1 — Minimal / Assert-Intact Supervision (zero-cost guard, opt-in Transactional
  rollback).** A single CRTP knob `OnFailure<Decision, MaxRestarts, Within>`; a plain
  Itanium zero-cost `try{dispatch}catch(...)` at the handler boundary only; `Resume` =
  assert-intact by default with rollback **opt-in** via Sequential-only `Transactional<>`;
  `ask`-on-`Restart` **always fails** with `unexpected(error::restarted)`; single node
  supervisor; message outcome recorded **before** actor fate so the reply cell stays clean.
- **D2 — Transactional-Undo Checkpoint + Retryable-Ask over the shard ReplyCell, with a
  typed Supervisor tree.** "Safety-maximal": opt-in delta-journal `Checkpoint<Undo>`
  (cost ∝ mutation count), retryable `ask` via `RetryPolicy<N>` keeping the pooled cell
  Armed, typed `Supervisor<S>` tree with consteval-finite depth.
- **D3 — PolicyTyped Supervision Surface (hybrid/configurable middle).** Every 007/004
  choice an opt-in policy *type* collapsed by `if constexpr`: `Transactional<Off|Snapshot|
  Journal>`, `OnRestartAsk<Fail|Retry<N>>`, `Supervision<Node|PerType|Tree<…>>`,
  `OnResourceFailure<FailMessage|Degrade>`, `Persistent<…>`; all divergence in one
  out-of-line `[[gnu::cold]] supervise()`.

---

## Evidence table (claim → survived red-team? → proven? → number)

### D1 — Minimal / Assert-Intact  *(WINNER)* — 13/13 proven, 0 disproven

| Claim | Survived | Proven | Executed number |
|---|---|---|---|
| F1 zero-cost guard latency | yes (F2 instr-identity conceded → F1 noise band is sole arbiter) | **CORRECT** | guarded p99 54.41 ns vs control 53.46 (g++ -O2, ×1.018); all 4 toolchain/opt combos ratio ≤1.019, all ≤54.4 ns « 100 ns goal |
| F2 zero alloc/snapshot/state-branch | yes (weakened from "instruction-identity") | **CORRECT** | objdump: g++ `endbr64;call;xor;ret` (control tail-calls `jmp`); clang +1 align push/pop; **0 allocs/msg over 1e7**; `Transactional<>` positive control shows the 4×movdqu 64 B copy |
| S1 poison removed exactly once | yes (unified single reclamation join point) | **CORRECT** | 4×2e6 descriptors across all 4 branches, returns{0:0, 1:all, 2+:0}; TSan+ASan clean; control (relaxed store) → ASan double-free |
| S2 ask-restart reply cell UAF/ABA clean | yes (siblings included) | **CORRECT** | 30 000 cycles: double_complete=0, double_release=0, unresolved=0 (no hung caller), misroute=0; ASan+TSan clean both compilers |
| S3 single-executor + FIFO across restart | yes (mechanism corrected: seal not Running-ownership) | **CORRECT** | max_concurrent_running=1, fifo_ok=1; Reentrant restart releases Running at co_await, admission blocked by Cancelling seal |
| S4 no silent drop / no lane crash | yes (new) | **CORRECT** | `static_assert(noexcept(sink.enqueue)&&noexcept(complete_reply))` gate holds; throwing-sink control fails compile; bounded sink delivered=100/shed=9900, lane survives |
| C1 Resume assert-intact / Txn atomic | yes (Sequential-scoped; Reentrant+Txn forbidden) | **CORRECT** | default persists (counter=7); Transactional rolls back (100); `static_assert(!(txn&&reentrant))` fires both compilers |
| C2 ask-on-Restart always fails | yes (unattacked) | **CORRECT** | reply=`unexpected(error::restarted)` (value, not exception), dispatched=1, never re-run |
| C3 bounded escalation | yes | **CORRECT** | MaxRestarts<3>: restarts=3, escalations=1, stopped, poison dead-lettered; no storm/recursion |
| C4 PerMessage factory failure fails msg | yes (both throw AND unexpected) | **CORRECT** | both variants → `unexpected(resource_error)`, handler_ran=0, policy fired |
| C5 deadline/cancel carve-out (no storm) | yes (new) | **CORRECT** | 20% deadline load: timed_out=1995, restarts=0, restart_charged=0, actor NOT stopped |
| C6 reconstruct-failure containment | yes (new) | **CORRECT** | store load throws AND returns unexpected: lane_survived=1, escalated+stopped, survivor never dispatched against empty state |
| C7 EventSourced staging fence | yes (new) | **CORRECT** | stage debit(100) then throw → durable log empty, reconstructed balance=0; normal path → log=[100] |

### D3 — PolicyTyped Supervision — 10/10 proven, 0 disproven

| Claim | Survived | Proven | Executed number |
|---|---|---|---|
| F1 objdump-zero default success path | yes | **CORRECT** | **GCC hot BB byte-identical** to control; clang +1 free reg-move; guarded p99 76.4 ns on **sub-reference** silicon (est. reference ~1.5–1.9× faster), under 100 ns goal |
| F2 per-knob-only cost | yes | **CORRECT** | Off thunk == control; Snapshot adds only the memcpy; slope≈1, intercept≈0 vs sizeof(state) |
| F3 lane-private 0 cross-core RMW | yes (unattacked) | **CORRECT** | objdump: 0 lock-prefixed ops on budget/snapshot; shared-atomic control → TSan race + `lock addw` |
| S1 poison exactly-once (adoption/copy-out) | yes (as-written double-ownership conceded) | **CORRECT** | 640 k msgs, sink XOR pool, returns=640 k; control (delete-to-OS) → ASan UAF |
| S2 retry reply cell clean (CAS-claim) | yes (relaxed re-stamp conceded) | **CORRECT** | 2 M asks Retry<2>: one outcome each, 0 misdeliver/0 double-resolve; both UAF+ABA controls fire |
| S3 quiesce(Cancel) reentrancy-safe | yes (seal-window conceded → episode marker) | **CORRECT** | restart_count/reconstruct once/episode, dead_letters==in_flight_at_seal; control → double-restart 2946/2000 |
| S4 recovery-throw containment | yes (new) | **CORRECT** | bad_alloc into sink/reconstruct: lane survives, escalate-to-Stop within MaxRestarts; unwrapped control → SIGABRT |
| C1 bounded escalation (runtime+static) | yes | **CORRECT** | ttl-bounded runtime cycle, per-shard rate limiter 1600≤cap over 10 k actors, hop depth ≤ chain |
| C2 Txn Snapshot atomic (Reentrant forbidden) | yes (unconditional conceded) | **CORRECT** | 0/100 k fuzzed partials survive; Reentrant+Snapshot → hard compile error |
| C3 Persistent reload + idempotent retry | yes (effect-dup conceded → IdempotencyKey) | **CORRECT** | persistent reload==store, non-persistent load_calls=0; fenced retry effect==1 vs unfenced==2 |

### D2 — Transactional-Undo + Retryable-Ask — 5/5 proven, 0 disproven, **2 fatal flaws conceded**

| Claim | Survived | Proven | Executed number |
|---|---|---|---|
| F1 default guard latency | yes (bit-identical conceded) | **CORRECT** | guarded p50 42.00 vs 41.05 (×1.023), all ≤43 ns |
| F2 Undo cost ∝ mutation count | yes | **CORRECT** | ~6.7 ns/journaled write, **flat 72.55 ns across 64 B..1 MB at K=4**; 0 allocs (arena) |
| S1 poison exactly-once + retry cell clean | yes (**fatal reply-cell UAF + missing `next_` reset conceded**, then fixed) | **CORRECT** | 3 M msgs clean; both `BUG_NO_NEXT_RESET` and `BUG_NO_RESERVE` controls fire |
| C1 Txn rollback type-sound | yes (**fatal raw-memcpy-of-non-trivial conceded**, then value-semantic fix) | **CORRECT** | pmr::string/vector rollback ASan-clean; pool.available()==4 via RAII |
| C2 bounded escalation + decoupled retry | yes (consteval-only conceded → ttl) | **CORRECT** | restarts==3; type cycle fails compile; ttl bounds runtime cycle |

> Evidence caveat weighing against D2: its S1/S2 concurrency was **modeled at the
> hazard level, not with real suspended coroutine frames** ("K=64 suspended siblings are
> modeled … not with real suspended frames"). D1 and D3 exercised the reply-cell
> abandon/complete and quiesce interleavings on the actual protocol.

---

## Decision

**Winner: D1 — Minimal / Assert-Intact Supervision (zero-cost guard, opt-in Transactional
rollback),** adopted as the accepted core, **extended with D3's proven configurability
knobs** (below) to fully satisfy the "every choice a knob" mandate.

Rationale, against the ranking gates in order:

1. **Safety gate — all three pass.** Every safe/correctness claim came back CORRECT under
   executed C++; **zero disproven claims** for any design; every positive control fired,
   proving the sanitizers had teeth. No design is disqualified.
2. **Proven beats claimed.** Counting claims that BOTH survived red-teaming AND were
   proven with executed evidence: **D1 = 13, D3 = 10, D2 = 5.** D1 leads, and its lead is
   substantive, not slicing: D1 uniquely proves **C5** (deadline/cancellation are carved
   out of the restart decision, so transient overload cannot restart-storm a healthy actor
   into node-supervisor `Stop`) and **C7** (EventSourced events stage per-message and
   commit only at handler completion, so `Restart`+replay never resurrects a poison
   handler's pre-throw append). Both are real containment properties D3 does not establish.
   D2 is out: it entered red-teaming with **two fatal, load-bearing flaws** (raw
   byte-copy Undo → UAF/double-free on any non-trivially-copyable state — exactly the
   string/vector/map MES state it targets; and a reply-cell UAF because the restart window
   is unbounded and a deadline/cancel completer can recycle the pooled cell), plus a
   weaker evidence base (hazard-level modeling, fewest proven claims).
3. **Cheapest success path + best failure numbers.** All survivors are zero-cost on the
   no-throw path (no alloc, no snapshot, no state-branch by default). On **directly
   comparable reference-class silicon** D1's guarded local-tell is **p99 54.4 ns** (≤1.9%
   over a no-guard control, well inside the 023 100 ns goal and the ≤250 ns hard ceiling);
   D3's headline p99 76.4 ns was measured on explicitly **sub-reference** silicon and is
   only competitive after a 1.5–1.9× projection. D1's failure path shares **one** audited
   `Running→Completed` gen-bump + pool-return join point across all four decision branches,
   proven exactly-once over 4×2e6 descriptors.
4. **No core invariant bent.** D1 upholds success-path zero-cost (F1/F2), single-executor
   (S3, max_concurrent_running=1), mailbox-FIFO of survivors (S3, fifo_ok=1), no-silent-drop
   (S4, statically-`noexcept` bounded shed sink), and reply-cell safety (S2). Its
   Reentrant+Transactional incoherence is closed by a **compile-time** `static_assert`, not
   left to runtime.

**Where D1's resolution is narrower than the target asked, adopt D3's proven knobs.** D1
resolves escalation as a single node supervisor and `ask`-on-`Restart` as always-fail —
coherent, but *fixed answers*, whereas the target wants these expressed as knobs. D3
**empirically proved** the richer forms are implementable inside the same zero-cost,
UAF-clean, bounded-escalation envelope:
- **`Supervision<Node | PerType | Tree<…>>`** — configurable escalation hierarchy;
  consteval-finite static depth *plus* a per-message `escalation_ttl` and per-supervisor
  `MaxRestarts` that bound runtime/reconfigurable cycles the consteval check cannot see;
  `escalate()` **tells** the supervisor's lane (never a synchronous touch → single-executor
  preserved); storm rate-limiting via 022 **per-shard-local** token buckets (no global
  atomic). (D3 C1 proven.)
- **`OnRestartAsk<Fail | Retry<N, IdempotencyKey>>>`** — `Fail` (D1's proven default)
  stays the default; `Retry<N>` is opt-in and **requires** an `IdempotencyKey` (or
  EventSourced command-dedup), enforced by `static_assert`; the retry claims the reply cell
  with the same acq_rel win-arbitration CAS and re-stamps a fresh descriptor **before** the
  poison is freed, and resets `next_=nullptr` to honour the Vyukov contract. (D3 S2/C3
  proven.)

The accepted surface is therefore D1's guard + assert-intact/opt-in-Transactional core +
D1's C5/C6/C7 carve-outs, with escalation topology and ask-retry promoted from fixed to
configurable per D3's proven mechanics.

---

## Resolution of the open questions

| Open question | Resolution (all compile-time knobs) |
|---|---|
| **(007) State rollback on `Resume`** | Default `Resume` = **assert-intact, no snapshot** (proven zero-cost, D1 C1/F2). Rollback is **opt-in** `Transactional<>` — the only policy that copies state — **Sequential-only** (Reentrant+Transactional is a `static_assert` compile error; MVCC/COW for Reentrant deferred to the 015 large-state open question). D3's `Transactional<Off|Snapshot|Journal>` naming is adopted as the knob's variant axis; `Off` is the default and emits nothing. |
| **(007) Escalation granularity** | **Configurable hierarchy** `Supervision<Node | PerType | Tree<…>>` (D3, proven). Single node supervisor is `Tree` of depth 1 (D1's default). Static depth is consteval-bounded (`static_assert ≤ 8`, no cycles by construction); runtime/reconfig cycles and correlated fan-in are bounded by per-message `escalation_ttl` + per-supervisor `MaxRestarts` + 022 per-shard rate limiter. `escalate()` is a **message hop** to the supervisor's lane, never a synchronous cross-lane touch. |
| **(007) `ask` reply on `Restart`** | Knob `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`. **`Fail` is the default** (D1 C2 proven: `unexpected(error::restarted)`, single dispatch). `Retry<N>` is opt-in and **requires** an idempotency fence at compile time; it reserves the pooled cell (CAS-claim before the restart window) so a racing deadline/cancel cannot recycle it (D3 S2 proven). |
| **(007/012) Does `Restart` reload persisted state** | **Yes iff `Persistent<…>`** is in the policy list (D1 C6, D3 C3 proven): reload via `StateStore::load` + fencing-token bump + EventSourced tail replay; non-persistent actors reconstruct fresh via the construction factory (`StateStore::load` never called). The poison message is dead-lettered exactly once either way. **Reload returns `std::expected`** — adapter exceptions are caught at the store seam; a reload failure **escalates** (Stop + dead-letter survivors under the held seal), never releases the seal onto empty state and never crosses the `noexcept` cold path (D1 C6 proven). EventSourced events **stage per-message and commit at handler completion** (015), so a throwing handler commits nothing and `Restart` cannot replay a poison's partial append (D1 C7 proven). |
| **(004) `PerMessage<T>` factory failure** | **Fail the message** (routes to the 007 policy), never degrade silently. Products are resolved+checked **inside the guarded region before the handler body**, so both a **throwing** factory and one **returning `std::unexpected`** are handled uniformly: skip dispatch → `on_fail(resource_error)` → ask caller gets `unexpected(resource_error)`, tell → dead-letter, handler body never runs degraded (D1 C4 proven). Partially-acquired external resources are reclaimed by the factory product's **RAII guard** (D2 C1 / D3 proven), not by state rollback. "Degrade" remains expressible as the explicit `OnResourceFailure<Degrade>` knob (D3) for callers who want it — throwing factories are tagged `FailureSource::Resource` so `Degrade` fires on both channels. |

---

## Spec recommendations

**`007-Failure-and-Supervision.md`** (largest change)
- Add a **failure-source classification** step ahead of the supervision decision: sources
  #2 (deadline) and #3 (cancellation) are **transient** — report the message
  (ask→`unexpected(deadline_exceeded|cancelled)`, tell→dead-letter) and `Resume` **without**
  `quiesce(Cancel)`, **without** reconstruct, and **without** charging `MaxRestarts`; only
  #1 (handler throw), #4 (resource) and #5 (poison-loop) run the configured `Decision`. The
  carve-out disposition is itself a policy slot. (Closes the transient-overload restart
  storm; D1 C5 proven.)
- Replace the flat "Escalation → node supervisor" section with the configurable
  **`Supervision<Node | PerType | Tree<…>>`** hierarchy: consteval-bounded static depth
  (`static_assert ≤ 8`, acyclic by construction) **plus** a per-message `escalation_ttl`
  and per-supervisor `MaxRestarts` window that bound runtime/reconfigurable cycles and
  correlated fan-in; state that `escalate()` **tells** the supervisor's lane
  (single-executor preserved) and aggregate storms are capped by 022 **per-shard-local**
  token buckets. Update the code example to show the knob.
- Resolve the "`ask` reply on `Restart`" open question: **`OnRestartAsk<Fail |
  Retry<N, IdempotencyKey>>`**, default `Fail` with `unexpected(error::restarted)`;
  `Retry<N>` opt-in, **`static_assert`-requires an idempotency fence**, reserves the pooled
  `ReplyCell` (Armed→Retained CAS) across the restart window so a deadline/cancel completer
  cannot recycle it, and resets `next_=nullptr` on re-enqueue (Vyukov contract).
- Resolve the "State rollback on `Resume`" open question: default **assert-intact**;
  opt-in **`Transactional<>` is Sequential-only**, enforced by
  `static_assert(!(is_transactional && is_reentrant))`; the message outcome
  (`complete_reply` / `dead_letter`) is recorded **before** the actor's fate, and both are
  **statically `noexcept`** with a **bounded shed-don't-buffer** dead-letter sink (022) so a
  full/failing sink sheds-with-metric and never terminates the lane.
- State the **single descriptor-reclamation join point**: `Running→Completed` gen-bump +
  shard-pool return happens **once**, at one site shared by the success path and all four
  failure branches (`do_restart` does not reclaim separately).
- Note the reconstruct/reload-failure rule: reload returns `std::expected`; failure
  **escalates** rather than releasing the seal onto empty state.
- Correct the reentrant-restart single-executor rationale: safety comes from the
  **`Cancelling` seal + exec-state CAS**, not from "holding Running" (Running is released at
  `co_await quiesce`).

**`004-Resources.md`**
- Resolve the factory-error open question: a `PerMessage<T>` factory failure **fails the
  message** via the 007 boundary; the engine **resolves+checks each product before the
  handler body**, handling both throwing and `unexpected`-returning factories uniformly
  (handler never runs with a null/degraded resource). Require partially-acquired external
  resources to be released via the product's **RAII guard**; state that `Cached<>`-pool
  checkouts must be RAII-scoped subobjects of the product. Cross-reference
  `OnResourceFailure<FailMessage | Degrade>` in 007.

**`015-Reentrancy-and-Quiescence.md`**
- Add that on the Reentrant `Restart` branch, cancelled in-flight **sibling `ask`s** each
  unwind through their own handler-boundary guard and complete their reply cell with
  `unexpected(cancelled)`; the `quiesce(Cancel)` "await in_flight==0 **before** reconstruct"
  step (existing #3) is what guarantees every sibling cell completes before state teardown —
  **no caller hang** (D1 S2 proven, count resolved == K).
- Add the **restart-episode marker** rule: a sibling that faults *during* the
  seal→quiescence window (before observing its cooperative stop_token) is dead-lettered as
  its cancellation outcome and returns **without** re-incrementing `restart_count` or
  launching a nested restart (protects the `MaxRestarts` bound; D3 S3 proven).
- Keep the large-state **COW-snapshot** item open — it is the precondition for lifting the
  Sequential-only restriction on `Transactional<>` to Reentrant actors.

**`012-Persistence.md`**
- State the **EventSourced staging fence**: events raised by a handler **stage in a
  per-message buffer** and become durable only at the handler-completion commit point (015);
  a throwing handler commits nothing, so `Restart`→reload replays only pre-poison committed
  state (D1 C7 proven) — closes durable "Resume-on-corrupted-state."
- Note that `Restart`'s reload path returns `std::expected` and a reload failure escalates
  (ties to the 007 reconstruct-failure rule); the fencing-token bump happens on reconstruct.
- Fold the "exactly-once effects" open item toward the `OnRestartAsk<Retry<N,
  IdempotencyKey>>` fence: a fenced idempotent retry commits its durable effect exactly once
  (D3 C3 proven, effect==1 vs unfenced==2).

**`023-Performance-Targets-and-Budgets.md`**
- Add a **Failure/Supervision (ADR-009)** row block: `Local sync tell with handler-boundary
  guard` — Goal p99 ≤ 100 ns, Hard ≤ 250 ns, **Proven p99 54.4 ns** (guarded) vs 53.5 ns
  (no-guard control), ratio ≤ 1.019 across GCC/Clang × -O2/-O3+LTO; **0 heap allocs/msg**;
  objdump: no added call/alloc/state-branch on the success path.
- Record `Transactional<>` opt-in overhead as ∝ sizeof(State) (**not** on the default
  path); record the failure path as **cold/off-budget** (µs-scale Itanium unwind, bounded by
  `MaxRestarts`/`Within` + 022 rate limiter).
- Bind the escalation-storm guard to 022's **per-shard-local** token bucket (no global
  atomic; consistent with the 0-cross-core-RMW drain gate).

---

## Residual risks

1. **Cold-path unwinder cost.** The zero-cost guarantee is success-path only; a throw pays
   a full `.gcc_except_table` unwind (µs-scale). A workload throwing on a large fraction of
   messages will see p999 dominated by unwinding, not dispatch. Acceptable under 023's cold
   scoping, but a high-throw domain may later need an opt-in error-return handler lane.
   A `-fno-exceptions` build changes the guard codegen entirely and must be benchmarked
   separately.
2. **Large-state `Transactional<>` footgun.** The opt-in rollback is a full per-message
   state copy with no COW; big-state actors get either no rollback (assert-intact) or an
   expensive copy, with nothing in between until the 015 COW-snapshot open question closes.
   Reentrant actors cannot use `Transactional<>` at all yet.
3. **Retry idempotency is asserted, not verified.** `OnRestartAsk<Retry<N,
   IdempotencyKey>>` requires the *presence* of a fence at compile time but cannot prove the
   handler is actually idempotent; a mis-keyed handler can still double-effect. The type
   system labels, it does not enforce semantics.
4. **ARM64 / non-x86-TSO unverified.** All S1/S2/S3 evidence is x86-TSO. The reply-cell
   reservation CAS, the `next_` release/`tail_` exchange re-enqueue, the `commit_epoch`
   release/acquire, and the `Cancelling`-seal Dekker fence need herd7/GenMC litmus before
   ARM64 promotion.
5. **Generation-width ABA assumption.** `exec_state`/reply-cell generations are 48-bit;
   ABA-safety assumes no wrap within any live window — practically unbounded at realistic
   rates, but an assumption, not a proof, for pathological multi-year peak-rate uptime.
6. **Adopted D3 knobs (`Supervision<Tree>`, `OnRestartAsk<Retry>`, `OnResourceFailure<
   Degrade>`) were proven in D3's harness, not re-implemented against D1's exact guard.**
   Their integration into the accepted D1 core should carry the same permanent
   ASan/TSan/014-simulator gates before this ADR's spec text moves those specific knobs
   from Draft to Accepted; the D1 core (F1/F2/S1–S4/C1–C7) is proven as one coherent build.

---

## Status of the debate

Decisive. The winner is empirically established, not a judgement call: D1 leads on every
ranking gate (13 proven vs 10 vs 5; zero disproven; directly-measured lower p99; all core
invariants held with compile-time enforcement of the one incoherent configuration). No
tie-breaking experiment is required. The only follow-up work is re-running D3's three
adopted configurability knobs through the same sanitizer/benchmark gates against the
committed D1 guard before their spec paragraphs promote to Accepted.
