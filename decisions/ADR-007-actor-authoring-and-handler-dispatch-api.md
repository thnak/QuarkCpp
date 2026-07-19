# ADR-007 — Developer-Facing Actor-Authoring & Handler-Dispatch API

Status: **Accepted**
Date: 2026-07-15
Deciders: design-debate-prove judge (closing the 005/006/001 dispatch-API debate)
Supersedes/relates: builds on ADR-002/003/004 (mailbox MPSC hot path); resolves the
open questions in `005-Developer-Model.md`, `006-Messaging-and-Addressing.md`,
`001-Actor-Execution-Model.md`.

---

## Question

Resolve, **as one coherent developer API**, the standing open questions for how a
developer authors actors and how handlers are dispatched — every choice stated as a
compile-time **policy/config knob**, with no RTTI, no reflection, and no virtual
dispatch for policy anywhere on the local hot path:

- **(005)** How are `handle` overloads dispatched — generated switch vs jump-table
  keyed by message-type-id vs `tag_invoke`/CPO compile-time dispatch?
- **(005)** Resource-declaration ergonomics — member fields `Cached<Logger> log_;`
  vs a `using resources = ResourceSet<…>` type list?
- **(006)** `ask` from synchronous code — provide a blocking `ask_sync`, or forbid it?
- **(006)** Typed `ActorRef<A>` vs an untyped/dynamic ref for routers/dead-letter?
- **(001)** Reply ordering for concurrent `ask`s to a reentrant actor.

The **provable core** is dispatch: per-`tell`/`ask` dispatch overhead against the 023
local-tell budget (≤100 ns goal / ≤250 ns hard, 0 alloc), objdump-confirmed absence of
virtual call and RTTI on the local path, and the compile-time / code-bloat tax per
actor type. Percentiles, not means. x86-64 Linux, GCC 14.2 + Clang 20.1, -O2/-O3+LTO.

---

## Designs (one-line summaries)

- **D1 — JumpTable-Dispatch** (WINNER): each actor declares `using protocol =
  Protocol<…>`; the send site computes a dense per-actor slot as a **compile-time
  constant** and stamps a 2-byte index into the descriptor; the drainer does one
  `.rodata` function-pointer indexed call `thunks[slot](self,payload,ctx)`. Typed
  `ActorRef<A>` always; **async-only `ask`** (no `ask_sync`; off-lane bootstrap uses
  `block_on`); member-field resources; reply ordering falls out of admission order.
- **D2 — CPO-Dispatch**: `tag_invoke`/CPO `handle_to` resolved at the statically-typed
  send site bakes a per-`(A,M)` link-time-constant thunk into the descriptor; the drain
  does zero type lookup (one pre-resolved thunk). `ask_sync` offered (off-engine-thread
  guarded); `ResourceSet<…>` type list; `ReplyOrder<Unordered|Causal>` knob.
- **D3 — Split-Enqueue Dispatch**: compile-time thunk baking on the typed path; the 008
  jump-table survives only at the erased `AnyRef` boundary (routers/dead-letter). Typed
  and erased share one uniform drain indirect call. `ask_sync` off-engine-thread guarded;
  `ReplyOrder<Completion|SenderFifo>`; dead-letter via a first-class `DeadLetter` wrap.

All three are std-only C++23, uphold "unhandled send = compile error," ride the settled
ADR-002/003/004 publication edge with **zero new cross-core RMW on the drain**, and were
objdump-proven to carry **no virtual call and no RTTI** on the local path.

---

## Evidence table (claim → survived red-team? → proven? → number)

Executed on x86-64 Linux, TSC ~2.095 GHz (no turbo), pinned core, rdtsc percentiles,
GCC 14.2 + Clang 20.1, -O2 and -O3+LTO. Allocations hooked (global `operator new` +
pmr upstream counters). "Survived" = survived cross-examination on the revised design.

