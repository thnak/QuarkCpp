# ADR-024: EventSourced Log Compaction Cadence and Checkpoint Blocking Mode

## Status
Accepted

## Question

012's EventSourced persistence model appends events and periodically snapshots to bound
replay length, but leaves two things unspecified:

1. **Compaction trigger** — fixed event count, wall-clock interval, adaptive byte
   threshold, or something else — and how tightly it bounds worst-case replay
   length/cost.
2. **Checkpoint blocking mode** — does the snapshot write block the actor's own
   persistence lane, or run asynchronously — and how that interacts with the
   existing per-actor `PersistMode` (Sync/Batched) policy and the ADR-009 C7
   staging-fence commit-point rule (events stage per-message, become durable only
   at handler-completion commit; a throwing handler commits nothing).

Three designs were debated, red-teamed, revised, and then proven (or disproven)
with compiled-and-executed C++23 (g++ 14 and clang++ 20, `-O2`/ASan+UBSan/TSan).

## Designs (one-line summaries)

| # | Design | Trigger | Checkpoint mode |
|---|---|---|---|
| 1 | **Fixed-N-Events Synchronous Checkpoint** | Fixed compile-time-configurable event count `N` (lane-private `uint32` counter, no atomics) | Always synchronous (`co_await`), unconditional on `PersistMode` — suspends only the actor's own coroutine/lane, never the OS worker thread |
| 2 | **012-B: Wall-clock `CompactEvery<T>` + background `CompactionJob` queue** | Wall-clock timer `tell`s an internal `CompactionTick`; also fired opportunistically on idle-deactivation | Always asynchronous/background — a dedicated persistence-I/O thread pool does the write; completion closes out via an ordinary FIFO `CompactionApplied` message back on the lane |
| 3 | **Adaptive byte-threshold compaction, PersistMode-inherited mode** | Adaptive byte threshold, self-tuned toward a target replay-time via an EMA of measured bytes/ns | Inherits `PersistMode`: inline under Sync, off-lane via a dedicated I/O executor under Batched, with a hard byte-cap that defers new-message admission if the checkpoint doesn't clear in time |

## Evidence table

Kind: **F** = fast/cost claim, **S** = safety claim, **C** = correctness claim.
"Survived" = held up after cross-examination and revision. "Proven" = executed
C++ result from the prover stage (CORRECT / WRONG). Only claims that both
survived red-teaming *and* were proven CORRECT with executed evidence count for
ranking; INCONCLUSIVE carries no weight; WRONG counts against and, if
safe/correct, gates.

### Design 1 — Fixed-N-Events Synchronous Checkpoint

| Claim | Kind | Survived red-team? | Proven | Number |
|---|---|---|---|---|
| F2 (steady-state cost) | fast | yes | CORRECT | 1 cmp + 1 conditional jump added to non-triggering commit path; 0 heap allocs, 0 atomic/lock instrs (both compilers); mean delta ~57ns/msg, within noise |
| S2 (fencing reused unmodified) | safe | yes | CORRECT | 500/500 stale-token snapshot writes rejected under concurrent legitimate writes; 0 TSan/ASan reports |
| S3 (zero cost for non-EventSourced) | safe | yes | CORRECT | `sizeof` unchanged (8B/16B before/after); dispatch thunks byte-identical machine code |
| C2 (poison handler contributes 0 events) | correct | yes | CORRECT | events_since_snapshot/log_seq unchanged after throw; 0 writes submitted |
| C3 (Sync reply strictly after append+drain+snapshot durable) | correct | yes | CORRECT | 5000 Sequential + 150 Reentrant-with-sibling trials, 0 ordering violations |
| F1→**F1b** (revised: delta is mode-independent, absolute latency is not) | fast | yes (revised) | CORRECT | Batched triggering-msg median tracks snapshot-alone (~34–80µs); Sync triggering-msg tracks append+snapshot summed (~40–94µs); non-triggering ~257–274ns |
| C1→**C1b** (revised: N+K-1 bound conditional on externally-capped K) | correct | yes (revised) | CORRECT | K=8 harness-capped: 500/500 SIGKILL trials, replay never exceeded N+K-1=57. Uncapped K: replay scales linearly with attacker-controlled K (10,000,049 events for K=10M) — confirms the bound is conditional, as restated |
| S1→**S1b** (revised: `quiesce(Drain)` inserted before serialize_state, closing the Reentrant leak) | safe | yes (revised — was FATAL as originally written) | CORRECT | 1000/1000 fuzzed interleavings: compacting message provably blocks on the poison sibling; captured snapshot never includes the poison mutation |

