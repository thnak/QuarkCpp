# ADR-014 — Streaming async-suspend seam vs. the REAL scheduler: 024 promotion gate

Status: **Accepted (verification record)**
Date: 2026-07-15
Deciders: gate-verification judge (closing the single named 024 Draft→Accepted promotion gate)
Supersedes/relates: **executes — does not redesign** — the SETTLED `StreamChannel`
design of [ADR-005](ADR-005-inbound-stream-ingestion-hot-path.md) (credit-ring +
split-cursor `disp`/`tail` async-suspend seam) and spec
[024-Streaming-and-Inbound-Streams.md](../024-Streaming-and-Inbound-Streams.md);
reuses **verbatim** the Accepted mailbox exec-state wakeup + `seq_cst` Dekker
close-out of [ADR-002](ADR-002-mailbox-mpsc-hot-path-r2.md)/[003](ADR-003-mailbox-mpsc-hot-path-r3.md)/[004](ADR-004-mailbox-mpsc-hot-path-r4.md)
and the admission-gate / single-executor invariant of
[015-Reentrancy-and-Quiescence.md](../015-Reentrancy-and-Quiescence.md); closes the
one Open Question 024 named as **THE promotion gate** (024 lines 223–230). Does not
touch the mailbox core (001/002/003/004) or 015 — both Accepted and UNCHANGED.

---

## Question

ADR-005 proved the `StreamChannel` steady hot path and the split-cursor suspend seam
8/8, but the last seam — replacing the in-thread **model resolver** with the REAL
002 multi-threaded worker + 015 admission gate, where completion arrives on a foreign
thread and the continuation re-enters `StreamChannel::drain` by **transferring the
activation** (exec-state CAS re-acquired afresh by 002) rather than re-enqueuing the
descriptor — was *modelled, not wired*. 024 stayed Draft with this real-scheduler
wiring named as the gate.

The bar (decision inequality, both g++14.2 & clang20.1, ≥10⁷ frames): exactly-once
holds (`lost==dup==torn==fifo_violations==0`); no `StreamActivationDescriptor`
double-enqueue/orphan (max membership == 1); credit returned **only** for
completion-observed frames; steady-drain path **measured** 0-alloc + 0-RMW; TSan clean
on the positive path; **and BOTH mandatory controls FIRED** (CONTROL-4 single-cursor
tore/lost; CONTROL-5 re-enqueue double-dispatched/orphaned), plus the inherited
load-bearing CONTROL-6 fence-removed producing `lost>0`. A control that did not fire,
or a "real scheduler" that was a model resolver in disguise ⇒ **INCONCLUSIVE, not
CORRECT**.

---

## Gate ruling — `Streaming-async-suspend-real-scheduler-024` → **CORRECT**

### The wiring is the real seam, not a model resolver

The headline false-pass route (MODEL-RESOLVER-IN-DISGUISE) is closed by direct
instrumentation. Producer, suspend-trigger, 015 completion, and post-completion drain
re-entry run on **three distinct threads** with a real 002 exec-state handoff between
them: `three_distinct = 26 367–31 721` handoff events per run (across g++/clang ×
ASan/TSan), and `parked_off_workers = 32 282 / 32 282` — on **every** park the
activation is in a distinct `Suspended` exec-state, owned-but-off-all-workers, so the
`await_ready==false` transfer path is genuinely taken (not compiled-and-skipped, not a
synchronous fake). Only the 015 completion transfers `Suspended→Scheduled` **without
re-enqueuing the descriptor**; `tail` (hence derived credit) advances only when a
worker re-acquires the exec-state CAS afresh and observes the completion, so `tail`
freezes at the parked frame and the producer's derived credit provably cannot cover
the parked slot. Suspend is biased to wrap boundaries (seed `0xc0ffee24`), ring
cap 256, ≥10⁷ frames — the overwrite race is **attempted**, not merely possible;
CONTROL-4's `credit_for_parked = 32 282` confirms the harness reaches the hazardous
window.

### Exactly-once evidence table — positive path, ≥10⁷ frames (worst across g++14.2 / clang20.1 × ASan / TSan)

| Property | Oracle | Result | Gate |
|---|---|---|---|
| Lost frames | per-frame monotone id | **lost = 0** | == 0 ✓ |
| Duplicated frames | per-frame monotone id | **dup = 0** | == 0 ✓ |
| Torn frames | per-frame payload checksum | **torn = 0** | == 0 ✓ |
| Per-stream FIFO | id monotonicity | **order_violations = 0** | == 0 ✓ |
| Id integrity | id_mismatch | **0** | == 0 ✓ |
| Descriptor double-enqueue | armed-flag membership counter | **max_membership = 1, obs = 0** | ==1 ✓ |
| Single executor | running-executor counter | **max_running_executors = 1** | ==1 ✓ |
| Credit oracle | `credit_returned == completions_observed` | **tail_final = 10 000 000 = N**; `credit_for_parked_not_completed = 0` | ✓ |
| Three-thread handoff | per-event thread-id log | **three_distinct = 26 367–31 721**; parked_off_workers = 32 282/32 282 | ✓ |

