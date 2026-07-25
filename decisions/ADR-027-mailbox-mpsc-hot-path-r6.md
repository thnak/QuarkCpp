# ADR-027: Mailbox MPSC Hot Path (Round 6)

## Status

Accepted. The incumbent intrusive Vyukov MPSC mailbox (ADR-001..004, ADR-020) wins a sixth
consecutive round. Two challenger designs were pushed to their strongest forms, red-teamed,
revised, and then proven or disproven with compiled-and-executed C++ (TSan+ASan+UBSan+benchmarks
on GCC 14.2.0 and Clang 20.1.2). Both challengers were disqualified by the safety gate. Four
concrete, narrow spec/implementation hardenings are adopted from claims that survived on the
incumbent (C3, C4) even though the design itself did not need to change.

## Question

Design the actor Mailbox — the MPSC queue owning FIFO message ordering, storing fixed-size
`MessageHandle`s, drained by exactly one worker at a time under the single-executor invariant.
Prove it fast (allocation-free hot path, scales under contention), safe (TSan+ASan+UBSan clean),
and correct (strict FIFO, no lost/duplicated handles, tombstones skipped exactly once).

## Designs debated

1. **Intrusive Vyukov MPSC Mailbox** (incumbent, pushed to strongest form) — the queue node IS
   the pooled `Descriptor` (link at offset 0); one shared `tail_.exchange(acq_rel)` + release
   link-store per producer; a private, non-atomic `head_` for the sole consumer; message
   lifecycle packed into one `gen_state` atomic word gating claim/cancel as a single CAS;
   cross-worker handoff rides the actor's exec-state CAS plus a seq_cst Dekker close-out fence.

2. **Segmented Bounded-Ring MPSC Mailbox** (Michael–Scott chain of Vyukov bounded rings) —
   producers claim slots via `fetch_add` on a bounded array Segment; consumer does a straight-line
   array scan with zero atomic stores; segments chain via CAS on overflow and recycle through a
   shard-local free-list, later hardened with QSBR (quiescent-state-based reclamation) after an
   initial ABA-style cross-generation message-injection bug was found and fixed mid-debate.

3. **BSR-Mailbox** (Bounded-Segment Treiber-Push with Per-Segment Reversal) — producers push onto
   a depth-capped Treiber-CAS LIFO stack; the thread crossing the cap seals the segment onto an
   outer Vyukov-style FIFO; the consumer reverses at most one bounded segment into a private FIFO.
   This exact lineage (Treiber-push + reversal) had already been rejected five times previously
   (ADR-001..004 Design C, ADR-020/REX-BIR) for O(batch) reversal latency; this round targeted
   that specific defect with a hard segment-size cap.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|---|
