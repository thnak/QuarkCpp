# ADR-045: SwimMembership Disseminates NodeCapabilities via Bounded Piggyback on the Existing Gossip Digest

## Status

Accepted

## Question

021 (SWIM membership) and 025 (static capability-gated placement) both assume `CapabilityView`
is fed by real, network-backed cluster state. In the codebase as found, `SwimMembership`
(`include/quark/core/cluster.hpp:345`) gossips only the live `NodeId` roster — zero mentions of
"capabilit" anywhere in the file — and `membership.hpp:16`'s comment promising "025: static
capabilities gossiped in the SWIM join payload" was never implemented; only
`InProcessCapabilityView`, a std-only test double, exists. `VoiceChannel` (ADR-030) and
`Require<...>`/`Prefer<...>`/`Weighted` placement (ADR-013) already depend on a live
`CapabilityView` but require a caller to construct and push one in manually.

The question: how should `SwimMembership` actually disseminate a node's advertised
`NodeCapabilities` to the rest of the cluster over the real `Transport`, reusing the existing
bounded-piggyback-digest gossip machinery the same way ADR-040 piggybacked revocation
fingerprints onto control frames — proven (fast) bounded per-tick payload growth, (safe)
race/UB-free under concurrent republish, and (correct) eventually-consistent convergence with
incarnation-based conflict resolution — without touching the mailbox/dispatch hot path or
bending SWIM's own convergence/placement-determinism invariants?

## Designs summarized

1. **Full-Snapshot Capability Piggyback** — extends `ControlMsg` with a
   `capabilities: vector<CapabilityDigestEntry>{node, incarnation, opaque blob}` field riding the
   *same* frame as `updates`/ADR-040 `revocations`. A new `CapabilityRegistry` class is wired via
   `set_capability_gossip(pull, merge)` (identical shape to `set_revocation_gossip`): `pull()`
   runs inside `send_control()` and attaches cached blobs up to `max_capability_gossip_bytes`;
   `merge()` runs inside `handle_control()` and replaces a node's whole cached entry when the
   incoming SWIM incarnation is newer, with a same-incarnation tiebreak. `table_` is
   protocol-thread-owned (no lock, mirrors `members_`); a `std::atomic<shared_ptr<const CapMap>>`
   publishes cross-thread reads; a `std::atomic<shared_ptr<const NodeCapabilities>> pending_local_`
   is the sole cross-thread write entry point. `SwimMembership` itself stays
   capability-ignorant, moving only opaque bytes.

2. **Delta/version-vector piggyback** — adds a per-node `cap_generation` counter and a bounded
   `CapabilityDigestEntry{node, incarnation, cap_generation, base_generation, changed[]}` list to
   the same `ControlMsg`. Steady state emits only a 24-byte version stamp; a changed field-set
   rides only during a bounded active-push countdown after a publish, backstopped by round-robin
   full-snapshot anti-entropy. Acceptance gates first on incarnation, then `cap_generation`
   within it. Cross-thread exposure is mutex-guarded (`cap_mu_`, `pending_caps_mu_`), deliberately
   not lock-free, on the reasoning that control-plane cadence makes a mutex's cost irrelevant.

3. **Lazy-pull anti-entropy over SWIM's ping/ack round-trip** — attaches a fixed 17-byte
   `(cap_incarnation, cap_digest, cap_pull_request)` stamp to every Ping/Ack/PingReq/PingReqAck
   frame; full content rides only the Ack answering a Ping whose sender detected (via digest) a
   stale cache, backstopped by a slow recheck interval. Reuses `self_incarnation_`/self-refutation
   as the sole freshness axis. Targets a smaller steady-state footprint at the cost of an O(N),
   not O(log N), worst-case convergence bound (SWIM's direct-probe round-robin, not its gossip
   fanout).

## Evidence table

