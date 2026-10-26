# ADR-048 — RelayTransport: direct-vs-relay endpoint fallback policy

## Status

**Accepted.** Three designs were proposed, red-teamed, revised, and proven with real compiled,
sanitized, and benchmarked C++ (MSVC 19.51 + clang++ 22.1, ASan/UBSan; TSan unavailable on this
MSVC-ABI toolchain, substituted per `VERIFICATION.md`'s Windows exclusion policy with reentrancy
asserts + ASan+UBSan + run-to-run determinism checks). One design is disqualified by a proven
correctness violation of a MUST invariant. Of the two safe survivors, **Design 2 —
`DirectThenRelay<TimeoutMs>`, the wall-clock fallback-threshold policy — is selected.**

## Question

Today `TcpTransport` (`include/quark/net/tcp_transport.hpp`) requires a direct TCP path to every
peer: `add_peer(Endpoint)` teaches it one `NodeId → Endpoint` mapping, and a peer with no reachable
listener (behind NAT / a restrictive firewall) simply cannot be dialed — `send_on_loop` drops the
frame (010 dead-letter) if no endpoint resolves, and even when an endpoint *is* known, a
connect that never succeeds cycles forever through `St::Backoff` with jittered exponential retry
(`tcp_transport.hpp:61-62, 207-235`) — there is no terminal "direct is unreachable" signal anywhere
in the existing state machine.

Quark needs a third way for two nodes to exchange frames when neither can dial the other directly:
routing through an intermediary relay node's already-open connections to both, TURN-style. The
architecture is settled and was not re-litigated here (see prior discussion, retained below); the
only open crux this ADR settles is **the fallback-threshold policy** — given that direct-connect
failure has no terminal signal (just endless `Backoff`), what threshold decides "give up on direct,
dial the relay endpoint instead," how is that threshold expressed as a CRTP-style policy parameter
(matching `Sequential` / `Priority<P>` / `Placement<...>` / `DrainBudget<N>`, `policies.hpp`), and
what re-probing behavior (if any) recovers a peer back to direct once its NAT/firewall state
changes — without reordering or duplicating that peer's frames.

Settled architecture (not re-litigated):

- **Client side**: no new `Transport` implementation — a policy that resolves, per peer, which
  `Endpoint` to dial (the peer's own listener vs. a designated relay node's listener). Still plain
  `TcpTransport` underneath, reusing its existing dedup/backoff/framing machinery unchanged.
