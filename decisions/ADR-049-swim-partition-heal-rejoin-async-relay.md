# ADR-049: SWIM Partition-Heal Rejoin + Asynchronous Indirect-Ping Relay

## Status

**Accepted and merged** (2026-10-22) into `include/quark/core/cluster.hpp`, with the merge
conditions in "Decision" below met; see "Merge record" at the end. This ADR resolves
OpenQuestions.md item **021** (the two SWIM defects found 2026-10-21).

## Question

`SwimMembership` (021 §3, `include/quark/core/cluster.hpp`) has two defects that
`run_swim_chaos.sh` measured on 5 real processes in separate network namespaces with iptables cuts:

1. **Partition-heal collapse.** `apply_update` lets `Dead` replace `Alive` at an *equal*
   incarnation, and `Dead` is terminal. After a heal, the node that was cut off gossips its stale
   "everyone is Dead" view, and healthy nodes believe it about *each other* before the subject can
   refute. Measured: isolating node 5 for 10 s left node 1 at `*DDDD`, and a {1,2}|{3,4,5} split
   left only the 4–5 pair connected. Also, `Join` on a node that is already Dead does nothing, so a
   healed node can never get back in. This contradicts 021 §1.
2. **The indirect ping does not work over a real transport.** The `PingReq` relay expects the
   subject's Ack to arrive in the same call stack, which only the loopback does. Over `TcpTransport`
   the Ack is dropped. Measured: with only the 1↔2 link cut, incarnation went from 1 to 31 in 22 s.

The question is how to fix both together without breaking any of the following: the single-thread
contract, determinism under the injected clock, crash detection, the rule that a departed or
re-epoched node is not resurrected, the accidental-merge guard, bounded state, and the existing
tests.

## Designs (one-line summaries)

1. **SAR/ART: Subject-Authoritative Resurrection + Bounded Async Relay Table.** Only a frame from
   the subject itself, or a Join from the subject, can lift its tombstone. A third-party Dead at an
   equal incarnation becomes Suspect, but only if this node heard the subject directly within
   `suspicion_timeout`. Tombstones are probed one per `tombstone_probe_interval`. `leave()` sets a
   sticky departed bit, carried in bit 7 of the wire status. The relay keeps an inline
   `std::array<RelaySlot,32>` with tagged sequence numbers and deadlines on the injected clock.