| Claim | Design | Kind | Survived red-team? | Proven? | Number / verdict |
|---|---|---|---|---|---|
| F1 bounded per-tick payload | 1 | fast | yes (fixed: `break`→`continue`, self-blob guard) | **CORRECT** | encoded `capabilities` bytes ≤ 4096 exactly at N=10/100/1000 (850B/4084B/4080B) after fixing an unaccounted wire-framing-overhead bug found by the prover |
| F2 zero dispatch/hot-path cost | 1 | fast | yes (wording narrowed) | **CORRECT** | placement-path read delta −4% to −5% (noise); `merge()`→`publish_snapshot()` ratio 0.33–0.34 rebuilds/frame (≤1.0 ceiling); mailbox/dispatch headers structurally never include cluster.hpp/capabilities.hpp |
| S1 publish-vs-gossip race-free | 1 | safe | yes | **CORRECT*** | 160k publishes vs 10k tick() rounds, clean under ASan(MSVC)+ASan/UBSan(clang); *TSan unavailable on prover's box, substituted with debug-CRT stress |
| S2 no torn cross-thread reads | 1 | safe | yes | **CORRECT** (after a real bug fix) | prover found and fixed a genuine UAF (`capabilities_of()` returning a reference into a function-local `shared_ptr`); 0 torn reads across ≥1.6M reads post-fix |
| S3 decode never OOB / bounded alloc | 1 | safe | yes | **CORRECT** | 20k fuzzed inputs, 0 crashes under ASan+UBSan; oversized-but-well-formed frame correctly rejected by `kMaxDecodedCapabilityBytes` |
| C1 O(log_fanout N) convergence | 1 | correct | yes (fixed: `break`-starvation bug) | **CORRECT** | N=8/32/128 → 1/1/2 rounds, vs. 5×-slack bound of 12/18/25 (6–12× faster) |
| C2 incarnation precedence, no 2nd axis | 1 | correct | yes | **CORRECT** | 100/100 shuffled-order runs converge to incarnation-7, never incarnation-5 |
| C3 deterministic same-incarnation tiebreak | 1 | correct | yes (fixed: blob-only tiebreak → `local_seq`) | **CORRECT** (after a real bug fix) | prover found the original blob-byte tiebreak silently discarded a node's own second `publish_local()` (violates documented "last store wins"); fixed with `local_seq`, both sub-cases verified |
| C4 Dead-node eviction within one tick | 1 | correct | (not attacked) | **CORRECT** | evicted same tick `sweep_hook_` fires; live count 2→1, `capabilities_of()` reverts to empty |
| F1 O(1) steady-state payload | 2 | fast | yes (starvation gap closed w/ dedicated quota) | **CORRECT** | flat ~59.8B avg across N=8..64 |
| F2 O(log N) round convergence | 2 | fast | yes (fixed: per-send→per-tick countdown accounting bug) | **CORRECT** | rounds flat at 3 across N=8,16,32,64 (round growth 1.0× vs N growth 2.0×/step) |
| S1 publish-thread safety | 2 | safe | (not attacked) | **CORRECT*** | 5/5 clean ASan+UBSan runs; *TSan unavailable |
| S2 gap-drop, never partial apply | 2 | safe | (not attacked) | **CORRECT** | gap-spanning delta dropped whole; snapshot entry heals in one step |
| C2 incarnation-first precedence | 2 | correct | yes (fixed: shadow-copy incarnation lag bug) | **CORRECT** (after a real bug fix) | prover found `CapState::incarnation` could lag a self-refutation bump; fixed by reading live `self_incarnation_`/`members_[node].incarnation` directly |
| **C1 (original) live-mutation-without-epoch convergence** | 2 | correct | **NO — conceded fatal** | — | red-team's 2-node executableCheck reproduced same-epoch, divergent-`CapabilityView` split → possible double-activation of a stateful actor; **directly contradicts** `capabilities.hpp`'s and `placement_policies.hpp`'s own documented determinism invariant |
| C1b (replacement, raw tier only) | 2 | correct | yes, rescoped | **CORRECT**, with caveat | 36/36 trials; but worst lossy case needed ~40× `resync_interval×node_count`, not the ~1× the design's own back-of-envelope implied |
| S3 (new: placement-committed tier via `roster_digest()`/StabilizationWindow) | 2 | safe | yes | **CORRECT** | closes the double-activation window; requires a **second CapabilityView tier** and folding capability digests into the roster/epoch/Hand-off machinery |
| F1 fixed 17B/frame, no content leak | 3 | fast | yes (fixed: leaked 17B onto Gossip/Join* frames) | **CORRECT** | 0 leaked bytes on Gossip/Join*, exactly 17B on probe-kind frames, N=4..64 |
| **F2 crossover byte threshold** | 3 | fast | **NO — measured WRONG** | **WRONG** | claimed worst case 5 frames/85B (crossover ≈28.3B); measured worst case is 4 frames/68B (crossover ≈22.7B) — `send_indirect()`'s early-exit makes 2 successful `PingReqAck`s in one cycle architecturally impossible under this transport |
| S1 publish-thread safety | 3 | safe | (not attacked) | **INCONCLUSIVE*** | TSan unavailable on both toolchains present; substituted stress (≈15M ops across 3 hardening modes) found 0 failures, but literal TSan clause unproven |
| S2 monotonic incarnation vs. reorder | 3 | safe | (not attacked) | **CORRECT** | stale replay at incarnation 3/2 rejected after incarnation 6 adopted |
| S3 receiver-side decode bound | 3 | safe | yes (fixed: bound moved from publish-side-only to decode-side) | **CORRECT** (after a real gap fix) | 1M fuzz iterations clean; 10k-flag oversized-but-well-formed frame correctly rejected against receiver's own limit |
| **C1 (original) direct-Ping-only convergence** | 3 | correct | **NO — fatal, indirect-only-reachable nodes never converge** | — | `cap_pull_request`/`cap_payload` only rode the direct Ping→Ack path; a node reachable only via relay stayed `Alive` (SWIM detection fine) but capability-invisible **permanently** |
| C1-revised (subject-relative pull forwarded through PingReq/PingReqAck) | 3 | correct | yes, after a real fix | **CORRECT** | indirect-reachable convergence confirmed; convergence class remains **O(N) probe-rounds, not O(log N) gossip-rounds** |
| C2 incarnation-gated replay rejection | 3 | correct | (not attacked) | **CORRECT** | replay at old incarnation rejected cluster-wide |
| C3 duplicate-delivery idempotency | 3 | correct | (not attacked) | **CORRECT** | 4× duplicate delivery, byte-identical state throughout |