- **Relay side**: a thin forwarding hook on `on_receive()` — "if `frame.to != self`, `send(frame.to,
  frame)` instead of delivering locally" — decorator-shaped like `SecureTransport`
  (`include/quark/core/secure_transport.hpp`), not a parallel transport hierarchy. `MessageFrame`
  already carries `to: NodeId` (`transport.hpp:61-74`).

## Designs (one-line summaries)

1. **Count-based threshold — `DirectThenRelay<MaxAttempts>`.** Falls back after `MaxAttempts`
   consecutive `Backoff` cycles; a successful `Open` resets the counter. Revised to inject the
   policy at runtime via `configure_relay_fallback<P>()` (function-pointer extraction) rather than
   templating `TcpTransport` itself, and to replace a single global recovery watermark with a
   per-peer "chasing" watermark (`relay_enqueued`/`relay_drained`) that re-arms on every new send
   admitted through the still-open relay gate.
2. **Time-based threshold — `DirectThenRelay<TimeoutMs>`.** Falls back once `TimeoutMs` has elapsed
   since the peer's first `St::Connecting` with no `Open` in between — a one-shot `io_.post_after`
   timer is the authoritative backstop (Backoff re-entry is only an opportunistic early check),
   which is what catches a black-holed `connect()` that never errors out of `Connecting` at all.
   Recovery uses an out-of-band probe connection that proves direct reachability before a fenced,
   never-mid-frame cutover swaps the live `Conn`'s fd in place. `TcpTransport` itself is **not**
   templated — the policy tag resolves to one runtime `int64_t` field at construction, and relay
   configuration is two ordinary runtime calls (`set_relay`, an optional timeout constructor
   parameter).
3. **Hybrid attempt/time-capped threshold with throttled recovery —
   `DirectThenRelay<MaxAttempts, TimeoutMs, RecheckIntervalMs>`.** Relay kicks in at whichever bound
   fires first; recovery reuses the *same* Backoff/reconnect loop at a throttled cadence (no second
   connection lifecycle), and a `Draining` sub-state (gated by a per-peer `out_peer`-aware barrier
   that also inspects the relay `Conn`'s own in-flight output buffer, not just the not-yet-started
   queue) defers the direct cutover until every relay-hop frame — including one physically mid-send
   — has left the wire. `TcpTransport` is templated on the policy (`TcpTransport<FallbackPolicy>`),
   requiring a mechanical `TcpTransport<>` migration at four existing bare-member-declaration call
   sites (`samples/15_tcp_cluster`, `samples/17_tcp_cluster_mesh`, `samples/20_swim_membership_
   stability`, `tests/tcp_distribution_integration_test.cpp`).

## Non-goals (deferred to follow-on ADRs)

- Relay-hop backpressure semantics (hop-local vs. end-to-end congestion signalling through the
  relay).
- Relay-node discovery/advertisement mechanism (static config vs. SWIM capability gossip, ADR-045).
- Multi-relay / relay failover (what happens when the chosen relay node itself goes down).
- Security posture of the relay node itself (routing-metadata trust boundary; relay authentication
  beyond ordinary peer mTLS, ADR-040).

## Cross-examination summary

All three designs were red-teamed against the real `tcp_transport.hpp`/`pal/` source (not just their
own pseudocode). Every design conceded at least one fatal or serious finding and revised:

- **Design 1** conceded a fatal reordering hole in its original global recovery-watermark barrier,
  and a fatal `outq_` re-keying regression that broke `close_peer`'s SWIM-death dead-letter contract
  and turned one relay-connection failure into collateral loss for every co-routed peer. Both were
  redesigned (per-peer chasing watermark; tagged `QueuedFrame{to,bytes}` entries) before proving.
- **Design 2** conceded two fatal integration gaps against the *real* source: `do_reconnect()` was
  never actually wired to the relay-endpoint resolver (verified by reading `tcp_transport.hpp:576-
  608` directly — the original claim that this was "one indirection function" already patched in was
  false), so relay-routing silently and permanently reverted to a broken direct dial the first time
  the relay leg itself needed any ordinary reconnect; and `cutover_to_direct` re-registered an
  already-registered fd via `add_fd` without a preceding `del_fd`, which fails silently (`EEXIST`)
  on the real Linux/epoll `pal` backend (confirmed at `pal/linux_x86_64/net.hpp:305-312`), muting the
  recovered connection and busy-spinning the shared `io_` thread. Both were fixed (`resolve_dial_
  endpoint()` wired into both `dial()`/`ensure_conn()` and `do_reconnect()`; `del_fd` before `add_fd`
  in `cutover_to_direct`) and re-proven.
- **Design 3** conceded the same class-template/CTAD compile break at four real call sites, a fatal
  local reordering gap (the drain-before-cutover barrier only checked `outq_`, never the relay
  `Conn`'s own partially-sent `out` buffer), and a fatal `salvage_out` mis-filing bug once a relay
  `Conn` multiplexes several logical peers (`c->peer` is the relay's id, not the frame's destination,
  so a torn frame was silently, permanently orphaned). All three were fixed (`TcpTransport<>`
  migration disclosed; `out_peer`-aware barrier; `out_peer`-keyed `salvage_out`) and re-proven.

## Evidence table

Legend: **survived** = the claim (as revised) held up through cross-examination; **proven** = the
`quark-prover` role compiled, sanitized, and ran it; **number** = the measured result.

| Design | Claim | Survived red-team? | Proven? | Result / number |
|---|---|---|---|---|
| 1 — `DirectThenRelay<MaxAttempts>` | F1 mailbox/dispatch untouched | yes | CORRECT | mailbox_bench-analog p50 ≈30–32µs/batch identical with/without relay linked; zero relay symbols in disassembled hot-path body |
| 1 | F3 multi-peer relay contention (measured, not left unquantified) | yes | CORRECT | 8 peers: 63.9k vs 67.7k fps direct; 16 peers: 84.6k vs 67.8k fps → 19.9% contention delta |
| 1 | S1 no data race on new-peer fallback state | yes | CORRECT | 8/8 MSVC ASan + 3/3 clang ASan + 1/1 UBSan clean; reentrancy asserts never fired |
| 1 | S2 send() never blocks | yes | CORRECT | p50=100ns, p99=1.5µs, max well below loop-thread-busy window |
| 1 | C1 flips at exactly the Nth consecutive Backoff re-entry | yes | CORRECT | deterministic, 0/N-1 false-positive across both compilers |
| 1 | **C2r mid-flight relay→direct recovery: no reordering** | **no** | **WRONG** | **50% (MSVC) / 57% (clang) inversion rate across 30 runs each** — pre-flip "stranded" frame (queued to the direct hop's `outq_` in the window *before* `relay_routed` flips true) arrives after dozens of correctly relay-routed frames sent in between; no fix implemented or re-proven |
| 1 | C3 no duplication across hops | yes | CORRECT | 0 duplicate nonces, drops == 2×K exactly, deterministic |
| 1 | C4 close_peer dead-letters only that peer | yes | CORRECT | P's entries → 0, Q's 500 entries untouched, byte-for-byte |
| 2 — `DirectThenRelay<TimeoutMs>` | F1 mailbox/dispatch untouched | yes | CORRECT | disassembly of `send_on_loop`/`flush` shows zero calls to fallback functions; end-to-end send() cost 8.3–9.7µs/op both configs, policy variant not slower |
| 2 | F2 exactly 2 `post_after` timers per timing epoch | yes | CORRECT | 5 real Backoff cycles → exactly 2 fallback timer arms (not 5, not 52) |
| 2 | S1 no data race (incl. `set_relay()`, once loop-thread-marshaled) | yes | CORRECT | 5/5 ASan clean, 40,000 concurrent sends, zero drops/lost frames |
| 2 | S2 send() never blocks | yes | CORRECT | p50 6.2–6.7µs tracking NoFallback baseline within noise across 4 runs |
| 2 | C1 recovered connection delivers every frame exactly once (fd handoff fix) | yes | CORRECT | 5000/5000 frames delivered exactly once, in order, across 5 runs + 1 ASan run; underlying Linux/epoll EEXIST mechanism itself not directly reproducible on this Windows/WSAPoll box (disclosed, inconclusive on that narrow point) |
| 2 | C2 at most one active hop services a peer at any instant | yes | CORRECT | byte-exact ordered 5000-frame sequence — any interleaving would have shown gaps/dupes |
| 2 | C3 relay-routing durable across relay-leg churn (do_reconnect fix) | yes | CORRECT | relay killed/replaced mid-flight: 0 spurious re-fallbacks, 0 attempts to dial the peer directly, 201/201 frames posted |
| 3 — `DirectThenRelay<Max,Timeout,Recheck>` | F1 mailbox/dispatch untouched (real `bench/mailbox_bench.cpp`, unmodified) | yes | CORRECT | 96.9–100.0 vs 97.5–99.8 M msg/s/core; p50/p99/p999 bit-identical |
| 3 | F2 policy-present-but-inactive send() overhead | yes | CORRECT | 10480 vs 10157 ns/call, −3.1% (not a regression) |
| 3 | S1 no data race (incl. `set_relay()`) | yes | CORRECT | 5/5 clang ASan+UBSan clean, 125k sends |
| 3 | S2 send() never blocks | yes | CORRECT | p99.9 tracked NoFallback baseline within noise every run |
| 3 | C1 no reordering across relay→direct cutover (`out_peer`-aware barrier fix) | yes | CORRECT | 12 real-loopback trials × 250 frames, 0 inversions across 3000 frames |
| 3 | C2 no duplication | yes | CORRECT | 0 duplicate sequence numbers across all 12 trials |
| 3 | C3 throttled recovery cadence overrides exponential backoff | yes | CORRECT (directional) | 6 reconnects (policy, 250ms throttle) vs 4 (plain exponential) over the same 12s window — compressed margin, attributed to this box's ~2.0s WSAPoll connect-refusal floor dominating both |

**Totals: 21 claims survived red-team and were proven CORRECT; 1 claim (Design 1's C2r) survived
red-team as revised but was proven WRONG against a MUST invariant, with no fix implemented or
re-proven.**

## Decision

**Winner: Design 2 — `DirectThenRelay<TimeoutMs>`, the wall-clock fallback-threshold policy for
`TcpTransport`, with the fd-handoff and dial-target-resolution fixes applied.**

**Design 1 is disqualified under the safety/correctness gate.** C2r — "in-flight frames are not
reordered across a mid-flight relay→direct recovery" — is exactly one of this ADR's MUST invariants
("If the design ever transitions a peer from relay back to direct mid-flight, in-flight frames must
not be reordered or duplicated relative to peer FIFO, 017-Delivery-Guarantees"). It was proven WRONG
with a reproducible 50–57% inversion rate, not a rare race: any frame enqueued for a peer in the
window between the fallback threshold firing and the relay route actually taking effect is queued to
the *direct* hop (because `route_hop` is evaluated once, at enqueue time, against the not-yet-flipped
`relay_routed` flag) and can arrive after dozens of subsequently, correctly relay-routed frames. The
prover identified the root cause (no migration of already-queued direct-hop frames on a fallback
flip) but no fix was designed, implemented, or re-proven within this round. Per the judging rubric,
a proven-WRONG correct-class claim disqualifies the design from winning unless a stated *and proven*
cheap fix exists — none does here, so Design 1 does not win regardless of its other 7 correct proofs
(including the genuinely strong F3 multi-peer contention measurement no other design produced).

**Designs 2 and 3 both fully survived**: every claim proven CORRECT, zero WRONG verdicts, real ASan
(+UBSan via clang) clean across all concurrency stress runs, TSan's absence handled per this
project's documented Windows/MSVC sanitizer-exclusion policy (reentrancy asserts + real clang
ASan+UBSan + cross-run determinism as the substitute, matching `VERIFICATION.md`'s stated posture for
this toolchain). Between them:

1. **Architectural fit.** Design 2 keeps `TcpTransport` **exactly the class it is today** — no
   template parameter, zero existing call sites touched. Relay behavior is injected via one optional
   constructor argument (a policy tag resolved once to a plain `int64_t` field, `DrainBudget<N>`-
   style) plus an ordinary runtime `set_relay()` call, marshaled through the same `started_`-gated
   `io_.post()` idiom `add_peer()` already uses. This is the most literal realization of this ADR's
   own settled premise — "still plain `TcpTransport` underneath" — of the three designs. Design 3
   templates `TcpTransport<FallbackPolicy>` and, even with CTAD covering constructor call sites,
   requires a disclosed, mechanical edit to 4 existing files (`samples/15_tcp_cluster`, `samples/
   17_tcp_cluster_mesh`, `samples/20_swim_membership_stability`, `tests/tcp_distribution_integration_
   test.cpp`) that otherwise fail to compile. A bigger, unforced footprint change is a reason to
   prefer the alternative when both are otherwise proven equally safe and correct (rubric rule 4).
2. **Hot-path evidence quality.** Design 3's F1 evidence is genuinely the stronger *artifact* — it
   runs the project's own real, unmodified `bench/mailbox_bench.cpp` (the actual 023 budget-gate
   instrument) rather than a hand-built analog, and its numbers (96.9–100.0 vs 97.5–99.8 M msg/s/core,
   bit-identical latency percentiles) are the cleanest zero-overhead demonstration produced by any
   design in this round. Design 2's F1 instead relies on a custom smoke test plus `dumpbin`
   disassembly (also conclusive, but a less direct proxy for the actual mailbox/dispatch gate).
   Weighed against point 1, this does not overturn the decision — the F1 zero-overhead property is
   binary (either the hot path calls fallback code, or it doesn't; both designs prove it doesn't) —
   but it is recorded here as a residual quality gap in Design 2's own proof artifacts, not a
   correctness gap in the design.
3. **Direct-connect-hang coverage.** Design 2's give-up decision is a one-shot `io_.post_after` timer
   armed at first `Connecting`, independent of whether `Backoff` is ever re-entered — this is what
   catches a black-holed `connect()` that never errors out of `Connecting` at all (Design 2's C3,
   proven: relay-leg churn does not cause the peer's own dead-letter regression). Design 3's `MaxAttempts`-
   or-`TimeoutMs` hybrid also has a timeout backstop and is not worse here, but Design 2's threshold
   is the simpler, single-purpose mechanism for exactly the "no terminal signal" problem this ADR
   opened with.

Both are viable, fully-proven implementations; Design 2 is chosen as the one that most faithfully
keeps to the settled "no new Transport subclass, plain `TcpTransport` underneath" architecture with
the smallest source-compatibility footprint, while matching Design 3 claim-for-claim on proven
safety and correctness.

### What is actually adopted

- `include/quark/net/tcp_transport.hpp` gains: an optional `fallback_timeout_ns` constructor
  parameter (default `0` = feature off, byte-identical to today); `void set_relay(NodeId, Endpoint)`
  marshaled through the existing `started_`-gated `io_.post()` idiom; per-`Conn` fields
  `first_connecting_ns`, `fallback_epoch`; a `relay_routed_` set; a `probes_` map for out-of-band
  recovery probing; `resolve_dial_endpoint()` as the **sole** dial-target lookup, wired into **both**
  `ensure_conn()`/`dial()` and `do_reconnect()` (the fix for the confirmed relay-leg-churn wedge);
  `check_relay_timeout()` as the one-shot give-up backstop; `cutover_to_direct()` with `io_.del_fd()`
  called before `io_.add_fd()` on the recovered fd (the fix for the confirmed silent-mute/busy-spin
  bug on the real epoll `pal` backend).
- `include/quark/net/fallback_policy.hpp` (new): `NoRelayFallback` (default, zero-cost) and
  `DirectThenRelay<TimeoutMs>` compile-time tags, resolved once at construction to a runtime
  `int64_t`, matching the `DrainBudget<N>`-style "compile-time tag → resolved runtime field with
  zero cost when unused" idiom already used elsewhere in `policies.hpp`.
- No existing call site changes required — `net::TcpTransport transport;` (bare member declarations,
  constructor-initializer forms, all of it) continues to compile unchanged across the full sample/
  test corpus.

## Residual risks

- **C1's underlying Linux/epoll `EEXIST`-on-double-`add_fd` mechanism was not directly reproduced on
  this box.** This dev environment has only the Windows/WSAPoll `pal` backend available (`pal/
  windows_x86_64/net.hpp`'s `add_fd` unconditionally overwrites `handlers_[fd]`, masking the bug
  class entirely), so the fix (`del_fd` before `add_fd`) is verified by source inspection of `pal/
  linux_x86_64/net.hpp:305-312` plus full end-to-end functional proof (5000/5000 frames delivered
  correctly through a real cutover) rather than by a sanitizer catching the specific `epoll_ctl`
  failure path on Linux. **Tie-breaking experiment**: run the same `c1c2c3_relay_recover` scenario on
  a Linux CI leg with the epoll `pal` backend and assert `io_.add_fd()`'s return value is checked (or
  instrument `epoll_ctl` call counts) to directly witness the `EEXIST`-avoidance the fix provides.
- **Design 2's C1/C2/C3 proofs, and Design 3's, both ran on Windows/WSAPoll only** — no TSan leg was
  available anywhere in this proof round (stated toolchain constraint). The ASan+UBSan (via clang,
  real sanitizer, not MSVC's ASan-only) plus reentrancy-assert plus cross-run-determinism combination
  is the strongest substitute obtained, consistent with `VERIFICATION.md`'s documented Windows
  exclusions, but a genuine happens-before violation that never manifests as a memory error and is
  deterministic across ~20 runs would still be undetected. A Linux+TSan CI leg for this specific
  scenario (concurrent `send()` + `set_relay()` + fallback/recovery churn) is recommended follow-up.
- **`DirectThenRelay<TimeoutMs>`'s single timeout parameter double-duty**: the same constant governs
  both the give-up threshold and, indirectly (via `arm_direct_probe`'s re-arm-on-`TimeoutMs` cadence),
  the direct re-probe interval, with no backoff/jitter of its own on the probe retries. A permanently
  unreachable-but-not-SWIM-dead peer burns one extra `tcp_connect` attempt per `TimeoutMs` indefinitely.
  Acceptable at second-scale timeouts; a follow-on `DirectThenRelay<TimeoutMs, ProbeIntervalMs =
  TimeoutMs>` is a compatible future extension, not required now.
- **Relay-node health is out of scope by design** (per this ADR's non-goals) and remains silently
  entangled: if the relay itself is unreachable while a peer is relay-routed, that peer's frames
  dead-end at the (also forever-retrying, now-durable-per-C3) relay `Conn` with no distinct signal
  beyond the existing `drops_`/`reconnects_` counters. An operator cannot currently distinguish "relay
  is down" from "peer is down" from diagnostics alone.
- **Proactive abandonment of a still-possibly-succeeding `Connecting` attempt at the timeout deadline
  is a new way to trigger 010's existing at-most-once loss contract** (today's forever-retry never
  gives up, so it never discards an attempt that might have completed a moment later). This is within
  the existing delivery-guarantee contract, not a new violation of it, but is a behavioral change
  worth naming in 017's text (see spec recommendations).
- **Design 1's per-peer chasing-watermark mechanism (C2r's own local invariant) was never falsified**
  — only the end-to-end no-reordering property was. If a future round wants to revisit Design 1 (e.g.
  for its measured multi-peer contention advantages), the fix path is disclosed in the cross-
  examination record: `route_hop` must be re-consulted (or already-queued direct-hop entries must be
  migrated) at the instant `relay_routed` flips, not only at original enqueue time.

## Spec recommendations

- **`010-Distribution.md`**: Add a subsection documenting the direct↔relay endpoint-fallback policy
  as a `TcpTransport`-level, non-`Transport`-subclassing mechanism: `NoRelayFallback` (default) and
  `DirectThenRelay<TimeoutMs>`, resolved to a runtime `int64_t` at construction. State explicitly that
  relay routing changes *which Endpoint* a peer's `Conn` dials, never `MessageFrame::to`, `outq_`'s
  per-logical-peer keying, or the SWIM dead-letter contract on `close_peer` — the frame's addressing
  and per-peer FIFO ordering are untouched by the routing decision. Cross-reference this ADR for the
  proven fd-handoff (`del_fd` before `add_fd` on cutover) and dial-target-resolution (single
  `resolve_dial_endpoint()` call site shared by `dial()`/`ensure_conn()` and `do_reconnect()`)
  invariants any future relay-adjacent change must preserve.
- **`017-Delivery-Guarantees.md`**: Add an explicit statement that a relay-routed peer's FIFO
  ordering guarantee is preserved across a recovery-triggered cutover from relay back to direct — no
  reordering, no duplication — and name the mechanism class (a fenced cutover that never splices
  mid-frame, deferred until the in-flight relay-hop frame has fully left the wire) as the required
  shape for any implementation, since a naive "flip on next `Open`" implementation (Design 1's
  original approach) was proven to reorder. Also document that a proactive give-up (abandoning a
  still-`Connecting` direct attempt at a wall-clock deadline to redirect to relay) is a new, disclosed
  trigger of the existing at-most-once loss contract, not an exception to it.
- **`021-Cluster-Formation-and-Lifecycle.md`**: Note that relay-node `Endpoint` acquisition
  (static config today; SWIM capability-gossip piggyback per ADR-045 as a follow-on) and relay-node
  health/failover are explicitly deferred non-goals of the fallback-threshold policy decided here —
  cross-reference this ADR so a future relay-discovery ADR does not need to re-litigate the
  fallback-threshold mechanism itself, only how a peer's designated relay `Endpoint` is learned/
  updated at runtime via the already-proven, loop-thread-marshaled `set_relay()`.
- **`019-Platform-Abstraction-Layer.md`**: Record the concrete backend asymmetry this ADR's C1 fix
  depends on: `IoContext::add_fd` on the Linux/epoll backend fails (`EEXIST`, silently, return value
  discarded by a careless caller) when called on an fd already registered in that epoll instance,
  while the Windows/WSAPoll backend's `add_fd` unconditionally overwrites the stored handler and
  masks the same bug class. Either (a) document this as a caller obligation — always `del_fd` before
  re-`add_fd`-ing a live fd, as this ADR's `cutover_to_direct` now does — or (b) file a follow-on PAL
  hardening item to make `add_fd` idempotent/replace-semantics on both backends so the two `pal`
  implementations cannot silently diverge in behavior for the same caller mistake. Given 019's own
  parity contract ("unchanged between the Linux and Windows backends"), option (b) is preferable long
  term; this ADR does not implement it, only surfaces the asymmetry it uncovered.

## Amendment (issue #14): two relay shapes, reconciled; relay side built

The ADR's "thin forwarding hook" and `tcp_transport.hpp`'s byte-transparent per-peer endpoint were two
different designs. Both are kept, named explicitly:

- **Shape A — transparent endpoint** (`set_relay`): an external TURN-style allocation answering with the
  peer's own HELLO. Client-side redial only; producing the allocation stays out of scope.
- **Shape B — node-side forwarding** (`set_relay_forwarding` + `set_relay_via`): an ordinary cluster node
  forwards inbound frames with `to != self` unchanged (an end-to-end SecureTransport seal is never
  opened). Spokes address the destination through an ORDERED relay list (failover = first Open entry).

Relay authentication (previously a non-goal): forwarding is opt-in and fail-closed. The node supplies
`admit(origin, dest)` (wire to membership / the NodeAuthority-admitted set); a frame whose `from` differs
from the connection's identified peer, an unknown destination, or a failed `admit` is refused and counted
(`relay_refused()`). A forwarded frame never takes a second hop. Known limit: the connection identity is
the HELLO claim at this layer, so the strength of the gate is the strength of `admit`. Still deferred:
relay discovery, hop-local backpressure, ordering across a relay<->direct switch. Test:
`tests/tcp_relay_forwarding_test.cpp`.