### D1 — JumpTable-Dispatch (winner)

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 sync tell enqueue→dequeue→dispatch | yes | **CORRECT** | g++ p50=46.8 / p99=62.1 / p999=119.3 ns; clang p50=47.7 / p99=66.8. **0 alloc.** Pure dispatch step p50=20 / p99=25 ns |
| F1 ask round-trip (pooled ReplyCell+frame) | yes | **CORRECT** | g++ p50=83.1 / p99=129.8 / p999=256.8 ns; clang p50=75.4 / p99=113.6. **0 pool-upstream.** ~40× under the 5 µs ask budget |
| F2 code-size + switch parity | yes | **CORRECT** | thunk .text **260 B/actor, linear**, independent of engine-wide msg count; glue ≤25 B; table = plain fn-ptr array (0 vtable). Within noise of hand-switch on **uniform AND 95%-skew** |
| S1 no new atomic/fence; drain 0-RMW | yes | **CORRECT** | TSan clean; positive control (relaxed publish) races the 2-byte `msg_slot_` every run; drain objdump has 0 lock-prefixed RMW; `sizeof(Descriptor)==64` |
| S2 static_cast type-sound, no RTTI | yes | **CORRECT** | objdump: 0 typeinfo/vtable/dynamic_cast; dispatch is a single `call *(%rax,%rcx,8)`; ASan+UBSan clean over 1e6 mixed types |
| C1 unhandled send = compile error | yes (fatal `slot_of` defect fixed) | **CORRECT** | 6/6 compile tests; **handled-but-unlisted now a compile error** (`requires Handles` + `InProtocol` static_assert); slot==k unreachable |
| C2 concurrent-ask reply ordering + lifetime | yes (protocol corrected by prover) | **CORRECT** | Reentrant K=64: 0 misroute, out-of-order observed; Sequential: request-ordered; ABA 200k / lost-wakeup 2M: 0 UAF; TSan+ASan+UBSan clean |
| C3 async-only ask; block_on off-lane | yes | **CORRECT** | no `ask_sync` symbol; `block_on` on a worker lane returns `unexpected(on_worker)`, off-lane drives to completion |

**D1 tally: 7 survived + proven CORRECT, 0 disproven.**

### D2 — CPO-Dispatch

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| F1 sync tell | yes | **CORRECT** | g++ p50=37.2 / p99=52.5 / p999=110.7 ns; clang p50=36.3 / p99=49.6. **0 alloc. Best sync-tell numbers.** |
| F1b async ask 0-alloc | yes (fatal alloc fixed by arena-backing promise) | **CORRECT** | 0 global new/msg arena-backed; control (default new) = 1.000/msg |
| F2 thunk ≤ 008 scan, advantage grows with k | yes | **CORRECT** | g++ thunk vs scan: k=1 2.80/3.27 → k=64 15.44/43.07 ns |
| S1 no virtual/vtable/RTTI, ≤1 indirect | yes | **CORRECT (1 sub-claim DISPROVEN)** | 0 RTTI, one data-ptr indirect. **"Zero-indirect under mono-LTO" REFUTED** — single-actor whole-program still keeps one `call *0x10(reg)` on both toolchains |
| S2 zero new cross-core RMW | yes (control re-aimed to `next_`) | **CORRECT** | TSan clean; corrected positive control on `next_.store` races the field; contrast control on `tail_` does not |
| C1 unhandled = compile error | yes (implicit-conv leak closed) | **CORRECT** | `tell(42)`→`handle(const Ship&)` now rejected; exact `MessageSet` membership |
| C2 sync/async compile-time select | yes | **CORRECT** | 1e6 interleavings, concur_violations=0; sync thunk emits no coroutine machinery |
| C3 ReplyOrder Unordered/Causal | yes (semantics pinned) | **CORRECT** | per-caller reorder, zero cross-caller HoL |
| C4 ask_sync off-engine-thread guard + futex | yes (real TSan race fixed) | **CORRECT** | fail-fast on engine thread; 1M lost-wakeup stress = 0 |
| F3 code-bloat tax | yes (regimes expanded) | **CORRECT** | per-`(A,M)`: async ~669 B/pair (g++); constant per pair → linear; build exp <1.2 |