\* TSan is unavailable on the Windows/MSVC+clang toolchain the prover ran on for **all three
designs** — this is an environment gap, not a design-specific one, and is called out as a
residual risk below, not treated as a design differentiator.

## Decision: Design 1 — Full-Snapshot Capability Piggyback wins

**Winner: "Full-Snapshot Capability Piggyback on SwimMembership's Bounded Gossip Digest."**

**Safety gate (rule 1).** No design has a `safe`-kind claim marked WRONG by the prover. All
three pass the gate. (Design 3's F2-WRONG is a `fast` claim, not `safe`/`correct`, so it does not
disqualify — but it does count against Design 3 under rule 2, and Design 3's S1 is
INCONCLUSIVE, contributing zero weight either way.)

**Proven beats claimed (rule 2).** Counting only claims that survived red-teaming *and* were
proven CORRECT with executed evidence, counting disproven claims against:

- Design 1: **9/9** surviving claims proven CORRECT, 0 disproven, 0 inconclusive.
- Design 2: **7/7** surviving claims proven CORRECT, 0 disproven, 0 inconclusive — but only after
  **conceding its own headline correctness claim (original C1) as fatally broken** and bolting on
  an entirely new placement-committed `CapabilityView` tier (S3) that folds capability state into
  `roster_digest()`/`StabilizationWindow`/epoch/Hand-off to recover placement determinism.
