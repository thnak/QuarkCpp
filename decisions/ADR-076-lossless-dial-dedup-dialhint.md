# ADR-076 — Lossless simultaneous-dial dedup: the DialHint settle handshake (019, 021; issue #65)

Status: **Proposed.** Evidence below is from a 28-core Linux host (GCC 15.2) running the real
`net::TcpTransport` over loopback; the transport-only mesh test, sample 17 (one process, 4-node mesh) and
sample 18 (4/8 separate OS processes) are the gates. Spec text: [021 §2 "Dial deduplication"](../021-Cluster-Formation-and-Lifecycle.md).

## 1. Decision

021 §2 already says the dedup loser is *"dropped before any application frame flows"*, but the transport
did not enforce it: each side confirmed its own outbound as soon as the peer's HELLO arrived, before the
rival inbound was identified, so both sides could put application frames on the wire and one of them was
then lost. Issue #65 measured this as 1–N message partial-lane loss on 4+ node meshes and a flaky
`FIFO=BROKEN` gate.

This ADR adds a **DialHint settle handshake** on the plaintext HELLO wire, plus one protection rule:

1. **The acceptor sends one `DialHint` frame** immediately after its HELLO and before any application
   frame. It reports its current view of the pair in one flags byte:
   `has_outbound` (I am also dialing you), `has_established` (I already hold a confirmed connection),
   `established_has_data` (that connection has carried application data), `established_aged` (it has been
   confirmed past the ADR-071 race window).
2. **A v2 dialer does not confirm until the hint arrives** (bounded by `dial_settle_timeout_ns`, default
   500 ms). Its application frames wait in the per-peer `outq_`; the loser never writes an application
   byte, so there is nothing to recover or replay.
3. **A connection that has carried application data in either direction is never closed by a dedup
   race.** The evidence is `app_data_sent || app_data_received`; the acceptor reports it, and the dialer
   applies the same rule from the hint. An aged resident connection is the ADR-071 reconnect case and is
   still superseded.
4. **A confirmed resident connection beats an unconfirmed newcomer** in a race. The lower-NodeId election
   now only decides when neither connection is confirmed (or the resident is an fd-less Backoff husk,
   which is dropped first).

Wire compatibility: `hello_version` becomes **2** with the same 14-byte layout. The ADR-053 ±1 window
accepts v1↔v2 both ways; a v2 transport never sends a `DialHint` to a peer that advertised v1, and a v1
peer keeps the old confirm-on-HELLO behavior (documented at-most-once). The hint is a transport-internal
`FrameKind::DialHint` (4), consumed in `TcpTransport::do_read` and never delivered to the 010 router or a
relay. The anonymous/pinned wire sends no HELLO, so it never emits or accepts a hint; it keeps ADR-072's
authenticated-supersede behavior (see §6).

No second connection slot, no retransmission, no message sequence numbers: the hint rides the connection
like HELLO, and `flush()` writes HELLO → hint → frames in that order. The steady-state send path is
unchanged apart from one `out_is_data` marker per loaded frame.

Two implementation details matter for safety:

- The hint is **recorded** inside the receive loop but the settle decision is **posted to the loop**, never
  acted on inside `FrameStream::feed`. Acting synchronously re-enters the transport: a rejected dial parks
  via `schedule_reconnect()`, which clears the same `Conn::in` stream `feed()` is iterating (ASan caught this
  as a libstdc++ vector-bounds abort in the young-reconnect test).
- A **rejected accepted-side connection is closed once its hint is in the kernel** (a short grace if the
  write would block), instead of being held until the 5 s slowloris pending-hello timeout. Holding it made
  every rejected dial look like a multi-second stall and delayed convergence under a heavy simultaneous
  start.

## 2. Rules