Every conjunct of the decision inequality holds on **both** compilers. No corruption
count > 0; no descriptor double-enqueue; no credit returned for a parked-not-completed
frame; no TSan race on the positive path.

### 0-alloc / 0-RMW — MEASURED, not asserted

- **0 steady-state heap alloc:** global `operator new` hooked after warmup, counted
  across the steady drain loop **and** the completion continuation (coroutine frame is
  pmr/pre-allocated) — **0 / 10⁷** on both compilers. The unmeasured-assertion route
  (a coroutine-frame malloc on the completion path) is closed.
- **0 cross-core RMW / frame:** objdump of `drain(Activation*)` on both compilers —
  **0** `lock` / `xchg` / `cmpxchg` / `xadd` / `mfence`; the consumer inner loop is
  plain acquire-load + release-store only. ADR-005's 0.30 RMW/frame naive-consumer
  `exchange` (the explicit FAIL bar) does not appear.

### TSan status

- **Positive path, 10⁷ frames: TSan 0 races, full clean run (exit 0), both compilers.**
  ASan/UBSan also clean (exit 0). TSan is load-bearing here because suspend /
  completion / drain-re-entry cross threads; it was run on the positive path **and**
  on both firing controls.
- **Disclosed, accepted limitation:** `std::atomic_thread_fence` is not instrumented
  by TSan (documented, both compilers). The sanitized stress therefore expresses the
  `seq_cst` Dekker close-out in its **TSan-modelled `seq_cst`-atomic equivalent form**
  (the same ISA-independent rendezvous), and the **standalone-fence** form's
  load-bearing property is proven separately in the unsanitized CONTROL-6 Dekker
  litmus (below). This is the ADR-004 posture verbatim, not a gap: the happens-before
  edges TSan observes are the real handoff edges, and the fence's necessity is
  independently demonstrated.

### Control outcomes — ALL THREE FIRED (identical stress, seed `0xc0ffee24`)

| Control | Injected defect | Required outcome | Observed (g++ / clang) | Fired? |
|---|---|---|---|---|
| **CONTROL-4 single-cursor** | collapse `disp`+`tail`; return credit at **dispatch**, before completion | torn>0 OR lost>0 OR wedge | lost **33 001 / 35 918**, torn **1 576 / 1 785**, `credit_for_parked = 32 282` both; **+ 2/2 TSan races** (producer WRITE vs `process_frame←drain` READ on the parked slot) | **YES** |
| **CONTROL-5 re-enqueue** | on 015 completion **re-enqueue** the descriptor instead of transferring | dup>0 OR TSan double-enqueue race | dup **115 991 / 602 016**, double_enqueue_obs **5 127 / 30 517**, max_membership **2 / 4**, max_running_executors **2**; **+ 2/2 TSan races** (`resume_and_drain←worker` vs `drain←completion`); **+ wedge/timeout on g++ ASan @ 10⁷** | **YES** |
| **CONTROL-6 fence-removed** (inherited load-bearing) | downgrade both sides of the close-out to plain release/acquire | lost>0 (ADR-004 CI guard) | lost **194 537 / 192 402** vs **0** with the fence, 5M isolating Dekker trials | **YES** |

Every control is non-vacuous: CONTROL-4 demonstrates the overwrite hazard the split
cursor exists to prevent (credit-for-parked = 32 282, then torn/lost), CONTROL-5
demonstrates the two-executor / orphaned-window defect that transfer-not-re-enqueue
prevents (max_membership up to 4, two running executors, TSan race), and CONTROL-6
re-proves the `seq_cst` wakeup rendezvous is load-bearing **in this wiring** and not
accidentally satisfied. Controls ran under the **same** frame count and
randomized-suspend schedule/seed family as the positive path.

### Anti-cheat closure

All twelve false-pass routes are closed: three-lane real handoff (not a same-thread
loop); suspend biased to wrap with a logged PRNG seed; 10⁷ frames × cap-256 (producer
laps the parked slot; inline ≤56B **and** by-reference frames exercised); per-frame
id+checksum+FIFO oracle (not aggregate count); descriptor membership instrumented
(transfer falsifiable and distinct from CONTROL-5); credit oracle asserted separately;
TSan on positive + both controls; suspend proven to leave all workers; footprint
measured via new-hook + objdump; both compilers; controls under identical stress.

**Build:** `-std=c++23 -O2 -Wall -Wextra`, **0 warnings**, base and sanitized, both
compilers. Report artifact: `/tmp/quark-024.3wa9Sd/report.html`.

**Verdict: CORRECT.** All conjuncts of the decision inequality satisfied on both
compilers at 10⁷ frames, and all three mandatory controls fired non-vacuously.