**D2 tally: 10 survived + proven CORRECT, 1 disproven sub-claim.**

### D3 — Split-Enqueue Dispatch

| Claim | Survived | Proven | Number / evidence |
|---|---|---|---|
| S1 no virtual/RTTI, single `call *reg` | yes | **CORRECT** | `jmp *0x10(%rsi)`; 0 typeinfo/vtable/dynamic_cast |
| F2 typed enqueue O(1) in k; erased O(k) | yes | **CORRECT** | typed flat ~22.6 ns across k=1..64; erased g++ 22.7→77.7 ns |
| F3 erased 0-alloc; drain cycle-identical | yes | **CORRECT** | 0 alloc/4M erased; drain delta <0.03 ns typed vs erased |
| F1 typed tell dispatch | yes (stop_source alloc fixed by intrusive token) | **CORRECT** | g++ p50=60.1 / p99=85.0 / p999=157.5 ns (incl. ~17 ns timer floor); 0 alloc/2M. **Worst of the three** |
| F4 ask round-trip 0-alloc + shed | yes (real leak fixed) | **CORRECT** | g++ p50=148 / p99=225 / p999=366 ns; 0 alloc/500k; shed = `unexpected(overloaded)`, 0 heap-grow |
| S2 no field race; acquire on `link.next` | yes (edge corrected) | **CORRECT** | TSan clean; control (relaxed traversal) races `d->thunk` at drain |
| S3 static_cast never mistypes | yes | **CORRECT** | 8-actor-type erased stress, UBSan clean; DeadLetter wrap, never re-bake |
| C1 typed unhandled = compile error, no implicit erasure | yes | **CORRECT** | compile-fail matrix holds on both compilers |
| C2 AnyRef unhandled = runtime unexpected | yes | **CORRECT** | `unexpected(send_error::unhandled)`, drain-count unchanged; handled → same thunk as typed |
| C3 ReplyOrder Completion/SenderFifo | yes | **CORRECT** | Completion order proven; SenderFifo reorders off-lane |

**D3 tally: 10 survived + proven CORRECT, 0 disproven; but the most real bugs surfaced
during bring-up (ask-by-rvalue dangling, stack-use-after-return, Treiber free-list race,
admission-queue leak).**

---

## Decision — D1 (JumpTable-Dispatch) wins

Applying the closing ranking in order:

**(1) Safety gate — all three pass.** No safe/correct claim was left marked WRONG by the
prover. D1's one *fatal* red-team hit (the `slot_of` guard checked `Handles` — handler
*existence* — instead of `InProtocol` — protocol *membership* — yielding an out-of-bounds
`.rodata` indirect call on a bounds-check-free drain) had a **stated cheap fix** (one
predicate: redefine `Handles<A,M> := InProtocol<A,M> && has_handle<A,M>`), and the fix was
**proven CORRECT** (C1: handled-but-unlisted is now a compile error; slot==k unreachable).
D2's S1 sub-claim was disproven but it is a best-case performance clause, not a safety
LOSE-condition (no-virtual/no-vtable/no-RTTI all pass).

**(2) Proven beats claimed; disproven counts against.** Every design's *revised* claim set
was proven by executed C++ — none rests on unproven assertion. The distinguisher is the
disproven column: **D1 and D3 carry 0 disproven claims; D2 carries 1.** D2's disproven item
is precisely its headline differentiator — that the pre-baked thunk reaches *zero* indirect
under monomorphic LTO. Executed objdump refuted it on **both** toolchains (the queue-obtained
descriptor defeats store-forwarding; one `call *0x10(reg)` remains). With zero-indirect gone,
D2 collapses to architectural parity with D1 and D3: exactly **one** indirect call, no virtual,
no RTTI. D2's central "strictly cheaper dispatch" narrative does not survive execution.