**8/8 claims proven CORRECT. Zero disqualifying WRONG verdicts.** (Original submission had 3 red-team concessions — S1 was fatal-as-written — all closed by revision and reproven.)

### Design 2 — 012-B Wall-clock cadence + background compaction

| Claim | Kind | Survived red-team? | Proven | Number |
|---|---|---|---|---|
| F1 (steady-state cost: one atomic release-store) | fast | yes | CORRECT | delta 0.008–0.048 ns/op (g++), noise-level on clang++ |
| F2 (on-lane tick cost independent of store latency) | fast | yes | CORRECT | on-lane handler duration flat at 4–48µs across injected store delays of 0/10/50/100ms |
| C2 (staging fence honored) | correct | yes | CORRECT | rolled-back debit never reflected in the next durable snapshot |
| S2 (crash after rename, before close-out, is safe) | safe | yes | CORRECT | recovery-by-scan reconstructs correctly; superseded WAL segment still present |
| S3→(revised, ActorId-keyed) at-most-one-inflight | safe | yes (revised — was FATAL: per-Activation not per-ActorId) | CORRECT | reactivation-while-inflight rejected; 100k CAS attempts, max-concurrency never > 1 |
| C1→**C4** (revised: fence-ordered-by-construction publish, not completion-order) | correct | yes (revised — was FATAL: check-then-act fence gap) | CORRECT | stale/slow writer's CAS-publish rejected by (fence, through_seq) comparator in both cross-activation and same-fence-out-of-order sub-cases |
| **C5** (stale `CompactionApplied` dropped via fence+seq check) | correct | yes (new, closes a serious gap) | CORRECT | stale replies dropped; bookkeeping non-decreasing across both attacks |
| C3→**C6** (revised: idle-deactivation also triggers compaction, bounding starvation) | correct | yes (revised — original wall-clock-only cadence could starve indefinitely) | CORRECT | replay bound reduced from "entire actor lifetime" to "one burst" |
| P1 (mutex-vs-atomic&lt;shared_ptr&gt; portability fix) | fast | no | **WRONG** | measured no cross-actor lock-striping collapse on this toolchain once benchmark false-sharing was corrected (26.8→46–56 Mops/s, no collapse); GCC 14's `atomic<shared_ptr>` uses a per-instance spinlock, not a global table — the diagnosed defect doesn't reproduce here |

**8/9 claims proven CORRECT; 1 WRONG, but P1 is a "fast" (non-gating) claim about an optimization that turned out to be unnecessary, not a safety/correctness violation.** No disqualifying gate hit. (Original submission had 3 red-team concessions, one — S1/C1 — fatal as written; all closed by revision.)

### Design 3 — Adaptive byte-threshold, PersistMode-inherited

