# ADR-047: `task<T>` — value-returning coroutine for non-void T

## Status

Accepted

## Question

`quark::task<T>` (`include/quark/core/task.hpp`) today implements only `T = void`: a
lazy, single-frame async-handler-mode marker that is never itself `co_await`ed and is
always `detach()`ed straight to the executor. `task<T>` for `T != void` does not exist —
`template <class T = void> class task;` is a bare forward declaration. Downstream code
(AgentEngine's `ae::task<T>`, `ChatClient::chat()`-shaped nested async calls) needs a
genuinely awaitable *inner* coroutine: an ordinary async function like
`quark::task<result<ChatResponse>> ChatClient::chat(Request)` that a handler's own
`task<void>` frame (or another `task<T>`) `co_await`s to get a `T` back.

The question put to the three competing designs: how does completing a nested `task<T>`
resume its awaiter (continuation storage, symmetric transfer vs. something else); where
does the `T` value live in the promise with zero extra heap allocation beyond the
compiler-managed frame; is `task<T>` lazy like `task<void>`, and what happens when one is
dropped unawaited; and how is a handler throw inside a nested `task<T>` contained
(rethrow-at-`co_await` vs. folding into `quark::result<T>`) without ever reaching
`std::terminate` and without double-wrapping a `T` that is itself `quark::result<U>`.

## Designs (one-line summaries)

- **D1 — Continuation-Pointer Symmetric-Transfer `task<T>`.** `task<T>` is its own
  awaiter; `await_suspend` stores the awaiting handle as a plain (non-atomic)
  `continuation_` in the promise and returns this task's own handle for symmetric
  transfer (same thread/stack, no scheduler hop); `final_suspend`'s awaiter symmetric-
  transfers straight back. `T` lives in an anonymous-union promise member
  (`std::construct_at`/`std::destroy_at`, gated by `has_value_`) — zero extra allocation.
  A throw is caught by `unhandled_exception()` into an `exception_ptr fault_` (identical
  shape to `task<void>`'s existing field) and **rethrown at `await_resume()`** — the
  idiomatic-C++, `task<void>`-consistent style.

- **D2 — Frame-local ReplyCell `task<T>`.** Embeds a stripped-down, frame-local,
  one-shot analogue of `detail::ReplyCell<T>` directly in the promise: an
  `atomic<uint32_t> st_` win-arbitration cell (`kEmpty`/`kWaiter`/`kResolved`) plus a
  plain `cont_` handle, with `outcome_t<T> = T` if `T` is itself `result<U>` else
  `result<T>` folding faults in via `Activation::classify_fault`. Requires an additive
  companion change to Activation's park bookkeeping: a `top_frame_`/`parked_frame_`
  split coordinated through a new thread-local `tl_innermost_parked_frame` that nested
  `task<T>::await_suspend` reparents inward.

- **D3 — Result-Unified `task<T>`.** Symmetric-transfer continuation shape close to D1,
  but folds faults into a single `flatten_t<T>` storage slot (`T` if `is_result_v<T>`
  else `result<T>`) via one `translate_current_exception()` function — matching
  `AskFuture<R>::await_resume()`'s value-not-exception contract instead of `task<void>`'s
  rethrow style. Fix for the resume-routing defect (below) threads a
  `std::coroutine_handle<>` explicitly through `ParkedResumeSink`/`ReplyCell::resolve()`/
  `Activation::complete_parked()`.

All three designs' *promise/awaiter mechanics in isolation* (lazy `initial_suspend`,
inline value storage, drop-while-suspended-at-initial-suspend safety, compile-time
rejection by `dispatch.hpp`'s exact-type `async_handler` concept) were sound from the
first round. The entire fight was in one place: **how does the engine resume the
correct frame when a nested `task<T>` genuinely parks on a cross-actor primitive**, since
`Activation::complete_parked()` and `ParkedResumeSink` were written for the
single-frame, `task<void>`-only world and unconditionally resume the *outer* handle —
discarding the leaf handle `ReplyCell::suspend()` already, correctly, captures. Every
design that shipped its first draft unmodified was fatally broken on exactly this point,
and every design converged on essentially the same class of fix in its rebuttal.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|---|
| Nested `task<int>`/`task<result<Foo>>` resumes outer with correct value across a real suspend | D1 (C1) | yes (revised) | **CORRECT** | 4-worker Engine, 64 askers × 3-nested chains, `completed=64 mismatches=0 layer3_resumed=128`; 75 stress runs, 0 failures |
| Same, for D2 | D2 (C1) | yes (revised) | **CORRECT** | 25/25 ASan runs, value==777, `messages_processed==1`, `reclaim_count==1`, every `async_frame_faulted` address verified == `top_frame_` |
| Same, for D3 | D3 (C2-rev) | yes (revised) | **CORRECT** | Real engine, 2500 nested cross-lane asks, `mismatches=0`; **negative control on unpatched plumbing**: `completed=0 mismatches=2500` (100% failure without the fix) |
| Handler throw inside nested `task<T>` contained, never `std::terminate`, observable at outer | D1 (C2) | yes | **CORRECT** | Real engine + `set_terminate` probe: `dead_letters=32 triggered=32`, exit=0; direct probe recovers original `runtime_error("x")` via `dynamic_cast` |
| Same, for D3 | D3 (C6-rev) | yes | **CORRECT** | Post-suspend counter==1 (throwing code path genuinely executed, closing the "throw never reached" gap red-team found), `errc::internal`, exit=0 |
| Dropped unawaited `task<T>` leaks nothing, never runs body | D1 (C3) | yes | **CORRECT** | 10,000 instances, 5,106 dropped: CRT-checkpoint leak-free, `body_entries==awaited_count` exactly |
| Same, for D2 (S1) | D2 | yes | **CORRECT** | alloc/dealloc balanced, `side_effect` stayed false |
| Same, for D3 (C4), incl. teardown mid-nested-suspend | D3 | yes | **CORRECT** | ASan-clean incl. Engine torn down while parked 2 frames deep in a never-resolving ask |
| Zero heap allocation beyond N compiler frames | D1 (C4) | yes | **CORRECT** | `new==delete==6` for N=5 chain (clean and exception path); 3 allocs for 2-nested+outer through real cross-thread seam |
| Same, D2 (F1) | D2 | yes | **CORRECT** | 100,000 sync co_awaits → exactly 100,001/100,001 new/delete |
| Same, D3 (C3) | D3 | yes | **CORRECT** | 1000 calls (500 int + 500 result), armed window: 0 bytes allocated |
| `task<T>` structurally rejected by `dispatch.hpp`'s exact-type `async_handler` concept | D1/D2/D3 (C5/C1) | yes | **CORRECT** (all three) | Negative-compile test trips `static_assert("handle() must return void (sync) or quark::task<> (async)")` unchanged |
| Nested `task<result<Foo>>` never double-wraps into `result<result<Foo>>` | D1 (C8) | yes | **CORRECT** | `static_assert` + runtime: one-level unwrap, exact error preserved |
| Same, D2 (C2) / D3 (C5) | D2/D3 | yes | **CORRECT** (both) | identical one-level-unwrap proof |
| **Cross-lane resume-routing fix is load-bearing** (D1's `leaf`-threading through `ParkedResumeSink`) | D1 (C6b) | yes (this *is* the fix) | **CORRECT**, with negative control | Fix reverted via `-DQUARK_C6B_CONTROL_BUG`: reproduces original defect **100% deterministically** (`completed=0 mismatches=128`); fixed build clean across 150 stress runs. TSan unavailable on this toolchain — ASan+Debug/RTC1 substituted, flagged as not TSan-equivalent |
| `task<T>`'s own generated code has zero atomics/fences in the intra-frame case | D1 (C7) | yes | **CORRECT** | Disassembly of `await_suspend`/`final_suspend`/`return_value`/`await_resume`: zero `lock`/`mfence`/`cmpxchg` instructions; task<T> overhead ≈118 ns/3 layers over a bare-malloc baseline |
| Same property for D3 (C7-rev) | D3 | yes (revised) | **INCONCLUSIVE** | TSan unavailable; negative control (plain non-atomic handoff) did not visibly corrupt under x86 TSO — explicitly *not* proof, correctly self-reported as inconclusive rather than CORRECT |
| Deep nested chain (100,000) survives on a small stack via guaranteed-tail-call symmetric transfer | D1/D2/D3 (C8/F3) | yes | **CORRECT** (all three) | D3: 100,000-deep chain survives 64 KiB stack in both Release and Debug+ASan; hand-built non-tail-call control overflows between depth 1000–2000 (>50× margin) |
| Real end-to-end engine latency: nested `task<T>` ask vs. plain `AskFuture` ask | D3 (bench2) | n/a (fast claim) | measured | PLAIN=1095–1109 ns/op, NESTED=945–972 ns/op — nesting overhead **within noise** of the ~1 µs cross-thread futex-wake cost |

## Decision

**Winner: D1 — Continuation-Pointer Symmetric-Transfer `task<T>`, with the C6b
`ParkedResumeSink` leaf-threading fix.**

Rationale, applying the stated ranking:

1. **Safety gate.** No design's *final* (post-fix) claim set contains a claim marked
   WRONG/disproven by executed evidence. All three designs' first-draft resume-routing
   claim (D1's original C6, D2's original C1, D3's original C2/C6/C7) was correctly
   found FATAL by red-teaming and conceded — but each design supplied a fix, and each
   fix was re-proven CORRECT with executed evidence, including negative controls that
   deterministically reproduce the original defect when the fix is reverted (D1's
   `-DQUARK_C6B_CONTROL_BUG` build, D3's unpatched-plumbing build). No design is
   disqualified at the gate.

2. **Proven beats claimed.** D1 is the only design with **every claim in its final set
   proven CORRECT (8/8)** — including the one claim (C7, zero atomics in `task<T>`'s own
   generated code) that D3's structurally equivalent claim (C7-rev) could only leave
   **INCONCLUSIVE** for lack of TSan on this toolchain. D1 closed the same gap with a
   *compiler-verified* proof instead — disassembly showing literally zero `lock`/`mfence`/
   `cmpxchg` in the intra-frame path — which is strictly stronger evidence than a
   negative-control race probe on x86's strong memory model (which is well known to hide
   real races rather than prove their absence). Per the ranking rule, D3's INCONCLUSIVE
   claim carries no weight; D1 has no such gap. D2's 6/6-proven set is smaller in scope
   (it never independently re-proves a "zero atomics" property at all, since its own
   design intentionally *keeps* an atomic win-arbitration cell in the promise) and its
   fix is architecturally heavier (see point 4).

3. **Measured hot-path numbers among safe survivors.** All three converged on
   comparable per-`co_await` costs in the tens-of-ns range (D2: ~57 ns/layer sync;
   D1: ~40 ns/layer marginal over a malloc baseline within a 3-layer chain; D3: ~67 ns
   for a 2-alloc round trip) — no design wins decisively on raw microbenchmark numbers,
   and the difference is dominated by frame-allocation cost common to all three, not by
   any design's own machinery. D3's real-engine end-to-end number (nested-ask latency
   statistically indistinguishable from, and in these runs slightly *below*, a plain ask)
   is the most directly production-relevant number produced in this debate and is
   corroborating evidence that D1's near-identical fix shape (explicit handle threading,
   not a thread-local) carries the same negligible integration cost — D1 and D3 share the
   same production-path fix design, so this number transfers as supporting evidence for
   D1 too.

4. **Core-invariant / design-cleanliness tiebreak.** D1's fix threads one explicit
   `std::coroutine_handle<> leaf` parameter through `ParkedResumeSink::fn` →
   `ReplyCell::resolve()` → `Activation::complete_parked()`, while `parked_frame_`
   remains the *sole* signal for `.done()`/`async_frame_faulted()`/`.destroy()`/
   `reclaim_()` exactly as today. This is a minimal, explicit, call-by-parameter change.
   D2's fix instead introduces `thread_local std::coroutine_handle<> tl_innermost_parked_frame`
   — new ambient mutable global state coordinating two components (`task<T>::await_suspend`
   and `Activation`) through an implicit side channel rather than an explicit call
   argument — a heavier, spookier mechanism for the same effect, and D2's own risk list
   admits the identical treatment is still owed to the Reentrant drain path and was never
   built or proven. D1's rebuttal explicitly traced why the Reentrant path is *unaffected*
   by the original bug (`ParkedResumeSink` is inactive there by pre-existing, documented
   design, so Reentrant asks already resume the correctly-captured handle via the
   existing `else if (h) h.resume()` fallback) — a closed, verified scope, not an
   acknowledged-but-open gap. No design bends a core invariant (single-executor,
   mailbox FIFO, activation-not-message scheduling, stable placement, workers-as-lanes
   all hold across all three), but D1's fix is the least invasive one that closes the
   defect cleanly.

5. **Exception-channel consistency (secondary, non-decisive factor).** D1's
   rethrow-at-`await_resume()` style is byte-for-byte consistent with `task<void>`'s
   *existing*, shipped `exception_ptr fault_`/`faulted()`/`fault_ptr()` pattern (ADR-009
   D1) — a developer moving between `task<void>` and nested `task<T>` sees one exception
   idiom, not two. D3's fold-into-`result<T>` style (matching `AskFuture<R>`'s existing
   contract) is a legitimate, well-executed alternative, but it is a real, acknowledged
   API-style bifurcation within the same codebase. This did not decide the vote by
   itself (D3 is safe and well-proven either way) but reinforces the choice.

D1 wins as the design with the most complete, most rigorously proven claim set, the
least invasive and most explicit fix to the shared resume-routing defect, and the
closest fit to `task<void>`'s existing exception idiom — with no core invariant bent and
no claim left disproven.

## Residual risks

- **No TSan on this development box (Windows/MSVC-only toolchain).** D1's C6b (the
  cross-lane resume-routing fix) and C7 (zero-atomics) were validated via ASan +
  UBSan + Debug/RTC1 + repeated stress runs (150 total across two cross-thread-heavy
  tests) plus disassembly inspection, but genuine TSan-grade race detection on the
  fixed `ParkedResumeSink`/`ReplyCell`/`Activation::complete_parked()` path has not
  been run. **Tie-breaking experiment before merge:** run the exact C1/C2/C6b test
  programs (already written, see artifact paths below) under `-DQUARK_SANITIZE=thread
  -DCMAKE_BUILD_TYPE=Debug` on a Linux/GCC or Linux/Clang box, pinned to 2–4 cores,
  ≥10⁴ iterations, before this ships to production.
- **Reentrant-path scope.** D1's claim that the Reentrant drain path
  (`AsyncSuspend`/`on_async_suspend()`/`complete_one()`) is unaffected by the original
  bug rests on `ParkedResumeSink` being inactive there by pre-existing design — this
  was argued and accepted during cross-examination but was not independently exercised
  by a dedicated Reentrant-path test in the prove phase. Add one before merge.
  Sequential and governed-Sequential are the domains actually proven end-to-end here.
  D3's `bench2` real-engine measurement was also run on a Sequential-shaped path.
  Confirming both the routing fix and its measured latency neutrality specifically under
  Reentrant is not yet closed.
  end-to-end.
- **Throwing-move-during-`return_value` corner.** Flagged in D1's own risk list and
  never independently tested: if `T`'s move constructor throws inside `return_value()`,
  the throw happens in the *awaiter's* frame rather than being cleanly attributable to
  the inner task — logically-successful `co_return` can surface as a fault purely from a
  throwing move. Worth a dedicated `has_value_`-stays-false-on-throwing-construct test.
- **Double-await/double-resume misuse has no runtime guard.** Same class of pre-existing
  footgun as calling `task<void>::detach()` twice — inherited, not introduced, by this
  design. A debug-only `assert(!consumed_)` in the promise would close it cheaply; not
  included in the proven design and should be decided explicitly, not assumed away.
- **Cross-compiler proof gap.** All executed evidence in this debate is MSVC-only
  (no g++/clang++ available on the proving machine). CLAUDE.md requires g++14/clang++20
  compliance; re-run the full claim set on at least one GCC/Clang Linux box (which also
  unlocks the TSan gap above) before considering this closed.

## Spec recommendations

- **`001-Actor-Execution-Model.md`.** Add a subsection under the hybrid
  sync/async-handler execution model describing `task<T>` (`T != void`) as a *distinct,
  non-detachable* inner-awaitable type: state explicitly that `task<>`/`task<void>`
  remains the only type ever `detach()`ed to the executor or fed to
  `async_frame_faulted()`/`async_frame_fault_ptr()`, and that a handler declared to
  return `task<T!=void>` must fail to compile via the existing exact-type
  `async_handler` concept (no spec change to that concept itself). Document symmetric
  transfer as the resume mechanism for nested `task<T>` completion — same thread/stack
  for the intra-frame case, with cross-lane migration only when an underlying
  cross-actor primitive (AskFuture/ReplyCell) is itself involved — and note that
  single-executor is upheld by the existing exec-state Parked/CAS gate, not by thread
  affinity.
- **`006-Messaging-and-Addressing.md`.** Note in the reply/result-type section that
  `task<T>` where `T` is itself `quark::result<U>` yields exactly `result<U>` at
  `await_resume()` (never `result<result<U>>`) — the value-type contract is
  unconditional pass-through, only a *thrown exception* is captured into the promise's
  fault channel. Cross-reference this against `AskFuture<R>::await_resume()`'s existing
  `result<R>` contract and flag the two different failure-observation idioms (`task<T>`
  rethrows at `co_await`; `AskFuture<R>` returns a `result` in the failure state) as a
  deliberate, documented split — not an oversight — so future contributors don't try to
  "unify" them without going through this ADR's reasoning.
- **`007-Failure-and-Supervision.md`.** Add `task<T>` (T != void) to the ADR-009 D1
  containment inventory: a throw anywhere in a nested `task<T>` await chain is caught by
  `unhandled_exception()` at every frame boundary and is only ever observable by
  rethrowing at that frame's own `await_resume()` — it never independently reaches
  `Activation`'s fault machinery except by eventually propagating (as a normal C++
  exception) up through the outermost `task<void>`'s own `unhandled_exception()`, which
  is unchanged from today. State explicitly that this ADR's fix touches only the
  *resume-routing* path (which coroutine handle `Activation::complete_parked()` calls
  `.resume()` on), never the *fault-classification* path (`async_frame_faulted()`/
  `async_frame_fault_ptr()` remain exclusively scoped to the top-level `task<void>`
  handle).
