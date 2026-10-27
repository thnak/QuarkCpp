# ADR-053 — Membership events + per-node health (#21); SWIM / TCP-hello wire-version window N, N-1 (#20)

Status: Accepted (in-process evidence; the 5-process netns run is NOT exercised here - see section 5).
Evidence: `tests/cluster_membership_events_test.cpp`, `tests/cluster_swim_wire_version_test.cpp`,
`tests/tcp_hello_version_test.cpp`. Specs touched: 021 (Rolling upgrade), 010 (Membership).

## 0. Issue claims, checked
- #21 "Membership exposes only a node-id list and an epoch": **true** (`membership.hpp` before this
  change: `view()`/`placement_view()`/`epoch()`, no events, no metadata).
- #20 "cluster.hpp has no version field": **true** (control codec had none; first byte is the kind).
  "TCP hello rejects anything not exactly equal": **true** (`parse_hello`: `version != hello_version`
  -> drop, silently). Nuance the issue missed: the hello DID already carry a u16 version field, so only
  its acceptance rule needed to change, not its layout.
  "spec 021 says the format is versioned": the spec text overclaimed; it was an open question.
These were verified by reading the code at the cited lines; no failing test was written against the
old code (the new tests would not compile against it - the API they exercise did not exist).

## 1. Events and health (#21)
- `membership.hpp` gains `MembershipEvent{kind,node,incarnation,epoch,protocol_version,at_ns,refusal,
  refused_by_peer}`, kinds `Joined, Suspect, Recovered, Dead, Left, VersionRefused`, `NodeHealth`, and
  two **defaulted** virtuals on `Membership` (`add_event_sink`, `health`) so third-party memberships
  still compile. `MemberStatus` moved here from `cluster.hpp` (same namespace; no source change).
- `SwimMembership` queues events while state mutates and drains them at the end of every public entry
  point (`tick`, inbound frame, `leave`, `admit`). **No new thread.** Callbacks therefore never see
  half-mutated state, may call back in (re-entrant calls queue and drain in order - tested), and run
  on the driving thread, which is the existing single-thread contract.
- Semantics: `Joined` fires when a node enters the **verified live** set (= `placement_view()`), not
  for unverified gossip ghosts (F12); `Suspect` only for a joined node; `Recovered` = Suspect -> Alive
  (an addition beyond the four in the issue: it is what ends a "reconnecting" window); `Dead`/`Left`
  only for a node that was announced. `epoch` is the roster epoch right after the transition (Suspect
  does not shrink the view, so it does not bump it). Tombstone expiry / cap eviction / unverified
  reaping emit nothing.
- **Latency bound.** Subscriber-side: 0 ticks - the callback runs inside the call that changed state
  (asserted: callback-time clock == event `at_ns`). Remote crash (virtual clock, n nodes, period P,
  ack A, indirect window I, suspicion S): Suspect <= (n-1)P + A + I + 2P; Dead <= that + S + P.
  Measured n=4,P=120ms,A=I=100ms,S=500ms: Suspect +360 ms (bound 800), Dead +960 ms (bound 1420).
  Graceful Left propagates to a peer within 5 periods on a 3-node loopback. These bounds describe
  the loopback/deferred-fabric model; on a real network add transit time (the transport RTT).
- `health(node)`: state, incarnation, last wire version heard **directly** from the node (0 = never;
  `admit()` alone does not teach it), `verified`, sticky `last_refusal` + the version it presented.
  Driving-thread-only, like `status_of`.
- `InProcessMembership` implements the contract for `Joined`/`Left` only (it is a test double).

## 2. SWIM wire version (#20)
- `ControlMsg::wire_version` (default 1). **v1 = the existing byte layout, no trailer. v2 = v1 + one
  trailing version byte.** The old decoder ignores trailing bytes, so v2 -> v1 is byte-compatible; an
  absent trailer decodes as v1 (tested: v1 is a strict prefix of v2). `kSwimWireVersion = 2`,
  `kSwimWireWindow = 1`, `Config::wire_version` (default current; for rolling upgrades and tests).