- Design 3: **6** claims proven CORRECT, **1 proven WRONG** (F2 — its own revised numbers,
  arrived at specifically to fix a red-team finding, were still wrong when measured), **1
  INCONCLUSIVE** (S1) — and its own headline correctness claim (original C1) was **also**
  conceded fatal (permanent non-convergence for indirectly-reachable nodes) before being patched.

Design 1 is the only design whose original claim set survived to proof without conceding a
*fatal, architecture-level* flaw in its core convergence story. Its two in-flight bug fixes
(`break`→`continue` starvation, blob-byte tiebreak → `local_seq`) were narrow, one-line
corrections to its own pseudocode caught by the prover's own experiments — not a redesign — and
both were re-verified CORRECT afterward. The prover also caught and fixed a genuine
use-after-free in `CapabilityRegistry::capabilities_of()` during S2's proof, which is exactly the
kind of memory-safety finding this process exists to surface before merge, not a mark against the
design.

**Best measured hot-path numbers among safe survivors (rule 3).** All three designs are
control-plane-only by construction and none touch the mailbox/dispatch hot path (verified
structurally for Design 1 by direct grep showing `mailbox.hpp`/`dispatch.hpp`/`activation.hpp`
never include `cluster.hpp`/`capabilities.hpp`). Since none is on the actual mailbox hot path,
this axis is secondary to correctness here, but Design 1's placement-side read cost (the one
touch point placement policies actually pay) measured **-4% to -5%** versus the pre-existing
`InProcessCapabilityView` baseline — i.e., statistically free — and its convergence numbers
(1–2 gossip rounds up to N=128) are the fastest and most standards-matching of the three (SWIM's
own O(log_fanout N) class), against Design 3's explicitly weaker O(N) probe-round bound and
Design 2's C1b caveat that worst-case lossy convergence ran ~40× looser than its own back-of-envelope
estimate.

**Core invariants (rule 4).** This is where Design 1 separates most clearly:

- Design 1 keeps `SwimMembership` **capability-ignorant** throughout — it moves opaque bytes
  exactly as it already does for ADR-040 revocation fingerprints, touching nothing in the
  roster/epoch/`StabilizationWindow`/Hand-off machinery. This is the task's explicit requirement
  ("reuses the existing bounded-piggyback-digest gossip mechanism... NOT a new side-channel")
  taken most literally and verified most directly.
- Design 2, to survive, had to **couple capability convergence to the membership epoch** via a
  new placement-committed tier — a real, working fix, but a materially larger footprint that
  touches `roster_digest()`, the commit/epoch pipeline, and Hand-off/fencing, i.e. it bends
  021's Hand-off procedure to also gate on capability content, something the original design
  explicitly (and, per the red team, wrongly) tried to avoid.
- Design 3 converges capability content on **SWIM's direct-probe schedule** (O(N) rounds), which
  is a strictly weaker convergence class than the gossip-fanout dissemination SWIM already gives
  membership itself — directly short of the task's stated invariant ("the same convergence class
  SWIM already gives membership"). Its own risk list concedes this; it is not a red-team artifact.

Design 1 is the only design that (a) never violates the placement-determinism invariant in the
first place — because a node's `CapabilityView` snapshot only ever changes atomically per merge,
and merges are gated on the same incarinated freshness axis SWIM already uses for membership, so
there is no epoch/capability split window to defend against — and (b) matches SWIM's own O(log
fanout N) convergence class, empirically confirmed up to N=128.

## Decision

Adopt **Design 1 (Full-Snapshot Capability Piggyback)**, with the two fixes the prover applied
folded in as part of the accepted design, not as follow-up work:

1. `CapabilityRegistry::pull()`'s budget-packing loop uses `continue`, not `break`, when a
   candidate entry would not fit the remaining `max_capability_gossip_bytes` budget, and enforces
   (assert in debug, log-and-drop in release) that a node's own published blob never itself
   exceeds the budget.