| Claim | Kind | Survived red-team? | Proven | Number |
|---|---|---|---|---|
| C1→**C1R** (Batched checkpoint I/O off the lane, gated by new per-shard `store_owner` CAS) | correct | yes (revised — was FATAL: unsynchronized concurrent `Store` access) | CORRECT | follow-ups dispatch at +0ms vs 50ms SlowStore delay; 0 TSan/ASan reports after adding the gate (negative control confirms it *would* race ungated) |
| C2 (Sync blocks the lane) | correct | yes | CORRECT | follow-up delayed ≥50ms matching injected store delay |
| S1 (fencing rejects stale background write) | safe | yes | CORRECT | stale F1 write rejected; only F2 payload persists |
| S2 (stale epoch close-out is a no-op) | safe | yes | CORRECT | bookkeeping unchanged by stale-epoch message |
| S3 (≤1 outstanding checkpoint/actor) | safe | yes | CORRECT | max concurrency observed = 1 across 4 actors, 20k commits |
| **S4** (new `store_owner` gate eliminates the concurrent-`Store`-mutation race) | safe | yes | CORRECT | 2M gated touches, 0 races; ungated negative control races immediately |
| F1 (bookkeeping cost) | fast | yes | CORRECT | p50 delta 0–1ns; zero footprint for non-EventSourced actors |
| F2 (EMA clamp/no-alloc) | fast | yes | CORRECT | threshold always in `[min_bytes,max_bytes]` over 4504 adversarial samples; 0 allocations |
| C3→**C3R** (revised: hard byte-cap via admission deferral bounds replay to `max_bytes`+1 event) | correct | yes (revised) | **WRONG** | **Deterministic deadlock**: deferring admission of ordinary messages while `inflight` blocks the actor's own single FIFO mailbox from ever reaching the `CheckpointDone` completion message queued behind them — the actor wedges permanently (0/10,000 drain-step progress in the isolated repro; integration repro never resumes within a 2000ms bound). Reproduces identically under plain/ASan/TSan, both compilers |

**8/9 claims proven CORRECT, but the 9th (C3R) is a "correct"-kind claim proven WRONG — a permanent deadlock, not merely an imprecision.** No cheap fix was stated or demonstrated in the debate (the fix requires restructuring completion delivery off the ordinary FIFO mailbox — a materially different, unproven mechanism). Per the ranking rule, **this disqualifies Design 3 from winning.**

## Decision

**Winner: Design 2 — 012-B Wall-clock `CompactEvery<T>` cadence with background
`CompactionJob`/`CompactionApplied` close-out.**

Rationale, applying the stated ranking in order:

1. **Safety gate.** Design 3 is eliminated outright: its C3R claim — the very
   mechanism invented during cross-examination to give it a tight,
   storage-speed-independent replay bound — was proven to deadlock the actor
   permanently (a `correct`-kind claim marked WRONG, with no stated cheap fix).
   Designs 1 and 2 both cleared the gate: every `safe`/`correct` claim that
   survived red-teaming was proven CORRECT. Design 2's one WRONG verdict (P1) is
   a `fast`-kind claim about an optimization's *justification* (whether
   `atomic<shared_ptr>` suffers cross-actor lock-striping on this toolchain — it
   doesn't), not a safety or correctness property; the mutex it proposed as a
   fix is still valid, merely unneeded here. It does not gate.

2. **Proven-claim count.** Design 1: 8/8 CORRECT. Design 2: 8/9 CORRECT (1 WRONG,
   non-gating). Both are strong, near-clean records after revision; neither
   has an edge here.

3. **Measured hot-path numbers, among safe survivors.** This is where Design 2
   wins decisively. Both designs' steady-state per-message overhead is
   negligible (Design 1: ~1 cmp + 1 branch, ~57ns/msg noise-level; Design 2: a
   single atomic release-store, 0.008–0.048ns/op) — effectively a wash. The
   decisive, *measured* difference is at the compaction trigger itself, which
   is exactly what the target question (2) asks to weigh:
   - Design 1's checkpoint is **unconditionally synchronous regardless of
     `PersistMode`** — F1b measured the triggering message's latency spiking
     to ~34–94µs (100–300× the non-triggering ~257ns baseline) *even under
     Batched mode*, whose entire raison d'être (012: "fast, acking before
     durable") this periodically defeats.
   - Design 2's checkpoint is **provably asynchronous and orthogonal to
     `PersistMode`** — F2 measured the on-lane `CompactionTick` handler
     duration staying flat (4–48µs) regardless of an injected background
     store delay ranging from 0 to 100ms. The disk write, and now (post-fix)
     the compare-and-swap-gated cross-thread `Store` access, never appears on
     the message-processing critical path in either `PersistMode`.

   Design 2 answers "does the checkpoint block the lane, and how does that
   interact with `PersistMode`" the way the spec's own Batched-mode contract
   implies it should: compaction is a side channel that never trades away a
   Batched actor's low-latency guarantee. Design 1's answer — "always
   synchronous, PersistMode be damned" — is honestly disclosed and cleanly
   proven, but is a materially worse fit to the existing PersistMode contract.