- **Window: accept [self-1, self+1], refuse the rest.** N+1 is accepted deliberately: during a roll the
  N-1 nodes must accept the first N node, and the rule is symmetric. This is wider than the issue's
  literal "accept N and N-1" and is the only reading under which a rolling upgrade works.
- **Refusal** happens before any state is touched: no gossip absorbed, no liveness evidence. The peer
  is counted (`version_refusals()`), reported once per (peer,version) as `VersionRefused` with
  `WireVersionTooOld`/`TooNew` + the peer's version, and kept in `health().last_refusal`. A refused
  `Join` is answered with `JoinReject` whose `seq` carries the reason; `JoinReject` is exempt from the
  gate so the joiner can learn why (event with `refused_by_peer=true`). Otherwise a refused peer
  behaves as a partitioned one (the cluster eventually marks it Dead) - the reason stays observable
  after the tombstone. Tested: a refused node's Suspect/Dead gossip never reaches the cluster.
- **TCP hello**: layout and value (1) unchanged; `parse_hello` now returns a verdict, window
  `hello_version_window = 1`. Refusals call `set_hello_refusal_handler` (IO thread) and bump
  `hello_version_refusals()`; bad magic stays silent. `SwimMembership::note_transport_refusal` is the
  thread-safe bridge (queued; surfaces as an event on the next tick/frame, <= 1 period).
  `set_hello_version` exists for tests / upgrades.

## 3. Rejected
- A new event-dispatch thread or async queue: violates "reuse existing seams"; synchronous drain is
  enough and keeps the ordering guarantee.
- Dispatching inside `apply_update`: re-entrancy into a half-mutated member table.
- Version negotiation in-band (pick the lowest common): the refusal window is simpler and testable;
  nothing here needs two versions' semantics negotiated at runtime.
- Bumping `hello_version` to 2 now: it would make every deployed node reject new nodes (they compare
  for equality). The value stays 1; the first real bump is the first one the window helps.

## 4. Caveats (honest)
- **Pre-gate nodes are indistinguishable from v1.** A deployed node without this code still compares
  the hello version exactly and has no SWIM trailer logic: it interoperates with v1/v2 on SWIM (it
  ignores the trailer) but ADR-049's Left/relay incompatibility with pre-049 nodes is unchanged - that
  roll still needs a flag-day. The gate protects the *next* change, not past ones.
- The window only protects changes an N-1 node can parse. A format change it cannot parse must bump
  the version and shrink the window by ADR; nothing enforces that discipline mechanically.
- A refusal is one-sided information: the refused node sees only silence on SWIM (plus a JoinReject
  for joins, plus its own `TooNew` event when it can decode the peer's frame). Version-mismatched
  hellos are refused on BOTH sides, which is why both report.
- `Joined` is not emitted for self, nor for unverified ghosts; a consumer wanting every known id
  should use `health_snapshot()`.
- Event `protocol_version` for `Joined` born from `admit()` is 0 until the node is heard from.
- `at_ns` and the bounds use the injected clock; real-network latency is not measured here.

## 5. What was exercised / not
- Exercised: codec compat; a 5-node mixed N/N-1 cluster on the deterministic deferred fabric across
  form, isolate and split (drop and hold, 4 shuffle seeds each), asym (indirect ping through mixed
  relays), crash detection and graceful leave, for the real versions (2/1) and a synthetic 4/3, with
  zero refusals; N-2 refusal incl. reason/event/health/JoinReject/no-poisoning and the exact window
  boundary for self in {3,5} vs peer 1..7; real-socket hello at versions 5/4/3.
- **Not exercised:** `tests/netem/run_swim_chaos.sh` itself (Linux netns + sudo + tc; this host is
  Windows). The script and `swim_chaos_node` gained an optional per-node wire version
  (`WIRE_VERSIONS="2 1 2 1 2" tests/netem/run_swim_chaos.sh <build>`), syntax-checked only, never run.
  No real-process 5-node mixed run, no real packet loss/latency. Not run under TSan/ASan here.