2. `CapabilityDigestEntry` gains `std::uint32_t local_seq`, a monotonic counter owned by the
   publishing node and bumped on every `pull()` self-claim at a fixed incarnation.
   `merge_one()`'s same-incarnation acceptance order is `(incarnation, local_seq)` first, falling
   back to a byte-lexicographic blob tiebreak only for genuine `(node, incarnation, local_seq)`
   collisions (forged/duplicated entries).
3. `CapabilityRegistry::capabilities_of()`/`view()` must return by value (or otherwise keep the
   source `CapMap` alive for the whole call), never a reference into a function-local
   `shared_ptr` — the UAF the prover found and fixed.
4. `publish_snapshot()` is called at most once per `handle_control()` invocation (batched at the
   end of `merge()`), not once per accepted entry, to bound protocol-thread CPU cost on the
   `io_thread_` that also pumps data-plane frames.

## Spec recommendations

**021-Cluster-Formation-and-Lifecycle.md**
- Add a "Capability gossip" subsection describing `SwimMembership::set_capability_gossip(pull,
  merge)` as the wiring point, explicitly mirroring `set_revocation_gossip`'s shape (ADR-040) and
  stating that `SwimMembership` stays capability-ignorant — it moves opaque
  `CapabilityDigestEntry{node, incarnation, local_seq, blob}` bytes only.
- Document the `ControlMsg.capabilities` wire field and its codec (length-prefixed, bounds-checked
  via `SwimByteReader`, capped by `kMaxDecodedCapabilityBytes`) alongside the existing `updates`
  and ADR-040 `revocations` fields in the control-frame catalog.
- Document `Config::max_capability_gossip_bytes` (default 4096) as a sibling of
  `max_gossip_updates`/`gossip_fanout`.
- Document conflict resolution precisely: incarnation strictly-greater wins; within the same
  incarnation, `local_seq` (same-node monotonic publish order) decides; a last-resort
  byte-lexicographic blob comparison exists only for genuine same-`(node, incarnation, local_seq)`
  collisions. State explicitly that no second freshness mechanism is introduced.
- Document Dead-node eviction: `CapabilityRegistry::evict_dead` is wired onto the existing
  `sweep_hook_` (no new timer); a node's capability entry is purged within one further `tick()`
  after it is marked Dead. Note this wiring is bootstrap-code responsibility with no compile-time
  enforcement — flag as an operational footgun for node-init code review.
- Record the known open gap: `fill_digest()`'s unordered-map iteration order has no
  staleness/priority weighting, so capability propagation fairness for forwarded (non-self) nodes
  in clusters larger than `max_gossip_updates` has not been proven under adversarial bucket
  layouts (only real hashing up to N=128). Recommend a follow-up experiment or ADR if this proves
  to matter at production cluster sizes.

**025-Placement-Policies-and-Stateless-Workers.md**
- Correct `membership.hpp:16`'s stale intent comment: capabilities are no longer described as
  "gossiped in the SWIM join payload" — they are disseminated continuously via the bounded
  piggyback-digest gossip channel (the same `ControlMsg` carrying membership `updates`), so a
  freshly joined node becomes capability-visible within the same O(log_fanout N) convergence
  window as membership itself, not necessarily atomically at Join/JoinAck.
- Document `CapabilityRegistry` as the real, network-backed `CapabilityView` producer for 021/010,
  with `InProcessCapabilityView` retained explicitly as the std-only test double.
- Keep 025's "capabilities are static for a node's lifetime — a change is a rejoin, never a live
  mutation" guidance as the *recommended* usage pattern for callers (since live republish before
  an incarnation bump is the less-exercised path), but note it is no longer a hard requirement:
  `publish_local()`/`local_seq` make live republish safe and deterministic if used, proven by C3.

**010-Distribution.md**
- Add capability gossip to the control-plane/data-plane separation discussion: it lives entirely
  inside `SwimMembership::tick()`/`send_control()`/`handle_control()` on the protocol thread,
  structurally verified never reachable from mailbox/dispatch/activation headers, with zero
  measured hot-path or dispatch-path cost (placement-side `CapabilityView` reads measured within
  noise of the pre-existing baseline).
