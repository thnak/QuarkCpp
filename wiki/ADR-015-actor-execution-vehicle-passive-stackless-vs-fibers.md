# ADR-015 — Actor execution vehicle: passive + stackless core, scoped off-hot-path fiber adapter

- **Status:** Accepted
- **Date:** 2026-07-15
- **Supersedes/relates:** 001 (execution), 002 (scheduler), 015 (reentrancy/quiescence), 024 (streaming), 023 (budgets); builds on ADR-002/003/004 (mailbox Dekker close-out), ADR-007 (dispatch + ReplyCell win-arbitration), ADR-008 (config HotCell), ADR-010 (byte-identical gate), ADR-014 (streaming async-suspend real-scheduler gate).

## Question

What runs an actor's handler? The user's hypothesis: because **most actors are idle at any instant**, pool and reuse **stackful green threads (fibers)** across actors to cut cost. The incumbent is **passive actors, run-to-completion on an N-worker lane pool, with stackless `quark::task<>` coroutines** for async — an idle actor is state+mailbox with no stack; a suspended async handler pins only its live coroutine frame. Debate ran as a genuine search across four vehicles on six measured axes: (a) idle footprint, (b) suspended footprint, (c) sync dispatch latency/throughput, (d) async suspend/resume round-trip, (e) function-coloring / blocking-interop, (f) 0 hot-path heap alloc — with single-executor and per-actor mailbox FIFO as hard safety gates.

## The vehicles (one line each)

1. **Passive + Hybrid-Stackless (incumbent).** 192 B / 3-line `ActivationCB`, no stack; sync handler runs inline on the worker's own stack via one indexed indirect call (ADR-007); async returns `quark::task<>` and suspends the *activation* (not the thread), pinning only the live coroutine frame from a home-shard pmr pool; completion re-admits through the 015 gate.
2. **BorrowedFiber.** Pool of 16 KiB stackful fibers borrowed by a worker *after* it wins the actor's `Idle→Running` CAS, returned when the drain completes; entered only for compile-time `BlockingHandler` actors; idle actors hold no fiber. Per-actor, per running/suspended actor granularity.
3. **Hybrid: passive+stackless core + opt-in off-hot-path Blocking/Fiber adapter.** Incumbent verbatim as the core; a bounded, opt-in adapter serves only `quark::blocking<>` (thread-backed, default) / `quark::fiber<>` (stackful, gated) handlers, suspending the activation like an async `co_await` and offloading the un-colorable leaf off the mailbox lane.
4. **Fibra: fiber-per-actor, tiny growable stacks (BEAM/Go-inspired).** Every actor owns a stackful fiber for its lifetime. Self-disproving: growable stacks are infeasible in C++ (interior pointers, foreign C frames), guarded stacks hit the VMA wall, and the sync path pays 2 context switches.

## Safety gate (disqualification pass)

All four upheld single-executor and per-actor FIFO **in their proven form**, but two designs required correction to get there and one is disqualified as a *core* vehicle:

- **Fibra** — its original `CORRECT1` ("no new atomic, `f.cont` rides the head_ edge") was **withdrawn**: the suspend branch skips the release-store, so `f.cont` had no happens-before to the resumer and the reactor was armed before the park was published (a real data race + premature-resume). The corrected `CORRECT1R` (distinct `Parked` exec-state, release-publish of `f.cont`, `Parked→Running` acquire re-admit through 015, **origin-worker-pinned resume**, 015 in-flight registration) proved CORRECT under TSan/ASan. **But Fibra is disqualified as the CORE vehicle on axis (c):** `FAST1` proved warm p999 = 276.8 ns and **cold-resume p50 = 591.9 ns / p999 = 6853 ns — breaching the 250 ns HARD ceiling.** A vehicle that fails a hard latency gate on the steady sync path cannot be the core.
- **BorrowedFiber / Hybrid adapter** — both conceded that the park/resume handshake is a **new StoreLoad rendezvous**, not "inherited verbatim," and both closed it with the ADR-007 ReplyCell-style win-arbitration CAS + an explicit non-`Idle` `Parked` seal. Both then proved single-executor and FIFO CORRECT (Hybrid's `-DQUARK_PARK_IDLE=1` control crashes/double-executes, proving the seal is load-bearing).

Incumbent single-executor (`S1`) and FIFO (`C1`) proved CORRECT: multi-worker TSan clean, concurrent-executor counter ≡ 1, both mandatory controls fire (`ADVANCE_SUSPENDED` → max_concurrent 8592 + double-dispatch; `RELAX_EXEC` → lost-wakeup hang). Honest caveat carried forward: the steady sync drain executes **one consumer-local uncontended `lock cmpxchg`/msg** (the `gen_state` claim-vs-late-cancel CAS), so the literal "zero lock op on the drain" wording is imprecise — but it is **0 cross-core RMW**, which is the 023 gate that matters, and that is met.

## Evidence table (claim → survived red-team? → proven? → number)

Rig for all executed evidence: x86-64 Linux, GCC 14.2 + Clang 20.1, `-O3 -march=native -flto`, pinned core, percentiles. Incumbent numbers taken on a sub-reference Xeon Silver 4208 @ 2.1 GHz (no turbo) → ~1.6× headroom scaled to the 023 Zen4/SPR reference.

### Core vehicle — Passive + Hybrid-Stackless

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 sync latency/throughput, 0 ctx switch | yes (fence-elision revision) | **CORRECT** | gcc p50 67.8 / p99 96.4 / p999 193.8 ns; clang p50 78.3 / p99 102.1 / p999 204.3 ns (all < 250 hard; gcc < 100 goal); drain 41.8–43.6 M/s/core; full-lifecycle 12.3–13.9 M/s. Producer Dekker fence elided to **0 instr** on x86; 0 context-switch asm |
| F2 idle footprint | yes (config off Line A) | **CORRECT** | **192.0 B/idle-actor at 1e6 AND 1e7** (identical), 5.59 M/GB, 1 non-scaling stack VMA |
| F3 suspended footprint (depth-parameterized) | yes (absolute bound withdrawn) | **CORRECT** | stackless D1=656 … D8=2896 (<4 KB page) … D64=20817 B; **fiber control ≥6088 B + 1 guard VMA even at D1** → stackless wins all D≤8; 0 global new / 5e6 suspends |
| S1 single-executor + 0 cross-core RMW | yes | **CORRECT** | concurrent-executor ≡ 1; TSan clean; controls fire (double-dispatch / lost-wakeup hang) |
| S2 0 alloc on sync drain | yes | **CORRECT** | 0.000000 alloc/msg over 1e7, both compilers; control fires 100000 |
| C1 FIFO + no advance past suspended | yes | **CORRECT** | dispatch order ≡ enqueue order, max_concurrent ≡ 1, head_ frozen |
| C2 coloring — honest split | yes | **CORRECT** | opaque blocking: **neither stackless nor fiber suspends in place** (fiber carrier also blocks 150 ms); offload hop measured; fiber wins only cooperative-yield ergonomics |

### BorrowedFiber (scoped candidate)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 idle | yes | **CORRECT** | 320 B/idle-actor flat 1e6↔1e7, 0 fiber-stack bytes for idle; +64 B fiber bookkeeping (≈+8 B if park cell shard-pooled per design) |
| F2 suspended (owned loss) | yes | **CORRECT** | **16587.8 B resident + 20 KiB mapped per suspended actor; 1e5 parked = 1.66 GB** vs incumbent 104–137 B (~120×) |
| F3a zero-cost default | yes | **CORRECT** | `drain_inline` objdump = 0 fiber/malloc/mmap ops; handler = ADR-007 `call *0xf8(%rbx)` |
| F3b switch cost (bimodal) | yes (10–20 ns absolute withdrawn) | **CORRECT** | hot p50 19.6 ns/switch; **cold rotating p50 95 ns, p99 262 ns/switch (~524 ns/pair)** → cold B=1 local-tell blows 250 ns (why it must stay scoped) |
| F4 throughput floor | yes | **CORRECT** | inline 52.4/47.9 M/s; fiber B≥8 30–34 M/s (> 4 M floor); B=1 hot 14.5/15.7 M/s |
| S1 single-executor + park/resume | yes (corrected to FiberParkCell + Parked) | **CORRECT** | concur_violations 0; TSan clean; injected fiber race → 61 TSan reports; broken arbitration → wedge rc124 |
| C1 FIFO | yes | **CORRECT** | fifo_violations 0 over 1e6 msgs, 32 workers |
| **C2 blocking-interop WIN** | yes | **CORRECT** | **5 uncolored frames suspend, lane freed with 2000 actors parked, all locals byte-intact; stackless `co_await` in plain `int()` = compile error; 16 KiB overflow → clean guard SIGSEGV; 512 KiB policy completes** |
| F5 0 alloc on borrow/return | yes | **CORRECT** | operator_new 0, cold_grow 0 over 1e7 cycles; starved-pool control → cold_grow 1 |

### Hybrid adapter (scoped candidate; one disproven claim)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| F1 sync-thunk byte-identity + no regression | yes (whole-loop identity conceded) | **CORRECT** | thunk byte-identical (7 insns g++/6 clang); 76→74 M/s co-resident, 12 M/s under completion storm |
| F2 0 RMW on drain + 3-party rendezvous | yes | **CORRECT** | isolated drain 0 lock/xchg/cmpxchg/mfence; Dekker litmus 0 lost / 20M with fence, 190k–1M without |
| **F3 thread-backed adapter latency** | no | **WRONG** | claimed p99 ≤ ~3 µs; **measured p50 62.1 µs / p99 142.8 µs (~20× over)**, min 2.08 µs. (Fiber-switch sub-component p50 16.2 ns is fine.) |
| S1 ownership-transfer safety | yes (bogus control replaced) | **CORRECT** | 160k dispatches TSan clean; `PARK_IDLE` control → heap-UAF + race |
| S2 FIFO explicit Parked phase | yes (Idle→Scheduled readmit conceded) | **CORRECT** | 6e6 msgs, 0 inversions; `PARK_IDLE` control → SIGSEGV |
| S3 no per-actor stack idle/async-suspended | yes | **CORRECT** | 384 B idle flat 1e6↔1e7; async pins 64 B frame; carriers capped at P |
| C1 asm-free blocking-interop | yes | **CORRECT** | 50 ms C `nanosleep` 3 frames deep, no `co_await`; co-resident lane 9.31 M/s during block; FIFO 0 |
| C2 fiber cross-foreign-frame | yes (gated) | **CORRECT** | SAX C parser `co_await`s mid-callback; stackless negative control leaves sum 0 (5 pending) = physically cannot |
| C3 bounded (022 shed) | yes | **CORRECT** | P=4, 40 calls → 4 stacks, queue depth 8, shed 28 |
| **C4 fiber multiplex behind foreign frame** | yes (new search win) | **CORRECT** | **32 foreign-frame handlers multiplexed on 1 cooperative driver across colorable yield points (nested ask); thread-backed carrier ceiling is P**  |

### Fibra (disqualified as core)

| Claim | Survived | Proven | Number |
|---|---|---|---|
| CORRECT1R single-exec + FIFO (corrected) | yes (original CORRECT1 withdrawn) | **CORRECT** | 0 concurrent-exec / 0 FIFO / 0 premature over 5e5 msgs; Parked necessary (Idle-publish control → 176k–189k premature resumes) |
| CORRECT2 uncolorable-chain suspend | yes | **CORRECT** | opaque libfoo.a, 3-frame chain suspends on interceptable fd, lane serviced others, resume inside C frame; boundary control: uninterceptable primitive → no win |
| **FAST1 sync dispatch (LOSES)** | yes | **CORRECT** | warm 8.33 M/s p50 62.1 ns (below 10 M goal); **cold p50 591.9 / p999 6853 ns → breaches 250 ns HARD → core rejected on (c)** |
| FAST2 batch amortization | yes | **CORRECT** | added-ns/msg falls ~1/batch; batch-256 still ~1.9% slower; tail rises with depth |
| FAST3 scoped per-invocation win | yes ("1e5+" narrowed) | **CORRECT** | uncolorable-chain per-wait ~96 ns engine vs ~1–3 µs thread hop; VMA-bounded (~500k this rig / ~32k default) |
| SAFE1 VMA wall | yes | **CORRECT** | guarded stacks ENOMEM at **524267** (=max_map_count/2); incumbent 1e7 actors, +1 VMA total |
| SAFE2 guardless-slab unsafe | yes | **CORRECT** | 2 KB local on 512 B slab clobbers neighbor canary; **ASan blind** (no redzone past raw slab boundary) |

## Trade surface across all vehicles (the axes the judge compares on)

| Axis | Incumbent (core) | BorrowedFiber | Hybrid adapter | Fibra (per-actor) |
|---|---|---|---|---|
| (a) Idle B/actor | **192** (5.59 M/GB) | 320 (0 stack) | 384 (0 stack) | ~4096 floor once run; guarded variant **won't boot** at 1e6 (VMA wall) |
| (b) Suspended B/actor | **656 B–2.9 KB (D≤8)**, no guard VMA | 16.6 KB + guard (~120×) | 64 B async / 128 KB per in-flight blocking (P-capped) | 4 KB (whole live stack) |
| (c) Sync p50 / p99 / floor | **67.8 / 96.4 ns**, 41.8 M/s drain, 0 ctx switch | inline path == incumbent; fiber B=1 hot 69 ns, **cold p99 > 250** | inline path == incumbent | warm 62 ns / **cold p999 6853 ns → fails HARD** |
| (d) Async suspend/resume | 92.7–113.6 ns (frame pool + 015 re-admit) | 2 switches: hot ~40 ns pair | stackless path == incumbent; fiber switch p50 16 ns | ~96 ns (2 switch + CAS) |
| (e) Coloring / blocking-interop | offload only (fiber carrier also blocks on opaque syscall) | **WIN: uncolorable chain suspends in place** | asm-free thread offload + gated fiber **WIN + C4 multiplex** | WIN but wrong granularity |
| (f) 0 hot-path alloc | **0/1e7** | 0/1e7 | 0/steady | 0/1e6 |

Notes that decide it: fibers **win axis (d)** for fine-grained suspend-heavy async — the incumbent's own benchmark measured stackless round-trip 92.7–113.6 ns vs a register-only fiber switch ~40 ns/pair (~2.5–3×). But that win is bought with the axis-(b) 16 KiB/suspended-actor cost (~120×) and the VMA ceiling, so it is a **scoped** win, never a core one. On axis (e) the honest split is decisive: for an **opaque blocking syscall** neither stackless nor fiber suspends in place (both offload — incumbent C2 and Hybrid F3/C1 proved the fiber carrier blocks too), so no fiber is justified there; the fiber's **irreducible, physically-unique win is the un-colorable/un-recompilable foreign-C chain on interceptable I/O** (BorrowedFiber C2, Fibra CORRECT2, Hybrid C2 — all proved the stackless `co_await` is a compile error), **plus** the newly-discovered **C4 multiplexing** across colorable yield points trapped behind a foreign frame (proven only in the Hybrid design).

## Decision

**CORE vehicle: Passive + Hybrid-Stackless (the incumbent), unchanged.** It is the only vehicle that passes the safety gate *and* wins the mostly-idle economy + throughput axis without qualification: 192 B/idle-actor (5.59 M/GB, ~28× under the 023 2 KB ceiling), depth-bounded suspended footprint that beats a fiber at every D≤8 with no guard VMA, sync p99 96.4 ns / 41.8 M/s drain / 12–14 M/s full-lifecycle with **zero context switch and zero hot-path allocation**, all proven CORRECT with executed evidence and both mandatory safety controls firing. Every fiber-per-actor / universal-fiber vehicle either fails the 250 ns HARD sync ceiling on cold resume (Fibra `FAST1`), pins a per-suspended-actor stack (~120× — BorrowedFiber `F2`), or cannot boot the mostly-idle population at all (Fibra `SAFE1` VMA wall). **The user's "pool green threads across idle actors to cut cost" hypothesis is disproven:** a fiber pins bytes and a VMA the moment it runs, and a fiber blocked in an opaque syscall pins its carrier OS thread exactly like a pooled thread — it gives no density or multiplexing win for the blocking case that motivated it.

**A fiber facility DID earn a SCOPED place** — but as an **opt-in, off-hot-path `BlockingHandler` adapter, never the core vehicle and never per-actor**:

- **Default form — thread-backed `quark::blocking<>` (no context-switch asm).** Covers the common case (opaque blocking syscalls / legacy C where the leaf must offload anyway). Delivers the full interop win asm-free: proven to free the mailbox lane (co-resident actor ran at 9.31 M/s during a 50 ms blocking call), preserve single-executor + FIFO via the explicit `Parked` seal + 015 re-admit, and stay bounded under 022 shed. **Cost owned:** one OS thread + full stack per in-flight call, P-capped; and its **latency is far off the sync budget — measured p50 62 µs, not the claimed 3 µs (F3 DISPROVEN)** — so it must be documented as a µs–ms offload path, never confused with the ns-scale sync path.
- **Stackful form — `quark::fiber<>`, gated, per-blocking-invocation (not per-actor).** Adopt the **BorrowedFiber mechanism** (fiber borrowed *after* the `Idle→Running` CAS, ADR-007 FiberParkCell win-arbitration for park/resume, origin-worker-pinned resume, registered in the 015 in-flight set) for the one axis stackless physically cannot serve: an **un-colorable / un-recompilable foreign-C call chain on interceptable I/O**, plus the **C4 multiplexing** of colorable yield points behind a foreign frame. **Measured advantages:** suspends a 5-frame uncolored chain in place with all locals byte-intact while the lane serves 2000 other parked actors (stackless equivalent = compile error); register-only switch ~19 ns hot; one cooperative driver held 32 foreign-frame handlers in flight vs a thread-backed ceiling of P. **Costs owned:** 16 KiB + guard (20 KiB mapped) per suspended actor (~120× the incumbent), cold-switch p99 ~524 ns/pair (why it stays off the hot path), VMA-bounded concurrency (~500k this rig / ~32k default kernel), and mandatory `__sanitizer_*_switch_fiber` / `__tsan_*_fiber` annotations behind the PAL (019) — the stackful asm and its sanitizer shims are the *only* non-std-core surface and are confined to this adapter.

The adapter is gated behind a demonstrated need: ship the thread-backed form first; enable the stackful form only for a workload that actually exhibits the un-colorable-chain or C4 pattern.

## Residual risks

1. **Thread-backed adapter latency is ~20× worse than claimed** (F3 disproven: p50 62 µs vs 3 µs on a non-quiesced rig). Not a veto (it is off the hot path and honestly µs-scale), but the budget must be restated as a *loaded-scheduler distribution*, and capacity planning must use it, not the optimistic figure.
2. **Suspended-fiber footprint does not scale** (16–20 KiB + guard VMA per in-flight fiber call). High-fan-out un-colorable blocking must be capped by P and 022 shed; the VMA ceiling (~32k on a default kernel) is a hard operational limit that must be surfaced in config/docs.
3. **ARM64 / weak-memory re-gate deferred.** The incumbent's 192 B / 0-RMW / Dekker-fence proofs are x86-TSO; the producer-fence elision is x86-only (a real `dmb ish` is retained on ARM behind `store_load_barrier()`), and the fiber park/resume release/acquire handshake needs a weak-memory re-gate (OpenQuestions).
4. **Sanitizer blindness on the fiber path.** TSan/ASan go blind or false-positive across a manual stack switch without the fiber annotations (proven in all three fiber designs). Every fiber correctness proof is inadmissible until the annotation positive-control fires; this is now a mandatory gate-0 for the adapter.
5. **Consumer-local `lock cmpxchg` on the drain.** The steady sync drain is 0 *cross-core* RMW (the 023 gate) but not literally lock-free — one uncontended `gen_state` claim-CAS per message. Spec wording must say "0 cross-core RMW," not "0 lock op."
6. **`ask`-chain suspended footprint is depth-linear**, not the withdrawn absolute "one frame." Deep async nesting (D ≳ 16–32) can reach a fiber's page; the common shallow case (D≤8) still wins decisively.

## Spec-update recommendations

- **001-Actor-Execution-Model.md** — Add a normative "Execution vehicle" subsection under *Hybrid handler execution* stating the core vehicle is passive + stackless run-to-completion (no per-actor stack; suspension parks the activation, not the thread), citing ADR-015. Add a **`BlockingHandler` / `FiberHandler` execution mode** as a fourth compile-time return-type category (`quark::blocking<>` thread-backed default; `quark::fiber<>` stackful, gated) that suspends the activation exactly like `quark::task<>` — offloading the un-colorable leaf off the mailbox lane and re-admitting through the 015 gate — **never on the hot path, never per-actor**. State that idle/suspended actors pin no fiber and that the stackful form owns a 16 KiB stack per in-flight call.
- **002-Scheduler.md** — In *Wakeup* / *Streaming activations*, add that adapter completion is a **structurally new third-party waker** (carrier → actor, not touching `tail_`) that re-admits via the exec-state `Parked→Scheduled` CAS carrying the same `seq_cst` Dekker rendezvous; note it is a distinct StoreLoad pair from the producer-vs-consumer one and requires its own isolating CI control (3-party litmus with a carrier-only fence-drop). Add the explicit **`Parked` exec-state** to the machine (Idle/Scheduled/Running/**Parked**), with `Parked` failing every admission CAS. Record the x86-only producer-fence elision behind PAL `store_load_barrier()`.
- **015-Reentrancy-and-Quiescence.md** — Under *How dependent specs build on this*, add a bullet: a `BlockingHandler`/`FiberHandler` in-flight call is a sealed admission (non-`Idle` `Parked`) registered in the in-flight set, so quiescence and the watchdog see it (a dropped wakeup surfaces as a stuck in-flight handler, not a silent "Running-but-nobody-home" wedge). Specify **origin-worker-pinned resume** for the stackful form (cross-worker resume of a foreign frame holding thread-affine state — pthread mutex owner, errno/TLS — is UB). Reuse the ADR-007 ReplyCell win-arbitration pattern for the FiberParkCell.
- **024-Streaming-and-Inbound-Streams.md** — Note that a stream handler may itself be a `BlockingHandler`; if so it follows the same "transferred, not parked" 015 re-admission as a suspended stream handler (ADR-014), and the un-colorable-chain fiber form is available for foreign-C stream decoders that cannot be colored as coroutines. Cross-reference the C4 multiplexing case for foreign-frame stream callbacks issuing nested asks.
- **023-Performance-Targets-and-Budgets.md** — Add a **Blocking/fiber adapter** budget block, explicitly off the sync latency budget: thread-backed suspend→resume p50/p99 as a *loaded* distribution (measured p50 ~62 µs; do not stamp the 3 µs figure); fiber register-only switch p50 ≤ 30 ns hot but flag cold-switch p99 ~524 ns/pair; suspended-fiber footprint 16–20 KiB/in-flight-call with a VMA-ceiling note; and a hard rule that the stackful fiber path is **excluded from the 100/250 ns sync ceilings**. Reaffirm the sync gate as "0 **cross-core** RMW on the drain" (not "0 lock"). Add the depth-parameterized suspended-footprint curve (D≤8 wins) to the memory/density block.

## Provenance

Proven claims across all vehicles: **32 CORRECT**, **1 DISPROVEN** (Hybrid adapter F3, thread-backed latency ~20× over budget). One claim (Fibra original CORRECT1) was withdrawn/conceded before proving and survives only in corrected form (CORRECT1R, CORRECT). INCONCLUSIVE sub-parts (Hybrid F3a mixed-build p99 delta; FAST3's own syscall-dominated timer) carry no weight and were re-sourced from adjacent microbenchmarks.
