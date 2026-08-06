# ADR-046: Cross-Node Mailbox Backpressure Signalling

## Status

Accepted (with residual risks — see below). Closes 010-Distribution.md's
"Open questions" item on remote-mailbox backpressure signalling for the
ordinary fire-and-forget tell/ask case.

## Question

010-Distribution's Transport seam is fire-and-forget (`send(NodeId,
MessageFrame)`, no delivery result) and 021 gives each peer pair exactly one
multiplexed connection. How does a REMOTE full (or near-full) mailbox signal
the sending node, so the sender can locally stall/shed BEFORE pushing more
frames onto the wire, without head-of-line-blocking the shared one-per-peer
connection — for plain cross-node tell/ask, not the reply-stream case ADR-018
already solved via `CrossNodeCredit`?

Three designs were debated, red-teamed, defended, and then proven with
compiled/executed C++ under the available toolchain (MSVC 19.51 + LLVM Clang
22.1.5 on Windows; TSan/UBSan unavailable on this box for either compiler
targeting `x86_64-pc-windows-msvc`, disclosed per design below and
substituted with ASan plus deterministic race/over-admission counting
methodologies).

## Designs (one-line summaries)

1. **PeerActorFlowCredit** — generalizes ADR-018's `CrossNodeCredit` shape to
   plain sends, keyed at (peer NodeId, destination ActorId): the receiver
   reuses its existing per-Activation `enqueued`/`drained` counters to detect
   a high/low watermark crossing and emits an edge-triggered
   `CongestionSignal{target, edge, drained_tail, gen}` over a new
   `FrameKind::Congestion`; the sender keeps a per-peer table, absent for
   every uncongested actor, gated by one relaxed load before every send.

2. **Coarse Per-Peer-Node Token Bucket + Edge-Triggered `Congested` Control
   Frame** — tracks backpressure at (sender-node, receiver-node) granularity
   only. The receiver periodically piggybacks an aggregate `resident_total`
   on the existing SWIM/capability-gossip `ControlMsg` (sizes a per-peer
   `TokenBucket`'s refill only) and emits one edge-triggered `Congested`
   control frame when it crosses a high-watermark, TTL-recovered (no explicit
   "clear" frame — the sender's own deadline expiry is the recovery path).
   The sender gates admission with one relaxed atomic load on the fast path,
   falling back to the existing 022 `TokenBucket`/`Admit` decision only while
   throttled.

3. **PeerBacklog** — a sender-local admission gate on `TcpTransport`'s own
   (currently unbounded) per-peer outbound byte queue, with no new wire
   protocol at all. Caps a real, independently-existing 022 violation
   (`outq_`/`Conn::out`/`pal::IoContext::queue_` growing without bound
   against a slow/dead peer) but — as its own authors and the executed
   evidence both confirm — cannot and does not detect a remote per-actor
   mailbox-full condition, because the receive path never blocks or signals
   back on ordinary local mailbox overflow.

## Evidence table

Only claims that both survived red-teaming (post-attack/rebuttal) and were
then proven CORRECT by executed C++ count toward the decision; INCONCLUSIVE
carries no weight; WRONG counts against.

| Design | Claim | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|---|
| 1. PeerActorFlowCredit | F1 (fast-path cost) | Yes (revised, fused-field commitment) | **INCONCLUSIVE** | Separate-map variant: +54ns/+10% over budget. Fused-field variant: +1.39ns, within budget — but the *real* repo integration (fusing into `SecureTransport`'s session map) was never built/measured, so the property is unproven as shipped. |
| 1 | F2-2 (0 cross-core RMW on drain) | Yes | **CORRECT** | 312.6M msg/s/core (31× the 10M floor); disassembly shows byte-identical drain bump, 0 `lock`-prefixed instrs on the taken path. |
| 1 | S1-2 (exhaustive/exclusive demux) | Yes | **CORRECT** | 2M fuzzed frames, 0 misroutes, 0 ASan trips. |
| 1 | S2 (FIFO under stall/resume) | Yes | **CORRECT**, but only after the prover found and fixed a **100%-reproducible ordering bug** (~4% inversions, 20/20 runs) in the original design (clear-before-drain race). |
| 1 | S3-2 (no HOL blocking) | Yes | **CORRECT**, but only after replacing the original per-peer `std::mutex` (proven, in cross-exam, to serialize *all* sends to a peer once *any* one flow stalls) with per-slot locks. |
| 1 | S4-2 (bounded heartbeat recovery) | Yes | **CORRECT**, but only after adding a driver (`DistributedRouter::tick`) the original design never showed, plus a grace-period fix for a starvation bug found while building the test. |
| 1 | S5-2 (bounded flow table, observable degradation) | Yes | **CORRECT**, but the prover **found and fixed two more real bugs** (broken tombstone deletion corrupting the open-addressed table; a stale-slot-reuse hazard) and **found and left unfixed** a third, more serious defect: |
| 1 | *(unclaimed)* | — | **PROVEN UNSAFE, NO FIX** | A **reentrant self-deadlock**, reproduced as an actual process hang, when flushing a stalled flow's staged queue under the per-slot lock re-triggers a new `Stall` back onto the same call stack. Prover's own words: *"no fix in the time available closes both S2's ordering requirement and this deadlock hazard simultaneously without deeper restructuring."* |
| 1 | C1-2, C2 | Yes | **CORRECT** | Zero-cost when ungoverned; Overflow-policy parity with local mailbox. |
| 2. Coarse Token Bucket | F1 (fast-path cost) | Yes | **CORRECT** | Marginal delta bounces both signs around a realistic baseline (±4–15%, all within noise of a ~50ns baseline); no systematic added cost. |
| 2 | F2b (bounded piggyback cost) | Yes | **CORRECT** | Byte delta exactly +24B/entry at every fan-out D (never O(N)); structural claim holds (specific ADR-045 numeric band not reliably hit, disclosed as sandbox noise, not a design defect). |
| 2 | S1 → S1b (bucket race) | **Fatal flaw found, then fixed and re-proven** | **CORRECT (post-fix)** | Original unsynchronized `TokenBucket::check()` reproduced 19–70% over-admission beyond configured capacity (both compilers, majority of trials). Fixed with a per-peer `bucket_mu_` (mirrors `PeerSession::session_mu_`'s existing precedent) — post-fix: **0/20 trials** over-admission on both MSVC and Clang, vs **16/20 trials** over-admission on the broken version under the identical harness. |
| 2 | S2 (TTL self-heals a dropped signal) | Yes | **CORRECT** | Sender resumes exactly at the reconstructed deadline; never re-wedges. |
| 2 | S3 (ADR-011 FIFO untouched) | Yes | **CORRECT** | 0 inversions/duplicates, 4M admissions, congestion actively toggled concurrently (scaled harness, disclosed vs. full ADR-011 scale). |
| 2 | S4 (control frame not HOL-blocked behind peer's own Data backlog) | **Serious gap found, then fixed and re-proven** | **CORRECT (post-fix)** | Single-FIFO baseline: control-frame latency scaled 80–96× with backlog. Two-queue priority writer (control ahead of data, never mid-frame): latency stays flat (0.6–0.7×) regardless of backlog, both compilers. |
| 2 | C1 (epoch gen-gate rejects stale signal) | Yes | **CORRECT** | Stale pre-restart `Congested` frame provably does not advance the throttle; a fresh one at current epoch does. |
| 2 | C2 (coarse granularity → real collateral shedding) | Yes | **CORRECT (confirmed, not falsified)** | Healthy co-located actor B: 100% shed rate during A's congestion window — the acknowledged cost, not a hidden one. |
| 2 | C3 (soft signal never substitutes for the hard local bound) | Yes | **CORRECT** | 006 Overflow policies enforce their configured bound exactly, independent of whether the gossip/Congested path is wired at all. |
| 3. PeerBacklog | C1 (grounding trace) | **Fatal, conceded false as argued** | — | `deliver_from_wire → Activation::post` is provably ungoverned on current master; `post_governed` has zero production callers. Superseded by C6. |
| 3 | C5 (caps total resident backlog) | **Fatal, conceded false as argued** | — | `pal::IoContext::queue_` dwell stage was invisible to the original counter. Superseded by C7 (fixed with a `reserved_bytes` pre-account). |
| 3 | C2, C4, C7 (revised) | Yes | **CORRECT** | Liveness, TSan-adjacent (ASan) safety, and the `reserved_bytes` fix all proven. |
| 3 | C3 (warm-path cost) | Yes | **WRONG (non-fatal sub-claim)** | Absolute bound (<25ns) held (9.96ns measured); the claimed "~2×  a single relaxed load" *ratio* characterization did not (22.8× measured for the realistic multi-peer cache). |
| 3 | C6 | Yes | **CORRECT — and dispositive** | Confirms, against real `Engine`/`LocalRouter`/`Activation`, that this design **does not and cannot** detect remote per-actor mailbox pressure: 200,000 real sends through the actual production entry points, 0 governance sheds observed, sender-side backlog counter stayed near baseline throughout. |

## Decision

**Winner: Design 2 — Coarse Per-Peer-Node Token Bucket + Edge-Triggered
`Congested` Control Frame.**

Rationale, applying the stated ranking in order:

1. **Safety is a gate.** Design 1 is disqualified. Beyond an inconclusive
   fast-path claim, the prove phase surfaced a **reentrant self-deadlock**
   reproduced as an actual process hang — a genuine liveness defect in the
   design's own required locking discipline (draining a stalled flow's
   staged queue under its slot lock can re-enter the same call stack when
   the flush re-triggers a `Stall` on the same flow) — with the prover
   explicitly stating no fix was found "without deeper restructuring."
   That is not a cheap, stated fix; per the ranking rule this alone
   disqualifies Design 1 from winning regardless of its other proven
   claims. Design 3 is disqualified on different grounds: its own C6 proof
   (run against the *real* `Engine`/`LocalRouter`/`Activation`, not a
   reimplementation) shows it structurally cannot answer the question this
   task asks — it bounds a real, independently-existing local resource
   (`outq_`) but a sender's backlog counter stays at baseline while a
   remote actor's mailbox is actively overflowing, because no byte of
   receiver state is ever transmitted back. It is a legitimate, separately
   motivated 022 fix, not an answer to 010's open question.

2. **Proven beats claimed.** Design 2 is the only one of the three whose
   entire final claim set — F1, F2b, S1b, S2, S3, S4, C1, C2, C3 — came back
   CORRECT after cross-examination and re-proof, with the one fatal flaw
   found in red-teaming (the unsynchronized `TokenBucket`, reproducibly
   over-admitting by 19–70%) fixed with a narrow, precedented change (a
   per-peer mutex on the cold path only, mirroring `PeerSession`'s existing
   `session_mu_`) and then *re-measured* to confirm the fix: 0/20 trials
   over-admission post-fix vs. 16/20 pre-fix, on both compilers. Design 1's
   claims required repeatedly finding and patching real bugs during the
   prove phase itself (a 100%-reproducible FIFO-inversion bug, a heartbeat
   starvation bug, a table-corruption bug) and *still* left one unresolved.
   Design 3's own two most consequential claims (C1, C5) were conceded
   fatally wrong as originally argued.

3. **Measured hot-path numbers, among safe survivors.** With Design 1
   disqualified by the safety gate, this tier only needs to confirm Design 2
   clears the budget, which it does: F1's uncongested marginal cost is
   within noise of zero versus a realistic baseline (single relaxed load +
   predicted-not-taken branch, no allocation, no RMW), and F2b's piggyback
   cost is a fixed +24 bytes/round regardless of cluster fan-out — never
   O(N).

4. **Core invariants.** Design 2's granularity (per-peer-node, not
   per-destination-actor) is a real, disclosed, and *proven* cost — C2
   shows a healthy co-located actor gets shed at the same rate as the
   actor that actually triggered congestion. This is a precision/fairness
   trade, not a violation of the task's literal "no stall" invariant:
   admitted Data frames for the healthy actor are never queued behind or
   delayed by the congested one (S3/S4 prove no reordering, no duplication,
   and — after the two-queue write-priority fix — no HOL-blocking of the
   `Congested` control frame itself behind the peer's own reverse-direction
   Data backlog); the cost is coarser *admission*, not wire-level queueing
   delay. It composes with, and never substitutes for, 006's hard local
   bound (C3) and 022's shed-don't-buffer discipline (the shed path reuses
   the same typed `overloaded` error and `Admit` enum every other
   governance checkpoint uses). ADR-011's proven cross-node FIFO is
   untouched by construction (admission gates *before* a Data frame is
   even constructed — S3). One socket, one connection per peer, unchanged
   (021).

## Residual risks (carried forward, not blocking adoption)

- **Collateral shedding is real and by design.** A congested node throttles
  every actor on it, including ones with headroom (C2, confirmed).
  Workloads with one persistently hot actor co-located with many cold ones
  on the same node will see avoidable shedding for the cold actors during
  the hot actor's congestion windows. If this proves costly in practice,
  the natural, additive follow-up is a second, optional per-(peer,actor)
  refinement *layered on top* of this coarse gate for specifically
  identified hot destinations — not a replacement.
- **Gossip propagation scope is FullMesh/D1-only as specified.** The
  pressure/`Congested` signal reaches only a node's direct, open
  connections; it is not relayed transitively the way ADR-045's capability
  digest is. Under 026 `BoundedPartialView`/Gateway topologies, a node
  outside the congested node's direct view never learns of the congestion
  via this path at all (not delayed — absent). 006's hard local bound is
  the correctness backstop for those nodes; only the soft, latency-hiding
  benefit is lost. Flagged as an explicit, honest scope limit — extending
  propagation to 026 relay topologies is a named follow-on, mirroring
  ADR-019's own cross-node-broadcast Draft status.
- **Aggregation cost at large actor populations.** Summing `resident_total`
  across all actors on a node each tick is O(actors-per-node) on the
  protocol thread (not the message hot path, but unbounded in actor count).
  Needs a cap, sampling, or incremental partial sums for nodes hosting very
  large actor populations before this is validated at 026 scale.
- **Broadcast fan-out at large N.** `broadcast_control(Congested, ...)` is
  O(#peers) per watermark-crossing event — acceptable at bounded-cluster
  scale, not yet validated against 026's O(N²)-avoidance posture at large N.
- **Watermark/TTL tuning is unvalidated operational config.**
  `high_watermark_`/`low_watermark_`/`congestion_window_ns`/
  `congestion_refresh_ns` need real workload tuning; too tight flaps
  Congested/uncongested (repeated collateral-shedding bursts), too loose
  arrives too late to matter. Same open tension 022 already leaves for
  adaptive-limit controllers generally.
- **Numeric proof was run on MSVC 19.51 + Clang 22.1.5 on Windows only** —
  the project's reference matrix is g++ 14.2 / clang++ 20.1 under
  ASan/UBSan/TSan on the documented Linux target. TSan/UBSan were
  unavailable on this box for either compiler; the S1b race fix was instead
  validated by deterministic over-admission counting (a lost update
  manifests directly as a capacity violation, measured, not inferred) on
  both compilers, and ASan ran clean on all concurrency-bearing tests. A
  full TSan run on the reference Linux toolchain is recommended before
  this is treated as fully proven per this repo's own sanitizer matrix.
- **This design does not need, and deliberately does not implement,** the
  per-(peer,actor) precision Design 1 attempted. If a future workload
  demonstrates the collateral-shedding cost above is unacceptable, revisit
  Design 1's *shape* (per-(peer,actor) flow, monotone gen-merge, fail-open
  to 006) as a targeted addition — but only after its proven reentrant
  self-deadlock is fixed by the restructuring its own prover named as
  necessary (e.g., draining a stalled flow's staged queue via a dedicated
  flush queue serviced off the notifying call stack, decoupling reentrant
  notification from the flush itself) and re-proven under TSan on the
  reference toolchain.

## Spec update recommendations

**`010-Distribution.md`**
- Move the "Open questions" item on remote-mailbox backpressure signalling
  to Resolved, citing this ADR.
- Add a new subsection ("Cross-node backpressure") documenting:
  `PeerCongestionGate` as a field folded into the existing per-peer
  connection-state table (same one `PeerSession` already lives in — no new
  map); the fast path (`congested_until_ns` relaxed load, predicted-false
  branch); the cold path (022 `TokenBucket`/`Admit`, guarded by a per-peer
  `bucket_mu_`); the wire representation (`ControlKind::Congested`, riding
  the existing `FrameKind::Control` class, no new frame kind, no second
  socket — 021 unaffected); and the explicit scope limit that propagation
  is direct-connection-only (FullMesh/D1), not gossip-relayed, pending a
  026-scale follow-up.
- Document the two-queue (`pending_control_`/`pending_data_`) write-priority
  discipline inside the concrete default Transport as a required property
  of any 010-conformant Transport carrying this mechanism: Control frames
  must be dequeued ahead of Data frames between (never mid-) frame writes,
  so a Congested announcement's own latency is bounded independent of the
  sender's own reverse-direction Data backlog.

**`006-Messaging-and-Addressing.md`**
- Add a note under the mailbox bound/overflow section that a REMOTE sender's
  admission decision (Design 2's `PeerCongestionGate`) is strictly a
  soft, latency-hiding front-end to this section's hard local bound —
  the local Overflow policy remains the sole correctness guarantee and is
  unconditionally enforced regardless of whether cross-node congestion
  gossip is wired, partitioned, or dropped (proven: C3).
- Cross-reference the new `ControlKind::Congested` payload fields
  (`from_incarnation`, `remaining_ns`) as reusing 006's existing deadline
  vocabulary (018-style deadline-travel reconstruction), not inventing a
  new one.

**`022-Resource-Governance-and-Overload-Control.md`**
- Add `PeerCongestionGate`/`TokenBucket` (per-peer-node) as a named
  governance checkpoint alongside `FairShare`/`CircuitBreaker`, explicitly
  noting the granularity choice (per-peer-node, not per-destination-actor)
  and its proven cost (C2: a healthy co-located actor is shed at the same
  rate as the congesting one) as an accepted O(1)-bookkeeping trade,
  consistent with 022's stated preference for cheap, approximate,
  per-shard-local governance over exact global accounting.
- Record the corrected memory-order discipline for any future
  multi-writer-shared checkpoint of this shape: a bucket shared by
  concurrently-calling worker/shard threads needs an explicit mutex around
  its check-and-consume step (the `TokenBucket` type itself remains
  documented single-writer); relaxed loads alone are only safe for fields
  that guard no other memory (`congested_until_ns`), not for the bucket's
  own internal RMW.
- Separately (independent finding surfaced by Design 3's C6 proof, not
  fixed by this ADR): flag that `Activation::post` reached via
  `deliver_from_wire → PostCourier → Engine::post` is currently
  **ungoverned** — `Overflow` policy / `post_governed` is not on the
  production wire-arrival path at all (zero reachable callers from `tell`/
  `deliver_from_wire`; only test code and `stateless_pool.hpp` call it
  directly). This means 006/022's bound-every-resource discipline is not
  actually enforced on remote message arrival today, independent of
  anything in this ADR. Recommend opening a follow-up ADR to wire
  `post_governed` (or equivalent) into the real inbound-wire path before
  either this ADR's `Congested` signal or Design 1's per-actor variant can
  be said to protect a genuinely bounded resource end-to-end.

**`021-Cluster-Formation-and-Lifecycle.md`**
- Add the `ControlKind::Congested` value and the `MailboxPressureEntry`
  payload shape to the enumerated Control-frame catalogue (alongside
  `MemberUpdate`/`CapabilityDigestEntry`/revocation `Fingerprint`), noting
  it is bounded and piggybacked on the existing gossip round (ADR-045's
  channel), not a new gossip class.
- Note explicitly that this mechanism's propagation is scoped to a node's
  direct, currently-open connections (021's one-per-peer-pair invariant is
  unaffected — no new socket is opened), and that it does **not**
  transitively relay the way capability digests do; extending it to
  026 `BoundedPartialView`/Gateway relay topologies is an explicit,
  named open item, not silently assumed to already work at that scale.

## Single tie-breaking experiment, if this decision needs revisiting

Run the full S1b reproduction (4 sender threads × 200k iters × 10 trials,
over-admission counting) plus the S3/S4 harnesses under real TSan on the
project's reference `g++ 14.2`/`clang++ 20.1` Linux toolchain, pinned per
CLAUDE.md's machine-safety rules (`taskset -c 0-3`, `-j1` for TSan builds).
If that run reproduces the same 0/20-over-admission, 0-inversion, and
flat-control-latency results this ADR is proven on the project's actual
target platform, not just the Windows/MSVC substitute used here, and no
further tie-break is needed.