---

## 024 promotion decision → **PROMOTE — Accepted (x86-64)**

Rule: *024's suspend path promotes past Draft iff the real-scheduler seam is CORRECT
with both mandatory controls firing.* It is. The single named gate — exactly-once +
no-wedge across suspension against a REAL 002 multi-threaded scheduler + 015 admission
gate, with a genuine cross-thread transfer (not a model resolver) — is proven with
zero-tolerance oracles, measured 0-alloc/0-RMW, clean TSan, and three firing controls.

**024-Streaming moves Draft → Accepted (x86-64).** The promotion is **scoped to
x86-64**: exactly-once, no-wedge, no descriptor double-enqueue/orphan, and steady-drain
footprint (0-alloc / 0-RMW) are proven. **The Hard absolute-latency budget remains
deferred** to the 023 reference silicon (Zen4/SPR), and the **ARM64 weak-memory
re-gate of the suspend seam remains deferred** (see residual risks). This mirrors the
ADR-005 posture: ratios, 0-RMW, 0-alloc and exactly-once are host-independent and
PASS; absolute ns await reference hardware.

With this ruling, the 024 spec header ("Two things gate promotion… the async-suspend
resume seam… not yet wired to the real 002 scheduler + 015 gate") is resolved for the
first item; the 024 Open-Questions "This is the promotion gate" line is closed
CORRECT. The second item (absolute latency on reference silicon) is carried below as a
non-blocking residual, consistent with an x86-64-scoped Accepted.

---

## Residual risks

1. **[deferred — ARM64 weak-memory re-gate of the suspend seam]** The exec-state
   wakeup + `seq_cst` Dekker close-out that the transfer path rides is proven
   **load-bearing on x86-TSO only** (CONTROL-6: lost 194 537/192 402 vs 0). The
   Dekker close-out and the `armed.exchange`-as-StoreLoad-arm rely on x86 `lock xchg`
   being a full StoreLoad barrier; on AArch64 they need an explicit `seq_cst` fence /
   `seq_cst` exchange. This is TSO-proven only and must be **herd7/GenMC
   litmus-checked, then re-run against the real scheduler on ARM hardware**, before any
   non-x86 promotion of the suspend seam. Deferred with the rest of ARM (no hardware);
   carries no weight against this x86-64 ruling.

2. **[deferred — Hard absolute-latency budget]** All exactly-once / footprint gates
   are host-independent and PASS, but absolute latency percentiles (024/023 Hard
   ceiling: p50 ≤ 100 ns goal / ≤ 250 ns hard; p999 ≤ 5 µs / ≤ 50 µs) were **not**
   the subject of this gate and were measured by ADR-005 only on a conservative
   ~2.1 GHz turbo-off rig, not the 023 Zen4/SPR reference core. The 024 Hard-latency
   budget is **not stamped** by this promotion; it must re-run on reference silicon
   (tracked with 023). This is why the promotion is scoped, not full Accepted.

3. **[disclosed sanitizer modelling]** TSan does not instrument
   `std::atomic_thread_fence`; the sanitized positive path used the TSan-modelled
   `seq_cst`-atomic equivalent form of the close-out and the standalone-fence form's
   necessity was proven in the unsanitized CONTROL-6 litmus. Accepted per ADR-004
   posture, but any future refactor of the close-out from `seq_cst` atomics to bare
   fences must re-run CONTROL-6 to keep the load-bearing proof attached.

4. **[inherited from ADR-005, not re-gated here]** Idle-density footprint tax
   (~60K streams/GB at cap-256, ~17× under 023's 1M-activations/GB target), the
   single-writer SPSC precondition (needs the stream-open single-writer token / typed
   007 error on double-bind), and the `ZeroCopyRetained` suspend head-of-line-blocking
   hazard (a suspended by-reference handler pins its RX buffer) are graded/open in
   ADR-005 and 024 and are **outside** this gate's scope — this gate proved the
   inline-default and by-reference exactly-once suspend seam, not the pinned-buffer
   pool policy.

5. **[extended by [ADR-018](ADR-018-outbound-streaming-replies.md) — outbound reply drain
   needs its own real-scheduler run]** The outbound streaming-reply item-drain **is** this
   gate's item-drain flipped (callee = producer, caller = consumer), and ADR-018 proved that
   flipped leg CORRECT under the shipped `StreamChannel`/`StreamActivation` headers. But the
   two seams unique to the reply direction — the single-resolve OPEN `StreamReplyCell`'s **015
   re-admit** (`co_await` on-lane resume, still unwired in `reply_cell.hpp`) and the two-part
   **terminal-wake edge** (terminal CAS arms the caller drain **and** bumps `credit_gen`) —
   were proven only in a wrapper, not against the real 002 scheduler + ADR-007 reply router.
   Each needs its **own ADR-014-grade real-scheduler run** before 006 outbound streaming
   promotes Draft→Accepted.