**(3) Best MEASURED hot-path numbers among safe survivors.** D2 has the best *sync-tell* p50
(37 vs D1 47 vs D3 60 ns) — a real ~10 ns edge — but did **not** report an `ask` round-trip
latency distribution at all, and its edge rests on the just-refuted dispatch story. D3 has the
worst tell and ask numbers. **D1 is the only design with excellent MEASURED numbers on BOTH
verbs**: sync tell p50=46.8/p99=62.1 ns (well inside the 100/250 budget, 0 alloc) *and* the
**best measured `ask` round-trip** p50=83.1/p99=129.8 ns (~40× under the 5 µs budget, 0
pool-upstream) — while D3's ask is p50=148/p99=225 ns and D2's ask latency is unmeasured. On the
explicitly-required **code-bloat / ergonomics tax**, D1 also wins decisively: 260 B/actor,
linear, *independent of engine-wide message count*, plain fn-ptr table — versus D2's per-`(A,M)`
monomorphization (async ~669 B/pair) that multiplies with protocol size × actor count.

**(4) No design bends a core invariant.** All uphold single-executor, mailbox FIFO,
schedule-activations-not-messages, and zero-RMW drain (proven). No tiebreak needed here.

**Net:** D1 is the only design that is simultaneously (a) safety-clean after a proven cheap
fix, (b) **zero disproven claims**, (c) best-in-class on the *complete* hot path (both tell and
ask measured, best ask numbers), and (d) best on the mandated code-bloat tax — with the cleanest
coherent policy API: dense-slot O(1) dispatch that **beats** 008's runtime scan and **ties** a
hand-written switch on uniform *and* skewed mixes; always-typed `ActorRef<A>`; **async-only
`ask`** (avoiding the runtime-guarded `ask_sync` footgun that produced genuine TSan races in
both D2 and D3); member-field resources; and reply ordering that falls out of admission order
(Sequential → request order, Reentrant → completion order) with no map lookup.

### The winning API (each choice a policy/config knob)

- **Dispatch (005):** `using protocol = quark::Protocol<M1, M2, …>;` per actor. `slot_of<A,M>`
  is a `consteval` dense index; the send stamps a 2-byte `msg_slot_`; the drain does one
  `.rodata` `std::array<Thunk,k>` indexed indirect call. Beats the 008 sorted-flat scan on the
  local path; the 008 scan is retained only where a message type is not statically known.
- **Resources (005/004):** member fields — `Cached<Logger> log_;`, `PerMessage<PgSession>
  session_;` — as in 004's current example.
- **`ask` (006):** async-only, returns `quark::task<std::expected<R, quark::error>>`. **No
  `ask_sync`.** Off-lane bootstrap uses `quark::block_on(task)`, which **asserts it is not on a
  worker lane**.
- **Refs (006):** `ActorRef<A>` is always typed. Routers are homogeneous `ActorRef<A>` groups
  forwarding pre-stamped descriptors; dead-letter is a descriptor sink (no dispatch). No untyped
  ref is exposed on the send path, so "unhandled send = compile error" holds universally.
- **Reply ordering (001):** each `ask` is an independent one-shot routed through a **shard-pooled,
  monotonic-generation `ReplyCell`** (not the caller frame) with an `await_suspend`/complete
  state-CAS handshake. Sequential → replies in request order; Reentrant → replies in
  handler-completion order. Requests stay mailbox-FIFO regardless.

---

## Load-bearing corrections adopted from the proof (normative)

These were surfaced by the prover and must be specified as the *proven* form, not the original
sketch:

1. **Protocol-membership guard:** `Handles<A,M> := InProtocol<A,M> && has_handle<A,M>`;
   `slot_of` static_asserts membership (`found`), never handler existence. Both drift directions
   (unhandled, and handled-but-unlisted) are compile errors. `slot==k` is structurally unreachable.
2. **No per-send TLS epoch map.** Per-(sender,receiver) local FIFO is inherited from the mailbox
   `tail_.exchange` modification order (ADR-002). No `sender_epoch_` field, no hidden hash-map,
   no first-touch heap insert on fan-out.
