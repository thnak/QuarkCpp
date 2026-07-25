# ADR-030: UDP Datagram Channel for Real-Time Voice/Media + Relay-Node Placement

## Status

Accepted

## Question

Design a NEW sibling to the `Transport` seam (010) — not a decorator of it and not a new
`Transport` implementation — that carries best-effort, unordered, unreliable UDP datagrams
(real-time voice/media) between player nodes and relay/distribution nodes living in the SAME
Quark cluster. `Transport`/`MessageFrame` and the actor mailbox path (`activation.hpp`
`GovernanceCore`) must stay byte-for-byte unchanged and zero-cost when a deployment doesn't use
voice. The design had to concretely resolve: (1) socket/loop ownership, (2) addressing/discovery
via 021 gossip + 025 capability placement, (3) the delivery contract and developer-facing API
surface, (4) connection/resource reuse at scale composing with 026's `Gateway`/
`BoundedPartialView`, and (5) an explicit security decision.

## Designs debated

1. **VoiceChannel — Shared-Loop, Capability-Placed (reuse-maximalist).** UDP fd registered on
   the SAME `pal::IoContext` `TcpTransport` already drives (one additive `event_loop()`
   accessor, zero other transport.hpp/tcp_transport.hpp diff). Relay eligibility = `Flag{"voice-relay"}`
   gossiped capability; room→relay = top-K rendezvous (`VirtualBins` generalized) over the
   eligible subset, recomputed deterministically by every node. Relay↔relay mesh reuses 026's
   `BoundedPartialView` verbatim over the small eligible-relay roster. `send()`/receive are
   raw, allocation-free function calls / synchronous callbacks — never `tell`, never
   GovernanceCore. Security: AEAD sealed with a sub-key HKDF-derived from the existing 020
   `SecureTransport` session key; sequence number doubles as replay/staleness guard.

2. **VoiceChannel — Dedicated UDP I/O thread + epoch-fenced `VoiceFastHandle` (isolation-maximalist).**
   A second, dedicated `pal::IoContext`/OS thread isolates voice fd readiness from TCP
   control-plane epoll batches. Relay reassignment is CAS-fenced through a per-session
   `VoiceFastHandle` (epoch + relay slot) written by a `Stateless<N>` control pool, read on the
   hot path. Security: DTLS-lite — HKDF sub-key off the 020 handshake, per-datagram AEAD with a
   sliding replay window and a generation-based grace window for in-flight stragglers across a
   relay handoff.