2. **FHT/TR: Expiring first-hand tombstones + accusation-targeted digest + stateless token relay
   (revised).** A node adopts Dead only from its own suspicion timer. A Dead learned from gossip is
   downgraded to Suspect and then checked with a direct verify ping (R1b). The digest puts the
   receiver's entry first, then the sender's self entry, then piggybacked changes, then a rotating
   cursor (R4'). Tombstones expire and are probed within a budget (R5'). `Left` is written at
   `inc + 2^32` and only a Join from the subject lifts it (R6'). The relay keeps **no state**: the
   requester id and a splitmix token travel inside the Ping and its Ack.
3. **Lifeguard-SWIM.** Dead is a local opinion that requires this node's own failed probe. Join
   assigns the rejoining node's incarnation at the seed. The relay is a 16-slot table keyed per
   requester and answers with a `PingReqNack`. A Local Health Multiplier (LHM) stretches the
   timeouts of a node that looks isolated. Reprobes and `Config::seeds` drive rejoin.

## Evidence table

A claim counts only if it survived red-teaming in its rebutted form **and** was proven CORRECT by
executed C++. That C++ was built with g++ 15.2 and clang++ 21.1 on Linux bare metal, under
ASan+UBSan and TSan. Each prover first reproduced both defects on HEAD `ed4c19f`, using a
deterministic deferred-delivery fabric that never delivers in the sender's call stack.

### Design 1: SAR/ART (11 proven, 2 disproven)

| Claim | Survived? | Proven? | Number |
|---|---|---|---|
| C1 isolate-5 heal | yes | CORRECT | worst 10 ticks (1.2 s), 100/100 seeds; HEAD `*DDDD` 100/100; real chaos rig all-alive 0.6–0.9 s after heal |
| C2 split heal, incl. 40-tombstone graveyard | yes (revised) | **WRONG** | no graveyard: worst 11 ticks. With graveyard: **0/100 within 40 ticks**, worst 139 ticks (16.7 s). Fixed only by a 500 ms probe interval (worst 27 ticks, 20 seeds only) |
| C3 rejoin cluster-wide via Join | yes | CORRECT | worst 2 ticks; HEAD never rejoins (100/100) |
| C4 async relay, 1↔2 cut | yes | CORRECT | inc 1 vs HEAD 189 per 1000 ticks; real TCP 1 vs 87; chaos asym 1 vs 31 |
| C5 lossy relays p=0.01 | yes | CORRECT | 0 Dead in 20×10k ticks; worst inc 14 vs HEAD ≈1861 |
| C6 crash-detection parity | yes | CORRECT | difference from HEAD 0 on 50 seeds; 0 re-entries in 2000 ticks |
| C7 no stale resurrection / departed | yes | CORRECT | 0 violations (50 seeds g++, 500 seeds clang++) |
| C8 merge guard | yes | CORRECT | all fields unchanged; JoinReject=1 |
| F1 bounded state | yes | CORRECT | relay 1536 B; ≤64 tombstones at all 21,481 checkpoints; flood 32/968; +≤1 send per tick |
| F2 refutation bound incl. graveyard | yes | **WRONG** | 99/100 seeds show +1 after heal+40 (follows from C2) |
| S1 ASan/UBSan/TSan | yes | CORRECT | 0 reports; TCP reader thread 4.9–9.2 M reads, 0 regressions |
| S2 determinism | yes | CORRECT | identical send-hash on both runs and both compilers |
| C9 no regression + chaos rig | yes | CORRECT | all named tests pass; `run_swim_chaos.sh` all five scenarios all-alive |

### Design 2: FHT/TR, revised (13 proven, 2 disproven; **winner**)

| Claim | Survived? | Proven? | Number |
|---|---|---|---|
| C1 isolate-5 heal (drop + hold) | yes (revised) | CORRECT | worst **600 ms** against 1340 ms bound, 64 seeds × 4 modes; graveyard (44 tombstones) worst **960 ms** against 3480 ms; HEAD 0/64; real TCP 3-node heal 0.40 s |
| C2 split heal, n=5 and n=64 | yes (revised) | CORRECT | n=5 worst 480 ms, 0 false Dead, ≤1 bump. n=64 cap 8: 0 false Dead, worst 1440 ms. Ablations show the revisions are load-bearing: V=0 gives 37,607 false Dead, legacy digest 72,896; HEAD n=64 gives 21,760 |
| C3 async relay with latency sweep | yes (revised) | CORRECT | 0 bumps for d ∈ {0,10,25,29} ms; harness detects failure at d ≥ 31; HEAD 179–188 bumps; real TCP +0 vs +59 |
| C4 crash still detected | yes | CORRECT | 1920 runs, never_dead 0, worst 1188 ms (HEAD 1188 ms); the first-prober clause holds only in tick-quantized form (840 ms, same as HEAD) |
| C5 no resurrection while tombstone resident | yes (narrowed) | CORRECT | 0 violations over 495 ticks of replay; the race with a refutation to k+2 is held; self-leave propagates in 2 ticks |
| C6 rejoin after restart (both paths, `admit()`) | yes (revised) | CORRECT | 1 tick (bound 7); expired-seed path 3 ticks (bound 11); HEAD never |
| C7 merge guard | yes | CORRECT | Dead and Left tombstones unchanged; 1 JoinReject each |
| C8 no regression | yes | CORRECT | 208/208 on g++, clang++ and ASan; TSan 190/191 (pre-existing `tcp_transport_control_priority` flake, also fails on HEAD) |
| F1 zero relay state | yes (scoped) | **WRONG** (sub-claim) | behaviour exact (1000→1000→+1000, forged and late Acks 0, no div-by-zero), but sizeof is **784 B vs HEAD 752 B**, so the claim that it shrinks is false |
| F2 non-starving digest | yes (revised) | **WRONG** (sub-claim) | size bound and accusation-first hold (0 / 57,737 frames missing it); the ≤11-frame coverage gap holds in steady state (max 10) but reaches **282–303 sends under change bursts** |
| F3 bounded tombstones / per-tick work | yes (revised) | CORRECT | 0 violations over 22.5k ticks; max 11 frames per tick against formula 24; probe gap 2760 ms equals the bound exactly |
| F4 false positives under asymmetric cut | yes (corrected) | CORRECT | Suspect 3.34e-2 per probe against q^k 3.44e-2; bump ratio 1.68; **0 Dead in 320k periods**; HEAD 0.896 per probe. Note: the claim's prose figure "1.2e-3" is an arithmetic error |
| S1 TSan over real TcpTransport | yes | CORRECT | 3×60 s runs, ≈1.2e8 reads each, 0 warnings |
| S2 erasure UAF + wire range check | yes (revised) | CORRECT | 10^6-step fuzz under ASan+UBSan: 0 reports, recount always equal; status=7 entry rejected (HEAD admits it) |
| S3 determinism | yes | CORRECT | FNV identical across runs (n=5, n=64); `nm` shows only `clock_gettime` via `real_steady_ns` |

### Design 3: Lifeguard-SWIM (14 proven, 3 disproven incl. **2 safe**; disqualified)

| Claim | Survived? | Proven? | Number |
|---|---|---|---|
| C1–C9, F1, F2, F3, S2, S4 | yes (revised) | CORRECT | heal worst 2 rounds (160/160); 90 s split via seeds 10 rounds; relay 0 Suspect vs HEAD ≈180; crash 11 rounds (HEAD 10) |
| F3 mechanism (i): late-Ack revive is load-bearing | yes | **WRONG** | `late_ack_revives()==0`; the ablation is identical, so the rule is dead code |
| **S1** TSan-clean over TcpTransport | yes | **WRONG** | 3/11 Debug runs report races, all in TcpTransport `close_fd`-before-`del_fd` (pre-existing, HEAD 1/10). Not a SwimMembership race on its own, but the claim as stated fails |
| **S3** bounded tombstones, no phantom re-entry | yes (revised) | **WRONG** | 100 nodes with 80 crashed: **2766 phantom node-rounds**. Healthy-node LHM saturates at 8, so detection stretches to **1236 rounds**, past retention. Under crash churn ≈10k phantom live ids remain and retained count never reaches 0 |

## Decision

**Winner: Design 2, FHT/TR (expiring first-hand tombstones + accusation-targeted digest +
stateless token relay), in its revised form (R1b, R4', R5', R6', `admit()` through the Join rule,
wire-status range check, `indirect_ns()` clamp).**

Rationale, following the ranking rules:

1. **Safety gate.** Design 3 fails it. Its safe claim S3 is WRONG: under correlated failure the
   LHM saturates on *healthy* nodes because dead relays send no response. That stretches crash
   detection to 1236 rounds and lets erased entries come back from stale gossip, with 2766 phantom
   node-rounds. No cheap fix was stated, because the stretch is the LHM mechanism itself. It also
   bends a required invariant: a crashed node must be detected within
   ack + indirect + suspicion. Its S1 failure comes from a real but pre-existing TcpTransport race
   (see residual risks). Designs 1 and 2 have **no** safe claim marked WRONG.
2. **Proven beats claimed.** Design 2 has 13 proven and 2 disproven; Design 1 has 11 proven and
   2 disproven. More important is *what* was disproven. Both of Design 2's failures are
   fast-claim sub-clauses: sizeof +32 B, and the digest coverage gap during bursts. In both cases
   the behavioural property (accusation-first, zero relay state) was proven. Design 1's failure is
   a **correctness bound**. With a realistic graveyard of 40 tombstones, the {1,2}|{3,4,5} heal took
   121–139 ticks (≈16.7 s) instead of 40, because round-robin probing at one tombstone per second
   starves the cross-split probe. Its F2 failure follows from that.
3. **Measured protocol cost** (the task defines "fast" as bounded convergence, bounded false
   positives and bounded state). Design 2 heals faster in every shared scenario: isolate 600 ms vs
   1.2 s, split 480 ms vs 1.3 s, graveyard 1.08 s vs 16.7 s. It is the only design proven at
   **n=64**, with 0 false Dead, and the ablations show its R1b/R4' revisions are load-bearing. It
   has 0 Dead over 320k periods at p=5% loss (Design 1 was tested only at p=1%). It adds 0 B of
   relay state, against 1536 B, and its `sizeof` is 784 B against 2344 B. Crash detection matches
   HEAD in both. Design 1 is better on CPU per frame (+2.5–3.5% vs +18% at n=5 and +65% at n=64
   cap 8). This is control-plane cost (5 frames per node-tick in all builds), not the message hot
   path, and it has a cheap fix (condition 2 below).
4. **Core invariants.** Design 2 adds no atomics, no thread-visible state and no relay table. It
   keeps `view()`/`epoch()` as the mutex-published immutable snapshot and reads only `now_ns()`.
   Actor-level invariants (single executor, FIFO, stable placement) are untouched.

**Conditions before merge** (all cheap, all within the winning design):

1. **Fix the F2 burst starvation.** Reserve at least `ceil((max_gossip_updates-2)/2)` slots per
   frame for the rotating cursor, so piggybacked changes cannot take every slot. Re-run the
   `accuse` and `coverage` experiments, and require the coverage gap to stay within
   `ceil(|members|/reserved)` during bursts too.
2. **Remove the O(n log n) sort from every frame.** Build the cursor's sorted id vector only when
   `republish()` or a tombstone transition changes the roster, reusing its capacity. Re-run the
   per-frame bench and require n=64 cap 8 to be within +25% of HEAD.
3. **`tests/netem/run_swim_chaos.sh` acceptance gate.** Design 2 was proven only on a 3-node
   in-process TcpTransport rig. Design 1 ran the full 5-process network-namespace rig. All five
   scenarios (form, isolate, asym, split, flaky) must end all-alive on the patched
   `swim_chaos_node`. This is the **single tie-breaking experiment** if this decision is
   challenged: run both Design 1 (with a 500 ms probe interval) and Design 2 on that rig, with a
   40-tombstone graveyard pre-seeded.
4. Add the `MemberStatus::Left` case to `tests/netem/swim_chaos_node.cpp:80` (`-Wswitch`).
5. Land the prover harness (`DeferredFabric` with drop, hold, seeded shuffle and per-wave clock
   advance) and the HEAD-failing scenarios as `tests/cluster_swim_*_test.cpp`. Restate F1 (sizeof
   +32 B, from the new Config fields) and F4 (q^k ≈ 3.4e-2 at p=5%) correctly in the test comments.

Design 1 also contributed one finding the merge should keep: its real-rig evidence that
subject-origin frames plus a target-first digest heal TCP clusters in under 1 s. Design 2's R4
already covers the target-first digest.

## Residual risks

- **Resurrection after tombstone expiry or cap eviction.** C5 holds only while the tombstone is
  resident. A peer partitioned for longer than `tombstone_ttl_ns` (default 60 s) that holds a
  stale Alive can re-admit an erased node until it is detected again. `tombstone_ttl_ns` must be
  larger than the longest partition the deployment tolerates. This is an operational constraint;
  assert it at startup against `2*(ack+indirect+suspicion)`.
- **Mixed-version wire.** Wire status 3 (`Left`) is new, and the relay echoes `subject` and
  `subject_incarnation` in Ack. An old node treats Left as live, and an old subject leaves the
  relay inert. A rolling upgrade across this change needs a SWIM wire-version gate or a flag day.
  None of the three designs claimed mixed-version compatibility.
- **Left band.** Every leave/rejoin cycle adds 2^32 to the incarnation, so u64 allows about 2^32
  cycles per identity. That is safe, but visible in metrics (009).
- **Join, Left and resurrection trust `m.from`.** Without SecureTransport (020, ADR-040), a
  spoofed Join with `subject == from` re-admits a departed id. This is no weaker than HEAD.
- **Relay RTT budget.** The 4-hop relay chain must complete within the tick-quantized indirect
  window. The measured threshold is `4d < ceil(indirect_ns/T)*T`. On a WAN, raise
  `indirect_timeout_ns`.
- **Probe-backs to dead addresses.** At most B=2 per tick, sent until TTL. Over TcpTransport these
  cause dial and backoff churn and grow the per-peer `outq_`. That queue growth is already a
  022 concern (see ADR-046's PeerBacklog finding) and is outside SwimMembership's scope.
- **Pre-existing TcpTransport fd race.** `close_fd()` runs before `io_.del_fd()` at
  `tcp_transport.hpp:517-519` and `:625-626`, which races epoll_ctl against accept4 fd reuse. It
  was found by Design 3's prover and HEAD reproduces it (1/10). It is not part of this ADR, but it
  must be fixed and tracked separately (OpenQuestions 010/021).
- **Slower cluster-wide Dead in theory.** Because Dead is re-derived first-hand plus a verify
  ping, convergence can take up to one extra `suspicion_timeout` on nodes other than the detector.
  This was not observed: worst 1188 ms, identical to HEAD.
- **Determinism is per toolchain**, because `unordered_map` iteration order is part of the trace.
  The g++ and clang++ traces matched only because both used libstdc++.

## Spec recommendations

- **021-Cluster-Formation-and-Lifecycle.md**
  - §1 "Cluster identity": state the semantics that make "a node rejoining after a partition can
    tell stale from current" true. Dead is first-hand only. A Dead learned from gossip is a
    Suspect hint, checked by a verify ping. A tombstone is lifted only by an Alive or Suspect at a
    strictly higher incarnation, or by a Join where `subject == from`, which re-admits at
    `max(presented, tomb.inc+1)`. The seed returns the assigned incarnation in JoinAck.
  - §3: add a subsection "SWIM failure-detector rules (ADR-049)" listing R1/R1b/R2/R3/R4'/R5'/R6'/R7,
    the stateless token relay, and the new `Config` fields: `indirect_timeout_ns` (0 = ack),
    `tombstone_ttl_ns`=60 s, `max_tombstones`, `tombstone_probe_interval_ns`,
    `verify_ping_budget`, and the probe-back budget B.
  - §3 "Scale down": graceful leave writes authoritative `Left@inc+2^32`. Self-leave is never
    refuted. Only the subject's own Join re-admits.
  - §3 "Rolling upgrade": the SWIM wire format changes (status 3, relay echo fields). Mark this
    change as outside the back-compat window and require a version gate.
  - Open questions: add "tombstone TTL vs longest tolerated partition".
- **010-Distribution.md**, "Membership": the published view is the non-tombstone set
  (`Alive`/`Suspect`). `Dead` is adopted only from a node's own suspicion timer; `Left` is a
  separate authoritative departure. Indirect ping is asynchronous, with correlation carried in the
  frame and no relay state. Add a note that the relay round trip is 4 hops and must fit within
  `indirect_timeout_ns`. Cite ADR-049 under Status.
- **014-Testing-Model.md**, "Fault injection": require a **deferred-delivery transport** for any
  protocol test whose correctness depends on reply timing. It never delivers in the sender's call
  stack, and supports delivery in waves, seeded reorder, per-directed-link drop, a **hold** mode
  that queues frames and releases them FIFO at heal (modelling TcpTransport's `outq_`), and a
  per-wave clock advance for latency. Add the rule that a defect fix must first reproduce the
  defect on HEAD. Name `tests/netem/run_swim_chaos.sh` as the multi-process acceptance tier for
  membership changes.
- **018-Clocks-and-Deadlines.md**: every SWIM timer (suspicion, indirect window, relay-token
  bucket `now/indirect_ns`, tombstone TTL, probe-back spacing, verify-ping pacing) uses the
  injectable monotonic clock, never wall time. Detection and heal bounds are stated
  **tick-quantized**, with each stage rounded up to the tick (proven: 840 ms against a
  700 ms un-quantized bound, identical on HEAD). Rule 2a: only Acks to the node's own probes feed
  the smoothed RTT. Relayed and probe-back round trips must not.
- **OpenQuestions.md**, item 021: mark it resolved in mechanism by ADR-049, pending merge under
  the conditions above. Add the TcpTransport `close_fd`/`del_fd` race as a separate 010/021 entry.

## Merge record (2026-10-22)

Merged into `include/quark/core/cluster.hpp` from the Design 2 prover's patch, re-applied onto the
existing file. The existing doc comments were kept, and the prover-only scaffolding was dropped
(`QUARK_SWIM_PATCH` marker, the `piggyback_digest` ablation switch, extra diagnostic accessors). All
evidence below is from thnak-server: bare-metal Ubuntu 26.04, 28 cores, g++ 15.2, clang++ 21.1.

| Condition | How it was met | Evidence |
|---|---|---|
| 1. Burst starvation | `fill_digest` reserves `⌈(cap−2)/2⌉` slots for the rotating cursor | `accuse` burst coverage gap 282 → **20** (limit `⌈64/3⌉ = 22`); steady-state `coverage` 10 (limit 11) |
| 2. No per-frame sort | cursor walks a cached `sorted_ids_`, rebuilt only on add/erase; piggyback passes are skipped when `tx_pending_ == 0` | per-frame cost vs old code: n=64 cap 8 **+17%** (2004 vs 1713 ns; limit +25%; the prover's patch was +59%); n=5 +16% |
| 3. `run_swim_chaos.sh` | all 5 scenarios on 5 processes, one per network namespace | all end all-alive; isolate heals ≈0.9 s and split ≈1.0 s after the cut lifts; asym: 0 Suspect samples, incarnation stays 1 (was 31) |
| 4. `-Wswitch` in `swim_chaos_node` | `Left` case added | clean build |
| 5. Harness + failing-on-old-code tests | `tests/swim_deferred_fabric.hpp`, `cluster_swim_partition_heal_test`, `cluster_swim_async_relay_test` | old code: heal test fails 32/32 heal runs, relay test 363 bumps. New code: worst heal 600 ms, rejoin 120 ms, crash still detected cluster-wide in 1.08 s, 0 bumps |

Full suites on the merged code: g++ **210/210**, clang++ **210/210**, ASan+UBSan **209/210**, TSan
**192/193**. Both failures are pre-existing test weaknesses, unrelated to SWIM and recorded in
OpenQuestions.md:
- `topic_no_quiesce_control` (ASan negative control): its race fired in only 1/20 runs on this host.
- `tcp_transport_control_priority_test` under TSan: the ordering threshold is missed in 4/10 runs on the
  old code and 6–7/10 on the new. It is not a data race; the test does not use SwimMembership.