4. **Core invariants.** Neither surviving design bends a locked invariant.
   Design 2's revised fencing (C4/C5, unique `(fence, through_seq)`-ordered
   filenames + CAS-comparator publish + fenced `CompactionApplied`) gives the
   most direct, multi-angle proof that the compaction mechanism cannot bypass
   or race the fencing-token invariant across activation/re-placement
   boundaries — arguably stronger evidence on this specific invariant than
   Design 1 provides (Design 1's S2 proves the reuse of the *existing* reject
   path is intact, but Design 1 never had to prove correctness across an
   activation-lifecycle boundary the way Design 2's background worker does).

**Residual trade-off, stated plainly:** Design 2 does not give as tight a
worst-case replay-length bound as Design 1. Design 1's fixed-N gives an exact,
proven count bound (`N + K − 1`, given an externally-capped `K`). Design 2's
bound is wall-clock/rate-dependent (`O(compaction_interval × peak_rate)`), only
partially tightened by the idle-deactivation trigger (C6) — a sustained
high-rate burst that never idles can still grow the tail large within one
interval. This is an honest, disclosed limitation, not a disproven claim, and
is the basis for the spec recommendation below.

## Spec recommendations for `012-Persistence.md`

1. **Adopt Design 2 as the compaction mechanism.** Specify: `Persistent<Model,
   Mode, Cadence = CompactEvery<30000>>` — `Cadence` consulted only when
   `Model = EventSourced`; Snapshot-model and non-persistent actors never
   instantiate it (zero cost, per S3/F1).
2. **Specify the checkpoint write as always asynchronous/background,
   orthogonal to `PersistMode`.** State explicitly that `PersistMode`
   continues to govern only per-event-commit durability timing (Sync = commit
   before reply; Batched = fire-and-forget, coalesced) and that the periodic
   snapshot/compaction write is a separate, always-background side channel
   that never blocks the actor's lane in either mode — cite the F2 measurement
   (flat 4–48µs handler duration independent of injected store delay) as the
   normative behavior to preserve under implementation.
3. **Add the idle-deactivation compaction trigger (C6) as a MUST, not an
   optimization.** Without it, a wall-clock-only cadence can be starved
   indefinitely by an idle-deactivation window shorter than `CompactEvery<T>`,
   silently degrading the replay bound to "the actor's entire lifetime."
4. **Specify the fence-ordered publish discipline from C4/C5 as normative,
   not incidental:** (a) durable snapshot files are named/keyed by
   `(fence, through_seq)` so a slower, stale writer can never overwrite a
   newer one by completion order; (b) the in-memory hot-cache publish is a
   compare-and-swap keyed on `(fence, through_seq)`, never an unconditional
   store; (c) the `CompactionApplied` close-out message itself carries the
   issuing activation's fence/epoch and is dropped as a no-op if it doesn't
   match the current activation's fence or doesn't strictly advance the
   watermark. These three rules are what closed the two fatal gaps (S3, C1)
   found during cross-examination, and must be treated as part of the
   contract for any `Store` adapter that opts into background compaction, not
   left as an adapter-specific implementation detail.
5. **Recommend a hybrid early-trigger addendum (open follow-up, not yet
   proven).** To close Design 2's disclosed residual — an unbounded-within-
   one-interval tail under sustained high-rate bursts — add an optional
   secondary byte- or count-threshold that can fire an *early* `CompactionTick`
   between wall-clock ticks (reusing Design 1's fixed-N counter, proven exact
   at C1b, as the early-trigger condition, but still routed through Design
   2's async background path, not Design 1's synchronous one). This was not
   itself red-teamed or proven in this debate and is the natural
   next-iteration target if a tighter replay bound becomes a hard requirement
   (e.g., driven by 023 Performance-Targets recovery-time budgets).
6. **Document the `Store` adapter concurrency obligation.** Any `Store`
   adapter used with background compaction must serialize cross-thread access
   to its shared per-actor state (the design's own fatal gap, closed here via
   a per-shard/per-actor CAS gate) — extend `store_conformance.hpp`-style
   tests with a genuine two-thread stress case per adapter (`FileStore`,
   `InMemoryStore`, and any future `SqliteStore`/`RocksStore`) rather than
   assuming internal locking from marketing claims ("ACID").
7. **Retain Design 1 in the spec as a documented alternative /
   fallback policy**, not the default: for actor types where a hard, exact
   replay-count bound matters more than avoiding a periodic lane-latency
   spike (e.g., actors already running exclusively under `PersistMode::Sync`,
   where the added synchronous checkpoint cost is proportionally smaller),
   `CompactEveryNEvents<N>` remains a fully proven, safe cadence policy
   (8/8 CORRECT) that a per-actor-type override could select.

## Residual risks

- Design 2's replay-length bound is rate-dependent, not a fixed count; a
  sustained, very-high-rate, long-lived actor that never idles can still
  accumulate a large tail within a single `CompactEvery<T>` interval. Mitigate
  via recommendation 5 (hybrid early trigger) if a hard bound becomes a
  requirement.
- Design 2's fencing/publish-ordering fixes (C4/C5) and the ActorId-keyed
  in-flight guard (S3) were validated against `InMemoryStore`/`FileStore`-style
  adapters only; any future `Store` adapter must independently prove the same
  two-thread stress properties (recommendation 6) before being trusted with
  background compaction.
- `std::atomic<std::shared_ptr<T>>` lock-freedom is implementation-defined
  across toolchains; this debate's P1 finding (no cross-actor lock-striping
  collapse on GCC 14/libstdc++) does not necessarily hold on libc++ or MSVC
  STL — verify per-toolchain before relying on it, or default to the
  per-Entry-mutex fallback the design's authors proposed (harmless, just
  possibly unnecessary here).