3. **VoiceBins — Static Relay-Pinned, Pure PAL, Zero Actor Involvement.** A standalone header
   sharing no symbol with transport.hpp/tcp_transport.hpp/activation.hpp at all. Dedicated UDP
   I/O thread; room→relay resolved by a `VirtualBins`-shaped table built only over the
   `Flag{"voice-relay"}` subset; relay learns player addresses on first receive. Security
   explicitly out of scope for v1 — cluster-perimeter trust only, loud `SecurityMode::Strict`
   rejection.

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| F1 zero-alloc send/fan-out | 1 (Shared-Loop) | yes (revised) | **CORRECT** | 0 allocations across ~1M send() calls, 4 threads; relay fan-out to 31 members also 0-alloc after a found-and-fixed unconditional heap-copy bug |
| F2r bounded control-plane latency under shared loop | 1 | yes | **CORRECT** | budgeted design: p99 0.2–1.1ms flat across room size 8→1000; negative control (unbounded) **starves the control fd completely** at P≥64 — proves the budget is load-bearing |
| F3r fan-out stays on loop thread, no `io_.post()` | 1 | yes | **CORRECT** | 0 thread violations across P up to 1000 (>9000 chained continuations) |
| S1 nonce uniqueness under RouteTable churn | 1 | yes | **CORRECT** | 0 duplicate nonces across 145k–366k concurrent sealed frames, TSan-clean |
| S2r replay/reorder correctness across relay-set rebuild | 1 | yes (revised: crypto state split into a stable PeerSessionTable) | **CORRECT** | exact adversarial delivery order verified: 4/4 correct deliveries, 4/4 correct rejects, survives 8 interleaved RouteTable rebuilds |
| S3r/S4 relay-side maps single-writer, bounded | 1 | yes (revised: sweep moved onto loop thread via `post_after`) | **CORRECT** | 0 thread violations under concurrent churn; table empties to 0 after idle timeout in a controlled test |
| C1–C6 determinism, convergence, O(rooms·K) bound, tcp_transport.hpp boundary, relay-ratio validation, token-gated authorization | 1 | yes | **CORRECT** (all 6) | 0 mismatches over 100k RoomIds; 0 empty routes after churn; peer-state flat at ≤3 across N=100→10,000; disassembly byte-identical outside voice code; ratio-gate rejects 60%-flagged cluster, accepts 2%; token-possession cleanly gates room membership |
| S1r epoch-CAS "prev = 2nd-highest" | 2 (Dedicated-thread) | yes (narrowed) | **WRONG** | core no-regression property (a) holds 20,000/20,000; the *stronger*, separately-claimed property (b) fails ~50% of trials (9981/20000) — no stated fix, no reworded claim offered |
| S3 instance-id fixes cross-session leak | 2 | yes | CORRECT | 0/1,000,000 stale deliveries after adding instance_id |
| S4 NodeId-keyed fresh lookup fixes slot-cache misdirection | 2 | yes | CORRECT | fixed: 0/10,000 misdirections; old cached-slot design reproduced at 261/10,000 (2.61%) in the same harness |
| C1 grace-window / supersession semantics | 2 | yes | CORRECT | all 5 sub-scenarios pass |
| F1/F2 zero-alloc, mailbox untouched | 2 | yes | CORRECT | 0 allocations/1M round trips; mailbox-tap delta 0; object code byte-identical when voice unused |
| F3 dedicated loop shields voice from TCP jitter | 2 | — | **INCONCLUSIVE** | could not clear the claimed ≥5x bound under the mandated ≤4-core cap; host noise dominated |
| F4 SO_REUSEPORT affinity / goodput scaling | 2 | split | affinity **CORRECT**, goodput-scaling **INCONCLUSIVE** | 0/10,000 cross-queue events; goodput flat across K=1..3 (sender-bound artifact of the core cap) |
| S2/C2 replay window, zero new wire tags | 2 | yes | CORRECT | 1/1000 replay accepted (correct), 0/100,000 tag-fuzz false-accepts; observed control tags ⊆ existing SWIM tag set |
| F1 relay_for() O(1)/N-independent | 3 (VoiceBins) | yes | CORRECT | p50 2.3–2.9ns flat across N=512→8192 |
| F2r zero-alloc send/receive hot path | 3 | yes (narrowed) | **WRONG** | 28 allocations/1.1M events overall; isolated test (TTL-sweep re-arm excluded) shows the actual `send_frame`/`drain_readable` path is 0-alloc — allocations traced to the borrowed `IoContext`'s once-per-second timer re-arm, not per-packet, but the claim as literally stated (and as scoped in the design's own dataStructure section, "no queue... no allocation") is falsified |
| F3 dedicated loop vs. shared | 3 | yes | CORRECT | p99 5177–5199µs (shared) → 94–283µs (dedicated), 97–98% reduction |
| S1r construction-ordering / no-lock relay maps | 3 | yes (revised: private `start()`/`stop()` sequencing) | CORRECT | found+fixed a real unsynchronized `sink_` race via TSan; clean after fix across 6 runs, ~1.2M events |
| S2 route-table snapshot UAF-free | 3 | yes | CORRECT | ≥10M combined ops, 0 TSan/ASan reports across 4 sanitizer/compiler combos |
| C1 no misdelivery to an uncomputed node across relay-set transition | 3 | yes | CORRECT | 0 mismatches across 6 runs, ~220–260k datagrams/run under live relay churn |
| C2 session-load balance CoV ≤ 0.2 | 3 | yes | **WRONG** | CoV 0.28–0.29 at the exact 16×max_relays operating point — reproduces an *already-documented* ADR-006/ADR-011 information-theoretic floor at N==max_nodes; a stated fix (oversample to 64×) measured CoV 0.14 in the same binary |
| A2 capability-gossiped addressing (no `TcpTransport::peers_` reuse) | 3 | yes (revised after conceding the original design's private-member/data-race bug) | CORRECT | 0 functional references to tcp_transport.hpp; 2M combined ops, 0 TSan/ASan reports |

**Totals: Design 1 — 13/13 claims proven CORRECT, 0 WRONG, 0 INCONCLUSIVE. Design 2 — 8 CORRECT,
1 WRONG (S1r, safety-class, no stated fix), 2 INCONCLUSIVE. Design 3 — 6 CORRECT, 2 WRONG (F2r,
C2 — both with a demonstrated/stated cheap fix in the same evidence run).**

## Decision

**Winner: Design 1 — "VoiceChannel — Shared-Loop, Capability-Placed UDP Datagram Channel"
(reuse-maximalist).**

Rationale, against the stated ranking:

1. **Safety gate.** Design 1 is the only one of the three with zero WRONG findings across its
   entire final claim set (13/13 proven CORRECT after cross-examination revisions). Design 2's
   `S1r` — a `safe`-class claim about its own headline mechanism (the epoch-CAS fence) — was
   proven WRONG as literally stated, and no reworded claim or concrete cheap fix was offered in
   the evidence; per the ranking's explicit gate, this disqualifies Design 2 from winning
   outright, independent of its otherwise-strong 8 proven claims. (Note for the record: the
   *load-bearing* half of that property — no epoch regression is ever externally observable —
   did hold at 20,000/20,000 trials; only the stronger, non-load-bearing "prev = second-highest
   of all racers" formulation failed. This tempers how alarming the disqualification is, but the
   rule as given does not carve out an exception for it.) Design 3 also has two WRONG findings
   (`F2r`, `C2`), but both come with a stated, cheap, in-evidence fix (isolate/replace the
   borrowed `IoContext` timer's per-second node allocation for `F2r`; retune the bucket-count
   multiplier from 16× to 64× for `C2`, mirroring 026's own documented practice) — so Design 3
   survives the gate, but only barely, and with a visibly patched design rather than a clean one.
2. **Proven beats claimed.** Design 1 has the largest fully-proven claim set (13) with the
   fewest asterisks: no INCONCLUSIVE results, and every fix proposed during cross-examination
   (instance-scoped crypto state, loop-thread-marshaled sweep, budgeted fan-out) was re-verified
   clean under TSan/ASan across multiple compilers and multiple runs, with three *real* bugs
   (a UAF/dangling-`this` lifetime bug, a condvar unlock-then-notify race, and a libstdc++
   `std::function` SBO/allocation regression) caught and fixed during the proof pass itself —
   evidence the proof process was adversarial against the implementation, not just the paper
   design.
3. **Reuse of `pal::IoContext` + `NodeId`/`Endpoint` + 025 capability placement + 026 topology.**
   Design 1 is explicitly the reuse-maximalist entry and it is proven to deliver on that: `C4`
   shows the *only* diff to `tcp_transport.hpp` is one additive accessor (disassembly of
   `TcpTransport`'s existing methods is byte-identical before/after, `activation.hpp` untouched);
   `C1`/`C2` show room→relay placement is a pure function of gossiped `Flag{"voice-relay"}`
   capability content via a generalized `VirtualBins`; relay↔relay mesh reuses 026's
   `BoundedPartialView` verbatim over the small eligible-relay roster (not the full cluster);
   `C5` closes the one real gap the red team found (no startup gate on relay-fraction R vs. N) by
   adding an 008-style validation, exactly mirroring 026's own `B ≥ 16·max_nodes` precedent.
   Designs 2 and 3 both introduce a *second* dedicated `IoContext`/OS thread instead of reusing
   the one `TcpTransport` already drives — a real, measured latency-isolation win (Design 3's
   `F3`: 97–98% p99 reduction) but a strictly larger resource/complexity footprint and a
   deliberate rejection of the reuse goal this exercise asked designs to be judged on.
4. **Core invariants / mailbox routing.** All three designs correctly keep voice off
   `GovernanceCore`/mailbox; none is disqualified on this axis. Design 1's `F2` (renamed here for
   clarity — its own C4) is the strongest proof of this: real mailbox-push counters showed a
   literal 0 delta, and object code for a non-voice build is byte-identical.

**On hot-path numbers**: Design 1's shared-loop budgeting (`F2r`) keeps control-plane dispatch
p99 in the 0.2–1.1ms band even at 1000-member rooms, and its negative control proves the budget
is doing real work (the unbudgeted variant fully starves the control fd at P≥64) — this is the
concrete, measured answer to the brief's "defend the choice against TCP control-plane jitter"
requirement, not an assertion. It does not match Design 3's dedicated-loop isolation numbers
(94–283µs vs. Design 1's 0.2–1.1ms) — that is a real, honest trade-off of the shared-loop choice
— but Design 1 wins on the combination of *zero disqualifying safety findings* + *cleanest
measured reuse of the four named subsystems*, which the ranking explicitly weights above raw
isolation-latency superiority once a design has passed the safety gate.

## Residual risks

- **Shared-loop tail latency under adversarial room sizes.** The budgeted-fan-out fix (`F2r`)
  bounds p99 to ~1.1ms at 1000-member rooms in the measured environment; this has not been
  proven at cluster sizes or room sizes beyond what was tested, nor against a genuinely hostile
  (not just large) workload. If a deployment needs sub-millisecond SWIM responsiveness under
  worst-case voice load, the shared-loop choice should be re-benchmarked at that target's actual
  scale before shipping.
- **`Aead::seal_into`/`open_into` (fixed-buffer, non-allocating) does not exist in `aead.hpp`
  today**, nor does `SecureTransport` derive a real per-peer session key (it takes one
  externally-constructed, effectively-shared `Aead` in the current dev/mock posture). `F1`'s
  zero-allocation proof and the entire point-5 security narrative are only meaningful once both
  land; this is disclosed, not silently assumed, but it is real follow-on work, not yet-built
  infrastructure.
- **Cross-room authorization is possession-of-token, not a richer ACL** (`C6`). A RoomId token
  leak (e.g. logged, or forwarded by a compromised legitimate member) grants full read/write
  access to that room's voice stream to any admitted cluster node holding it. This is an
  explicit, tested v1 model, not a gap — but operators should know the security boundary is
  "cluster-admitted + knows the token," not per-member revocation.
- **Relay-fraction discipline (`C5`) is enforced at node startup, not continuously.** An operator
  who reconfigures capability flags at runtime to push R toward N after startup is not caught
  until the next restart/validation pass; this should be tightened to a live-reload check if
  hot capability reconfiguration is ever supported.
- **The K>1 relay-replication path (named future work in the original design) has no dedup
  story.** V1 ships K=1 only; multi-relay redundancy for a room needs its own design before use.
- **Design 2's disqualifying `S1r` finding is narrow and possibly over-strict** (the actually
  load-bearing no-regression property held at 20,000/20,000). If voice/relay isolation ever
  becomes a hard requirement (e.g. a deployment target where SWIM p99 under voice load cannot
  tolerate the shared-loop's ~1ms budget), Design 2's dedicated-thread architecture — with `S1r`
  reworded to only claim the property that actually matters and re-verified — is the natural
  fallback to re-open, not Design 3 (which had two independent WRONG findings of its own, one of
  them, `C2`, inherent to the `VirtualBins`-style construction at low bin-multiplier and thus
  likely to recur in any bins-shaped redesign).
- **No experiment in this debate tested real network conditions** (packet loss, reordering,
  jitter typical of actual internet paths for player-to-relay traffic) — all proofs ran over
  loopback UDP. Given the workload is explicitly latency- and loss-tolerant by contract, this is
  a lower-priority gap, but the specific numeric latency claims (p50/p99 in the low-millisecond
  range) should be re-validated on a real WAN path before being used in a capacity-planning SLA.

## Spec update recommendations

- **New spec file: `028-Voice-Datagram-Channel.md`.** This is a large enough, independently
  testable subsystem (own header, own PAL surface, own security model, own placement rule) to
  warrant its own numbered spec rather than a subsection bolted onto 010. It should document:
  the `VoiceChannel` class and its `event_loop()`-sharing constructor contract; the wire format
  (`RoomId`/seq/from header, AEAD framing); the `PeerSessionTable` vs. `RouteTable` split (crypto
  state scoped to session lifetime, topology state rebuilt freely on capability-view churn — this
  split is the fix for the S2r nonce-reuse-across-relay-churn bug found in cross-examination and
  must be called out explicitly as a MUST, not an implementation detail); the budgeted-drain /
  budgeted-fan-out discipline (`kMaxDatagramsPerWakeup`, `kMaxFanoutPerDatagram`) as a required
  invariant, not a tuning suggestion, given `F2r`'s negative control showed the unbudgeted
  version fully starves the shared loop; the token-possession authorization model (`C6`) stated
  as the explicit v1 security boundary; and the 008-style relay-ratio startup validation (`C5`).
- **`010-Distribution.md`**: add a short cross-reference section ("Sibling seams") noting that
  `VoiceChannel` (028) exists alongside `Transport`, shares no code path with it, and is not
  reachable through `MessageFrame`/`GovernanceCore` — this closes the "silence is not
  acceptable" risk of a reader assuming voice traffic is just another `Transport` frame kind.
  State explicitly that `TcpTransport::event_loop()` is the one sanctioned extension point for
  sibling seams that need to share the node's I/O reactor, and that no other public surface
  should be added to `TcpTransport` for this purpose.
- **`019-Platform-Abstraction-Layer.md`**: document the new additive PAL primitives
  (`udp_socket`/`udp_bind`/`udp_send_to`/`udp_recv_from` and the fixed-buffer `Aead::seal_into`/
  `open_into` overloads) under the existing "one place touches OS APIs" rule, and flag
  `Aead::seal_into`/`open_into` as not-yet-implemented follow-on work required before `VoiceChannel`
  ships (per the residual risk above) — the spec should not claim this exists until it does.
- **`021-Cluster-Formation-and-Lifecycle.md`**: add a note under capability gossip that
  `Flag{"voice-relay"}` (+ optional `Scalar{"voice-udp-port"}`/weight scalars) is a first
  real consumer of the capability-gossip mechanism for a non-mailbox subsystem, and document the
  "mid-session handoff across a rejoin-only capability change" behavior explicitly: eligible-set
  changes are picked up on the next `on_capability_view_changed` rebuild, in-flight datagrams to
  the old relay are lost by the ordinary best-effort contract (bounded by one gossip-convergence
  round), no fencing/PathPin is applied because voice has no ordering guarantee to protect.
- **`025-Placement-Policies-and-Stateless-Workers.md`**: add `VoiceChannel`'s top-K rendezvous
  over a capability-filtered eligible subset as a second worked example of the "constrained
  placement resolves against a per-eligibility-class bin table" pattern (next to whatever 025
  already uses this pattern for), explicitly cross-referencing 028 rather than duplicating the
  algorithm.
- **`026-Large-Scale-Cluster-Topology.md`**: add a worked example showing `BoundedPartialView`
  constructed over a small `std::span<const NodeId>` subset (the eligible-relay roster) rather
  than the full cluster roster, to make explicit that this is a supported, tested usage pattern
  and not a novel extension — and add the relay-ratio startup validation (`C5`) as a named
  instance of the existing "topology knobs are validated at startup" rule (alongside the existing
  `B ≥ 16·max_nodes` example), since C2's WRONG finding on Design 3 showed this exact class of
  bin-table construction has a real, previously-undocumented CoV floor at low bin-multiplier that
  future bins-shaped designs (voice or otherwise) will hit again if not called out.