| F1 enqueue = store+xchg+store+ret, no retry/call | Incumbent | Yes (verification-method caveat only) | **CORRECT** | Identical objdump shape on GCC/Clang, -O2/-O3 |
| F2 zero RMW on steady drain path | Incumbent | Yes | **CORRECT** | 999/999 pops with 0 RMW; 1 RMW only at occupancy-1 boundary |
| F3 absolute-floor throughput/latency (revised: ratio excluded) | Incumbent | Revised scope | **CORRECT** | p50=60ns, p999=189-219ns; 17.7-30.9M msg/s (P=1/4), all ≥10M floor |
| S1 TSan/ASan clean, 400k msgs | Incumbent | Yes | **CORRECT** | 0 sanitizer reports, dup=0, missing=0 (12 runs) |
| S2 seq_cst close-out fence load-bearing (revised to match real harness) | Incumbent | Revised wording | **CORRECT** | real: 10/10 clean (200k rounds); control: 10/10 stranded early |
| S3 exec-state acquire/release load-bearing on head_ (new isolated harness) | Incumbent | Yes | **CORRECT** | real: 0 TSan reports/300k handoffs; relaxed control: reproducible head_ race + livelock |
| C1 strict per-producer FIFO, exactly-once, 400k msgs | Incumbent | Yes | **CORRECT** | received=400000, dup=0, fifo_violation=0 (12 runs) |
| C2 cancel/claim race exactly-once | Incumbent | Yes | **CORRECT** | handled+tombstoned=200000, double_free=0 (12 runs) |
| C3 Busy never misreported as Empty mid-publish (new litmus test) | Incumbent | Yes | **CORRECT** | 600,451 Busy observations, 0 Empty-during-window, 220k trials incl. TSan |
| C4 `complete()` TOCTOU hardening (new claim, design change) | Incumbent | Yes | **CORRECT** | hardened: 0/1M double-finalized; plain control: 1,000,000/1,000,000 double-finalized (TSan silent — proves it's a linearizability bug, not a race) |
| S1 combined sanitizer-clean across full protocol incl. cross-worker handoff | Segmented Ring | No | **WRONG** | TSan found real data race in `release_and_recheck()` reading `head_seg_`/`dequeue_pos` post-release, racing a re-acquiring worker (2-9 reports/100k handoffs, both orderings) |
| S2 QSBR reclamation closes cross-generation injection (revised) | Segmented Ring | Yes | CORRECT | 0 violations, 1000+ adversarial-stall trials |
| C1 FIFO/no-loss/no-foreign-injection | Segmented Ring | Yes (after free-list ABA fix) | CORRECT | 10M msgs, 0 mismatches (after fixing a real Treiber-freelist ABA bug found mid-proof) |
| F3 drain-side speed vs incumbent | Segmented Ring | Yes | CORRECT | 21.37ns/msg (gcc) vs 54.73ns/msg baseline — 2.56x faster drain |
| C1-rev strict FIFO across segment boundaries (revised w/ seals_in_flight_) | BSR | No | **WRONG** | 8,632 fifo_violations/1M msgs; 100%-reproducible leapfrog repro (`5 6 7 8 1 2 3 4 9`) |
| S1-rev no double-free/UAF (revised: mailbox never releases descriptors) | BSR | No | **WRONG** | Deterministic hang — `reverse_segment()` cycle from anchor-descriptor recycling |
| F2-rev segment size capped / backlog latency flat | BSR | Partially | **WRONG** (latency sub-claim) | segment size correct (0 overgrowth) but p999 growth 3.58x-6.31x, exceeding the claimed ≤2x bound |
| F1 uncontended push within 15% of Vyukov | BSR | No | **WRONG** | BSR = 116-120% of Vyukov median ns/op on both compilers |

## Decision

**Winner: Intrusive Vyukov MPSC Mailbox (incumbent), as hardened by claim C4.**

Rationale, by the stated ranking:

1. **Safety gate.** Both challengers produced a prover-confirmed WRONG on a safety/correctness
   claim with no *proven*, cheap, already-applied fix in hand:
   - Segmented Ring's S1 failure is a genuine TSan-detected data race in the close-out probe
     (`release_and_recheck()` reads consumer-private `head_seg_`/`dequeue_pos` after releasing
     ownership, unsynchronized against a worker that has already reacquired `Running`). The
     prover only *recommends* a fix (route the recheck through an atomically-published pending
     flag) — it was never implemented or re-proven clean this round. Per the ranking rule ("any
     safe/correct claim marked WRONG... disqualifies... unless a stated cheap fix exists"), a
     recommendation is not a demonstrated cheap fix; disqualified.
   - BSR's C1-rev (FIFO violated, 8,632/1M, 100%-reproducible) and S1-rev (deterministic hang from
     a corrupted intrusive chain when a recycled anchor-descriptor's `link.next` is reused by an
     unrelated message) are both severe, structural failures discovered *after* a full red-team
     revision round explicitly aimed at fixing this exact lineage's prior defects. This is its
     sixth rejection in this repository (ADR-001..004, ADR-020, now ADR-027) on the same family of
     seams (multi-step segment publish, descriptor double-duty as bookkeeping nodes). Disqualified.
   - The incumbent has zero WRONG verdicts across all ten claims (F1-F3, S1-S3, C1-C4); the one
     claim needing a design change (C4, Descriptor::complete() TOCTOU) was hardened and then
     proven correct with a positive control (plain-store version reproducibly double-finalizes
     1,000,000/1,000,000 trials while staying TSan-silent, proving the harness has teeth and the
     bug was real but sanitizer-invisible until guarded).

2. **Proven beats claimed.** All ten of the incumbent's claims survived red-teaming and were
   proven CORRECT with executed evidence (objdump, TSan/ASan/UBSan runs, positive/negative
   controls). Nothing is left at "claimed" status after this round.

3. **Measured hot-path numbers among safe survivors.** With both challengers disqualified, no
   comparison is required to declare a winner, but for the record: the incumbent's single-thread
   round-trip latency (p50=60ns, p999=189-219ns) and throughput (17.7-38.3M msg/s across
   configurations) comfortably clear the 023 budget's goal/floor lines on both compilers, with
   every one of 20 multi-producer trials landing at least 1.77x above the 10M msg/s floor.

4. **No core invariant bent.** The incumbent's single-executor invariant, FIFO-by-default
   ordering, and zero-heap-allocation hot path are all upheld exactly as specified in
   001/002/003/015; the C4 hardening changes only a once-per-message, off-hot-path CAS
   (`complete()`), not the enqueue/dequeue fast path.

## Design change adopted (C4)

`Descriptor::complete()` moves from a convention-only `relaxed load + release store` to a
`compare_exchange_weak(Running -> Completed, acq_rel, acquire)` gated on `state_of(cur) ==
Running`, with a debug-mode assert on CAS failure. This closes a TOCTOU/lost-update window that a
future `deadline_ns`-driven force-complete path (the field already exists on `Descriptor`) would
otherwise silently hit — a bug that is invisible to TSan (both sides are `std::atomic`, so a
collision is a linearizability bug, not a data race) but was proven present under the original
plain-store form (100% double-finalization in the differential control) and eliminated by the
CAS-gated form (0/1,000,000). Cost is negligible: `complete()` runs once per handled message, off
the enqueue/dequeue hot path, at the same instruction class already paid by `try_claim`/`try_cancel`.

## Spec recommendations

- **`001-Actor-Execution-Model.md`**: Add `Descriptor::complete()`'s CAS-gated
  `Running -> Completed` transition (with the debug assert) to the documented lifecycle table
  alongside `try_claim`/`try_cancel`, explicitly noting it is now a compare_exchange, not a plain
  store, specifically to pre-harden against any future concurrent force-transition path (e.g. a
  deadline-expiry handler) that touches `gen_state` while a handler is `Running`.
- **`002-Scheduler.md`**: (a) Document the exec-state close-out contract's hard requirement,
  proven this round via `exec_state_handoff_race_test.cpp`: any consumer-side "recheck for
  pending work" step performed after releasing `Running` must read **only** shared/atomic state
  that a re-acquiring worker cannot yet be mutating (mirroring the incumbent's `tail_`-only probe)
  — it must never read consumer-private cursor state (`head_`-equivalent) unconditionally before
  confirming reacquisition of `Running` via CAS. Cite the Segmented Ring's disqualifying S1 failure
  as the canonical counter-example of what NOT to do. (b) Correct the S2 test citation: the
  committed `sched_no_lost_wakeup_test` runs 200,000 rounds and reports a boolean stall/no-stall
  result, not a percentage-based lost-wakeup rate — update any spec text (inherited from an
  earlier ADR-004 characterization) that describes a "~0.05-0.09% residual leak" to match the
  actual committed harness's pass/fail semantics.
- **`003-Memory.md`**: Add `tests/exec_state_handoff_race_test.cpp` and
  `tests/mailbox_busy_not_empty_test.cpp` and `tests/descriptor_complete_race_test.cpp` (all
  authored and proven this round) to the canonical test list backing the Mailbox's safety/
  correctness claims — S3 and C3 previously had no isolated, non-Engine-mediated committed
  harness, and ADR-020 is on record showing the obvious full-Engine substitute masks exactly the
  race S3 needs to prove. Also record the corrected `Descriptor::complete()` signature
  (compare_exchange_weak, not relaxed-load+release-store) in the Descriptor lifecycle section.
- **`015-Reentrancy-and-Quiescence.md`**: Add the C4 finding as a named risk class ("Running-state
  concurrent force-transition") in the reentrancy hazard catalogue, since any future
  deadline/timeout-driven forced-completion mechanism (011) must route through the same CAS-gated
  `complete()` rather than inventing its own write path to `gen_state`.

## Residual risks

- The weak-memory (ARM64) proof remains explicitly deferred — every measurement and sanitizer run
  this round (and all five prior rounds) was executed on x86-64 (GCC 14.2, Clang 20.1). TSan does
  not model ARM's relaxed store-buffering; a herd7/CppMem litmus test or real ARM64 hardware run
  is the one experiment that could still overturn the acq_rel/fence reasoning.
- The single-membership invariant ("a descriptor is in the mailbox at most once") is still
  enforced only by pool-allocation discipline plus a debug assertion, not checked on the release
  hot path — a caller bug (e.g., a retry path that double-enqueues a live descriptor) would
  silently corrupt the intrusive chain. No claim in any of the six rounds has directly targeted a
  double-enqueue fault injection; this is the single most concrete "next experiment" if this
  mailbox is challenged again.
- gen_state's 48-bit generation still wraps eventually; no runtime detection of the wrap exists.
  Documented but silent latent bound, unchanged from ADR-004.
- The Busy/bounded-spin path still trades worst-case tail latency for correctness under
  producer preemption between `tail_.exchange` and its link-store (015's honest correction); this
  is qualitatively bounded but not benchmarked quantitatively in this round either.
- Both disqualified challengers surfaced a real, transferable lesson worth carrying forward even
  though neither displaced the incumbent: (a) any close-out/recheck probe must never read
  consumer-private state unconditionally before confirming CAS-gated reacquisition (Segmented
  Ring's fatal S1), and (b) multi-step publish protocols (detach-then-link, as in BSR's
  `seal_segment`) that expose an intermediate state visible to only one of two racing observers
  are a recurring, sanitizer-invisible source of FIFO violations — worth a standing design-review
  checklist item for any future mailbox redesign proposal.

## Tie-breaking experiment (if this question is reopened)

None needed this round — evidence was sufficient to decide outright. If a seventh round is ever
opened, the single highest-value experiment would be a double-enqueue fault-injection test against
the incumbent's release hot path (the one invariant still unverified by direct executed evidence
across all six rounds), rather than re-litigating the two now-twice/six-times-disqualified
challenger lineages.