3. **Descriptor carries a `ctx_` pointer, not inline context.** Layout `next_(8) + gen_state_(8) +
   payload_(8) + reply_(16) + ctx_(8) + msg_slot_(2) + kind_(1) + pad = 56 B ≤ 64 B`. Ambient
   `MessageContext` (deadline, trace, principal, headers, stop) lives in the payload-arena
   envelope; an inherited context resolves to `current_context()` with zero deref, an override to
   one pooled-pointer deref. It rides the same `next_.store(release)` publication edge.
4. **0 steady-state heap across sync tell, async handler, and ask:** async `task<>` frames come
   from a per-shard coroutine-frame pool; per-message cancellation uses the `gen_state` gen-gated
   CAS (queued) or an **activation-pooled** `std::stop_source` (running) — never a per-message
   `std::stop_source` (which allocates one control block, proven by positive control); payloads
   come from a fixed-size-class free-list shard pool that sheds (022) rather than reaching heap.
5. **Reply routing = shard-pooled monotonic-gen `ReplyCell` + win-arbitration CAS then release
   publish**, with the awaiter the sole cell-release point. This closes the UAF/ABA/lost-wakeup
   window that the original deposit-then-CAS sketch had (proven under ASan+TSan, ABA 200k /
   lost-wakeup 2M / Reentrant-cancel K=64 all clean).

---

## Residual risks

- **Protocol drift (ergonomics tax).** std C++23 cannot walk a member `handle` overload set, so
  `Protocol<…>` is a manual enumeration. A handler added but omitted from the list is silently
  unreachable — *sends still fail to compile* (safe), but the overload is dead code. Mitigation:
  a `QUARK_PROTOCOL` macro co-locating the list with the handlers, plus a lint. This is the price
  of the dense-slot table over a CPO scheme that needs no list.
- **Reply-lifetime correctness is a UAF class of bug.** Correctness hinges on the monotonic-gen
  `ReplyCell` + handshake. Proven now, but must remain a permanent ASan+TSan CI gate (concurrent
  cancel+complete, ABA reuse, sync-complete-before-suspend).
- **Weak memory (AArch64).** All S1/S2 evidence is x86-TSO. `msg_slot_`/`ctx_` ride the settled
  `next_` release/acquire edge and add no atomics, so they inherit the mailbox's open ARM64 gap:
  herd7/GenMC litmus required before ARM64 promotion. Not blocking on x86-64.
- **Skew workloads miss the *goal* (not the ceiling).** The indexed indirect call is always
  indirect; on a 95%-hot single type a predicted compare-chain switch can edge it out. Proven
  within-noise on both uniform and skew here, but a future hot-single-type path may add an opt-in
  devirtualizing switch. Ceiling (250 ns) is never at risk.
- **008 scan retained for the non-statically-typed path.** Any future untyped/wire-arrival path
  must recover the thunk via the 008 `type_index` scan; the dense slot must **never** be
  serialized or forwarded across actor types (it renumbers when handlers change).
- **Extended by [ADR-018](ADR-018-outbound-streaming-replies.md) (streaming replies).** The
  single-shot monotonic-gen `ReplyCell` above is **preserved verbatim** for ordinary asks. A
  multi-item streaming reply (an `ask` that returns a stream) does **not** reuse it: it rides a
  **distinct** `StreamReplyCell` (a single OPEN resolve, 16 B on the same `reply_` field) + a
  credit-ring + an in-band EoS — never a mid-stream cell reuse (which ADR-018's BASELINE proved
  is a lost/UAF class of bug). The permanent reply-lifetime ASan+TSan CI gate above **extends**
  to the multi-terminal (close/cancel/deadline) reclaim + cross-node replay surface, a strictly
  larger concurrent-reclaim state space than the single-shot cell.

---

## Spec recommendations