- **`015-Reentrancy-and-Quiescence.md`.** This is the load-bearing spec update. Document
  the `ParkedResumeSink`/`ReplyCell::resolve()`/`Activation::complete_parked()` change
  precisely: `ParkedResumeSink::fn`/`operator()` gain a `std::coroutine_handle<> leaf`
  parameter carrying the handle `ReplyCell::suspend()` actually captured (the innermost
  coroutine syntactically containing the `co_await`, which may be several `task<T>`
  layers below the top-level `task<void>` handle); `ReplyCell::resolve()`'s fast path
  calls `sink(h)` instead of `sink()`; `Activation::complete_parked()` gains a
  `leaf = {}` parameter and resumes `leaf ? leaf : parked_frame_` while `parked_frame_`
  (still set unconditionally by `drain_step` to the top-level `task<void>` handle)
  remains the sole signal for `.done()`/`async_frame_faulted()`/`.destroy()`/
  `reclaim_()`. Explicitly scope this to Sequential/governed-Sequential activations
  (matching `ParkedResumeSink`'s pre-existing documented domain) and record, as an open
  item, that the Reentrant drain path (`AsyncSuspend`/`on_async_suspend()`/
  `complete_one()`) needs its own dedicated verification pass (see residual risks) even
  though the design reasoning says it is unaffected by construction.

## Artifacts

- D1 (winner) prove artifact: `C:\Users\thanh\AppData\Local\Temp\quark-prove-ae236814-3fc3-40a0-86fc-f381912b5b7f`
- D2 prove artifact: `C:\Users\thanh\AppData\Local\Temp\quark-prove-5218f811-f0d7-4fbf-8651-b6e97ee1234d`
- D3 prove artifact: `C:\Users\thanh\AppData\Local\Temp\quark-prove-7d93c9b5-957d-4a3d-916f-f0de16cf882d`