| Situation (acceptor's view when it identifies the initiator's connection) | Hint | Dialer's decision |
|---|---|---|
| no resident connection | `0` | confirm |
| resident is an unconfirmed outbound (simultaneous dial) | `has_outbound` | confirm iff the **initiator** is the lower NodeId; otherwise wait for the rival inbound (the acceptor's outbound) |
| resident is confirmed, no application data, not aged | `has_established` | do not confirm; park this dial (the peer already has a connection) |
| resident is confirmed and data-bearing, not aged | `has_established \| established_has_data` | do not confirm; park this dial |
| resident is confirmed and aged (ADR-071 reconnect) | `... \| established_aged` | confirm (the old socket is presumed dead; its at-most-once reconnect contract is unchanged) |

The accept side applies the same rules locally (it knows its own resident state and the initiator's id),
so both ends agree without a second round trip. A parked dial calls `backoff_unconfirmed()`: the fd is
retired, the peer queue survives, and the normal jittered reconnect retries later.

## 3. Evidence

All on the 28-core host, Release `-O2`/`-O3`, real loopback sockets:

- **2-node simultaneous dial**: `tcp_transport_dial_dedup_test` now repeats the race **50 times** and
  asserts the burst itself (5 frames each way) arrives exactly once, in order — 50/50 rounds raced and
  passed on every run. The old test explicitly did *not* assert the race burst (`:51-53`).
- **Transport-only 4-node mesh** (`tests/tcp_transport_dial_race_mesh_test.cpp`, 6 pairs dialing at
  once): 6000/6000 per node at 2000 frames/lane, `ordered=yes`, `drops=0`, 0 settle timeouts.
- **Sample 17 (one process, 4-node mesh)**: N=150 was 10/10 OK; at **N=2000** all four nodes delivered
  **8000/8000**, `missing=0`, `out_of_order=0` (previously ~5100/8000).
- **Sample 18 (separate processes)**: 4×150, 4×2000, 4×20000, 8×10000 and **8×100000** all OK with
  `missing=0`, `peer_sheds=0`, `drops=0`.
- **Fallbacks** (`tests/tcp_transport_dial_settle_test.cpp`): a v1 peer is confirmed immediately with no
  hint sent/awaited; a v2 peer with the hint suppressed confirms on the 50 ms settle timeout
  (`settle_timeouts ≥ 1`) and still delivers every frame.
- Existing `tcp_transport_loopback_test` (400/400), `tcp_transport_reconnect_test` and the 2-node samples
  15/16 are unchanged.

## 4. A separate cause found while reproducing the issue (not dedup)

At N > 1024 frames per lane, sample 17/18 were also losing ~`N - 1024` frames per sender with
`transport_rx` equal to the full count: the **F15 per-peer inbound token bucket**
(`DistributedRouter`, default 4096 frames/s / burst 1024) shed the surplus. That is a deliberate overload
guard, not the dial race, and it was always present. The samples are losslessness gates, so they now call
`set_peer_budget(0, 0)` (with a comment that production should keep the guard or raise it to its measured
data-plane rate). The transport-level tests do not use the router and therefore never hit it. Issue #65's
high-burst loss numbers were largely this bucket; the dedup-specific failure is the 1–N message partial
lane loss / `FIFO=BROKEN` at small N, which this ADR's handshake fixes.

## 5. Why not the alternatives

- **Data-bearing rule alone**: both sides can confirm and send in the sub-ms race before either sees the
  rival, so both connections become data-bearing and one must still be closed. The hint prevents either
  from sending before the election is settled.
- **Two-connection-slot rework + gossiped-endpoint dial-back** (the seam the old code comment named):
  correct but much larger, and unnecessary once no application byte is ever written on the loser.
- **A fixed settle grace on both sides**: probabilistic; a delayed rival beyond the grace still loses data,
  and a long grace is first-send latency for every connection.
- **Per-message sequence numbers + replay**: turns the transport into an at-least-once protocol and
  duplicates work 017/051 already own above it.

## 6. Residual risks

- **First-send latency**: a fresh v2 connection's first application frame waits for the acceptor's hint,
  about one half-RTT (the hint follows HELLO). It is setup-only, bounded by `dial_settle_timeout_ns`, and
  zero on the steady path.
- **v1 peers** keep the documented at-most-once behavior (no hint capability). Mixed-version clusters are
  lossless only on the v2 side.
- **Pinned/anonymous wire**: no HELLO, no hint. ADR-072's handshake-proven identity and authenticated
  supersede are unchanged. Extending the same settle to the pinned wire needs a handshake capability bit
  and is a follow-up (not required by the samples or by issue #65's plaintext reproduction).
- **A data-bearing connection still loses to an ADR-071 aged reconnect**: if the old socket is actually
  alive when the peer dials a new one after the 1 s window, its in-flight frames are at-most-once, exactly
  the pre-existing reconnect contract. The new rule protects a *race*, not a declared reconnect.