- Design 1's `K` (max events staged by a single handler invocation) must be
  independently capped by a separate, unrelated policy (a per-actor-type
  max-events-per-handler limit) for its `N + K − 1` bound to hold in practice;
  this is a real gap if Design 1 is ever adopted per recommendation 7 without
  that companion cap.
- The single-FIFO-mailbox deadlock class that disqualified Design 3 (C3R) is
  a cautionary structural finding worth generalizing: any future mechanism
  that conditionally defers ordinary-message admission on the SAME mailbox a
  completion/control message also travels through must be checked for this
  exact wedge before being proposed again.

## Tie-breaking experiment (if evidence had been insufficient)

Not needed — the safety gate and the F2/F1b hot-path measurements were
sufficiently decisive. Had Design 2's P1 finding instead been a `safe`/`correct`
claim proven WRONG, or had Design 1 and 2's triggering-path latency numbers
come out comparable, the tie-breaker would have been: benchmark both designs'
triggering-message p99.9 latency for a **Batched** EventSourced actor with a
realistically large `State` (e.g., 100KB, matching a plausible MES
ledger/inventory working set) under sustained load with a real (non-tmpfs)
disk-backed `Store`, since that is the scenario where Design 1's mandatory
synchronous spike and Design 2's on-lane state-copy cost (proportional to
`State` size in both designs) diverge most from each other in practice.