- **005-Developer-Model.md** — Close the two 005 open questions. (a) Dispatch: adopt the
  **dense per-actor jump-table** — `using protocol = quark::Protocol<…>;`, compile-time
  `slot_of<A,M>`, `static constexpr std::array<Thunk,k>` in `.rodata`. State the drain as one
  indexed indirect call and note it beats the 008 scan on the local path. (b) Resources: adopt
  **member-field declarations** (`Cached<Logger> log_;`), matching 004's example; drop the
  `ResourceSet<…>` alternative. Add the protocol-membership rule to Validation (compile-time):
  handled-but-unlisted is a compile error. Record the `QUARK_PROTOCOL` macro + lint as the
  drift mitigation.
- **006-Messaging-and-Addressing.md** — Resolve the three 006 open questions. `ask` is
  **async-only** (`task<std::expected<R,error>>`); **no `ask_sync`**; off-lane bootstrap uses
  `quark::block_on` that asserts it is not on a worker lane. `ActorRef<A>` is **always typed**;
  routers are homogeneous typed groups, dead-letter is a non-dispatching descriptor sink — **no
  untyped ref on the send path**. Strengthen the `Handles` concept to
  `InProtocol<A,M> && has_handle<A,M>` (protocol membership, not mere call-validity) so implicit
  conversions cannot smuggle an unlisted type to runtime.
- **001-Actor-Execution-Model.md** — Close the reply-ordering open question: **Sequential →
  replies in request order; Reentrant → replies in handler-completion order**; requests remain
  mailbox-FIFO; each `ask` is a one-shot routed via a **shard-pooled monotonic-generation
  `ReplyCell`** with an `await_suspend`/complete win-arbitration CAS + release-publish handshake
  (never the caller frame). Note reply delivery re-admits through the 015 gate, never inline on
  the completing lane.
- **004-Resources.md** — Confirm member-field resource declarations as the chosen ergonomics
  (remove the "open question" framing). Add that the ambient `MessageContext` is carried in the
  message's payload-arena **envelope** referenced by a descriptor `ctx_` pointer, and that the
  running-handler `stop_token` is backed by an **activation-pooled** `std::stop_source` (sized to
  the Reentrant/MaxConcurrency limit), never a per-message allocation.
- **008-Metadata-and-Startup.md** — Change the `DispatchTable` decision. Keep the **sorted flat
  array (`type_index → thunk`) only for the non-statically-typed path** (wire arrival / any future
  untyped forward). Add a **per-actor dense-slot function-pointer array** (`std::array<Thunk,k>`
  in `.rodata`, indexed by the process-local `msg_slot_`) as the primary local-dispatch structure,
  materialized at metadata compilation. State explicitly that the dense slot is process-local,
  renumbers on handler change, and is **never serialized**.
- **023-Performance-Targets-and-Budgets.md** — Record the measured baselines as regression gates:
  local sync tell dispatch p99 ≤ 250 ns (measured ~62 ns), local ask round-trip p99 ≤ 5 µs
  (measured ~130 ns), 0 hot-path allocation across sync tell / async handler / ask (hooked,
  including a positive control that a per-message `std::stop_source` = 1 alloc/msg), 0 cross-core
  RMW on the drain, and **dispatch code size ≤ ~260 B/actor, linear, independent of engine-wide
  message count**. Add the jump-table-vs-hand-switch parity check (uniform + skew) and the
  relaxed-publish TSan positive control as permanent CI gates.

---

## Tie-breaking experiment (if this decision is contested)

Evidence is not insufficient — D1 wins on the ranking as executed. The single experiment that
would most directly test D1-vs-D2 (the only real contender) is a **mixed-actor-fan-in macrobench**:
drain a high-entropy interleaving across ≥8 distinct actor types and report `perf stat -e
br_misp_retired.indirect` and p999 for (a) D1's per-actor dense-slot table vs (b) D2's per-`(A,M)`
baked thunk vs (c) a hand-written switch. Both D1 and D2 keep exactly one indirect call there; if
D2's ~10 ns sync-tell p50 edge holds *and* its BTB churn does not regress p999 under mixed fan-in
while D1's ask advantage is deemed less load-bearing, the tell-latency case for D2 strengthens.
Absent that result, D1's complete-hot-path superiority and zero-disproven record decide it.