- Add `ControlMsg.capabilities` to the control-frame wire catalog alongside `updates` and
  ADR-040's `revocations`.

**020-Security.md**
- Add the capability-entry decoder to the adversarial-frame threat model alongside the existing
  `MemberUpdate`/Fingerprint decode discipline: every field routes through bounds-checked
  `SwimByteReader` accessors, plus a defensive `kMaxDecodedCapabilityBytes` total-allocation cap
  independent of declared per-entry blob lengths — proven via 20k-input fuzzing under ASan+UBSan
  with zero crashes and zero over-cap/partial accepts.
- Note as a residual, *not newly introduced*, risk: capability content itself is not
  cryptographically authenticated — a compromised or buggy peer inside the cluster's existing
  mTLS/session trust boundary can still advertise false capabilities (e.g. a false `Flag{"gpu"}`).
  This is the same trust model membership/revocation gossip already operates under; no new gap is
  introduced, and no new mitigation is added by this design.

## Residual risks

- **TSan was unavailable on the prover's Windows/MSVC+clang box for all three designs.** Every
  "safe" verdict above that cites TSan was substituted with ASan/UBSan(clang) plus a debug-CRT
  stress harness. This is a real gap against the project's own stated sanitizer matrix
  (`build-tsan` with GCC 14.2/Clang 20.1 on Linux, per CLAUDE.md). **Before merging, rerun S1/S2's
  concurrency-stress experiments under a real `build-tsan` on the project's actual Linux CI
  target** — this is the single most important open verification gap, independent of which design
  is chosen.
- Steady-state bandwidth: Design 1 retransmits a node's *full* encoded capability blob on every
  round it rides (no per-field diff/unchanged-skip), so marginal gossip cost scales with active
  blob size on rounds a node is selected, not just on actual change — a real, accepted tradeoff
  against Design 2's smaller-footprint delta approach, justified here by Design 1's much smaller
  blast radius on the placement-determinism invariant. If steady-state bandwidth becomes a
  measured problem at larger cluster/capability-set sizes, Design 2's delta/anti-entropy machinery
  (now proven correct in isolation) is the natural next escalation, but would need its own
  placement-committed-tier treatment re-derived for whatever change triggers it.
- `fill_digest()`'s unordered-map iteration order has no staleness/priority weighting; capability
  propagation fairness for forwarded (non-self) nodes in clusters exceeding `max_gossip_updates`
  members is proven only under real (non-adversarial) hashing up to N=128, not proven against
  adversarial bucket layouts that could pathologically starve a specific node's entry.
- Dead-node eviction correctness depends on bootstrap code actually wiring
  `CapabilityRegistry::evict_dead` onto `set_sweep_hook`; nothing enforces this at compile time or
  runtime, so a misconfigured node can silently leak dead nodes' capability entries indefinitely.
- The `local_seq` same-node tiebreak assumes `pull()`'s self-claim path is the only place
  `local_seq` is incremented; any future code path that also claims `pending_local_` outside
  `pull()` would need to preserve this monotonic-increment discipline or the same-node ordering
  guarantee silently regresses.
- No cryptographic authentication of capability content (see 020-Security.md recommendation
  above) — accepted as consistent with the existing gossip trust model, not a new gap, but also
  not newly mitigated by this work.

## Single tie-breaking experiment, if this decision needs revisiting

If steady-state gossip bandwidth at large cluster size / large capability-set size is later found
to matter in practice (contradicting the "accepted tradeoff" framing above), the tie-breaking
experiment is: run Design 1's `CapabilityRegistry` and Design 2's delta/version-vector registry
side-by-side on the same N=500+, high-churn, large-capability-set (200+ facts/node) virtual-clock
cluster and measure sustained steady-state bytes/tick/node for each — that is the one condition
under which Design 2's added placement-committed-tier complexity could become worth paying for.
