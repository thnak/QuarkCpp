# 028 — Voice Datagram Channel

How **best-effort, unordered, unreliable UDP** (real-time voice/media) reaches and
leaves a Quark cluster without touching the `Transport` seam
([010](010-Distribution.md)) or the actor mailbox
([003](003-Memory.md)/[activation.hpp](include/quark/core/activation.hpp)
`GovernanceCore`). This is a NEW sibling seam, not a variant of an existing one:
`Transport` promises ordered, FIFO-per-sender delivery ([010](010-Distribution.md));
voice wants the opposite, and paying mailbox dispatch overhead per audio packet is
the wrong cost model for a stream where dropping a stale packet is *correct*
behavior, not a failure to recover from.

> **Design pinned by [ADR-030](decisions/ADR-030-voice-udp-datagram-channel.md)** —
> `VoiceChannel`, the shared-loop, capability-placed design, the only one of three
> competing designs with zero disproven claims after cross-examination (13/13
> proven CORRECT under GCC 14.2 + Clang 20.1, ASan/UBSan/TSan). It shares
> `TcpTransport`'s already-open `pal::IoContext` epoll reactor via one additive
> accessor instead of standing up a second thread/loop/fd-set, and places relay
> nodes via the same capability-gossip + `VirtualBins` machinery [025](025-Placement-Policies-and-Stateless-Workers.md)/[026](026-Large-Scale-Cluster-Topology.md)
> already use for actor placement.
>
> **Status: Draft.** The shared-loop datagram path, the `PeerSessionTable`/
> `RouteTable` split, and the budgeted-drain/budgeted-fan-out discipline are
> proven. **Two items block promotion to Accepted**: `Aead::seal_into`/`open_into`
> is implemented only against `MockCipher` (**020**'s "honest exception" — no
> production AEAD is wired yet, see [019](019-Platform-Abstraction-Layer.md));
> and no experiment has run over a real network path (loss/reorder/jitter) —
> every proof to date is loopback UDP. See *Open questions*.

## The problem: voice is not a `Transport` frame kind

The obvious implementation — add a `WireMode`/`FrameKind` for voice and route it
through `Transport`/`MessageFrame` — breaks two assumptions the discrete-message
path exists to guarantee:

1. **Ordering nobody wants.** `Transport` guarantees FIFO-per-(sender,receiver)
   over one multiplexed TCP connection ([010](010-Distribution.md)). A voice
   packet that arrives late is worthless — retransmitting or re-ordering it just
   adds latency to a stream that has already moved on. Forcing it through an
   ordered pipe pays for a guarantee the workload actively does not want.
2. **Mailbox dispatch nobody wants to pay for.** Every `MessageFrame` decodes to an
   actor delivery, which means a descriptor, a mailbox enqueue, and
   `GovernanceCore` bound/overflow accounting per frame (003).  At voice packet
   rates (tens per second per speaker, fanned out to a room) that overhead is
   pure waste — and worse, overload shedding on a *mailbox* is tuned for message
   loss being a correctness event (022), whereas dropping a stale voice packet
   under load is exactly the desired behavior.

So voice is modeled as a **new, parallel seam** — `VoiceChannel` — that shares
infrastructure with `Transport` (the I/O reactor, node/capability addressing) but
none of its delivery machinery.

## VoiceChannel

One `VoiceChannel` per node, constructed over the **same** `pal::IoContext`
`TcpTransport` already drives:

```cpp
net::TcpTransport transport(self, bind_addr, bind_port);
transport.start();
net::VoiceChannel voice(transport.event_loop(), self, cipher);
voice.bind(bind_addr, voice_port);
```

`TcpTransport::event_loop()` is the **one sanctioned extension point** for a
sibling seam that needs the node's I/O reactor — see *Sibling seams* in
[010](010-Distribution.md). It is a single additive accessor; every other
`TcpTransport`/`tcp_transport.hpp` byte is untouched (ADR-030 claim C4:
disassembly of `TcpTransport`'s existing methods is identical before/after).

```
player node                          relay node                        player node
┌───────────────┐   UDP datagram    ┌───────────────┐   UDP datagram   ┌───────────────┐
│ VoiceChannel A │ ────────────────▶│ VoiceChannel R │────────────────▶│ VoiceChannel B │
│  send(room, .) │                  │  fan-out       │                 │ on_datagram(.) │
└───────────────┘                   └───────────────┘                  └───────────────┘
        ▲                                    ▲
        │ shares reactor                     │ shares reactor
        ▼                                    ▼
┌───────────────┐                  ┌───────────────┐
│  TcpTransport  │                  │  TcpTransport  │   ← Transport/MessageFrame/GovernanceCore:
│  (unmodified)  │                  │  (unmodified)  │      byte-for-byte untouched, zero-cost if unused
└───────────────┘                  └───────────────┘
```

### Wire format

A 32-byte header, sent as AEAD associated data, followed by `{ciphertext||tag}`:

```
| room (8) | seq (8) | via (8) | origin (8) | AEAD(ciphertext || tag) |
```

- **`room`** — the `RoomId` (modeled as `ActorId{kRoomTypeKey, room.value}` so it
  rides `VirtualBins` verbatim — no new placement machinery).
- **`seq`** — a per-`(via, receiver)` monotone sequence number; the replay/staleness
  guard (see *Security*).
- **`via`** — the datagram's **physical, immediate** sender. This is what the
  nonce, the `PeerSession`, and the replay/sequence state are scoped to — the
  relay↔listener channel the AEAD nonce is actually derived over.
- **`origin`** — the **logical speaker**. Preserved unchanged across a relay hop
  and handed to `on_datagram`'s callback, so a listener always learns who is
  really talking, not "the relay." A player originating a fresh frame sets
  `origin == via == self`; a relay re-sealing a fanned-out frame sets `via = self`
  (itself — the physical resend) but leaves `origin` exactly as it received it.

  **This split is load-bearing, not cosmetic**: collapsing `via`/`origin` into one
  field (as ADR-030's proven build originally did) makes every relayed frame
  misattribute its speaker to the relay — found and fixed during implementation,
  after the debate's proof pass (the ADR's 13 evidence claims don't cover
  speaker-attribution-through-relay, since none of them exercise `on_datagram`'s
  reported identity across a fan-out hop). Any future change to this header
  **MUST** keep the two fields distinct.

### Delivery contract

Best-effort, unordered, no retransmit — stated explicitly, not left implicit:

- `send()` never blocks and never queues. A kernel send-buffer-full result
  (`would_block()`) is counted (`drop_wb()`) and the datagram is gone — never
  retried.
- Out-of-order or duplicate arrival is rejected by the sequence guard
  (`drop_stale()`), not reordered or buffered for later delivery.
- There is **no** ack, retransmit, or delivery confirmation of any kind.
- Oversize payloads (`> kVoiceMaxPayload`, MTU minus header minus tag budget) are
  dropped locally before ever reaching the socket (`drop_oversize()`).

### Developer surface

```cpp
class VoiceChannel {
public:
    using DatagramCb = std::function<void(RoomId, NodeId origin, std::span<const std::byte>)>;

    VoiceChannel(pal::IoContext& loop, NodeId self, const Aead& cipher);
    bool bind(std::uint64_t addr, std::uint16_t port);
    void on_datagram(DatagramCb cb);
    void on_capability_view_changed(const CapabilityView& view);
    PeerSession* ensure_peer_session(NodeId peer, VoicePeer addr);
    void send(RoomId room, std::span<const std::byte> payload) noexcept;
};
```

Per-node, not per-actor: a `VoiceChannel` is constructed once alongside a node's
`TcpTransport` and shared by whatever session/room bookkeeping the application
layer builds on top (e.g. a lightweight per-room actor that calls `send()` — the
actor owns *session lifecycle*, never the datagram hot path itself). `send()`/the
`on_datagram` callback are raw, allocation-free function calls — never `tell`,
never routed through a mailbox.

## Addressing and placement (021, 025, 026)

A relay node advertises `Flag{"voice-relay"}` in its gossiped capability set
(021 §Discovery / 025 Part A) — the first non-mailbox consumer of capability
gossip. Every node computes the same eligible-relay subset from the same gossiped
content and builds a `VirtualBins` table restricted to it:

```cpp
std::vector<NodeId> eligible;
for (NodeId n : view.nodes())
    if (view.capabilities_of(n).has_flag("voice-relay")) eligible.push_back(n);
VirtualBins routes(eligible, virtual_bin_count(eligible.size()));
```

- **Room → relay** is `routes.owner_of(room_actor_id(room))` — an O(1),
  N-independent lookup (026), restricted to the eligible subset rather than the
  full cluster roster. This is the same worked pattern 025 already uses for
  `Require`/`Prefer` placement against a per-eligibility-class bin table; 028 is
  a second instance of it, not new machinery.
- **`RouteTable` vs `PeerSessionTable`** (ADR-030 claim S2r, a MUST): the
  `VirtualBins` snapshot (`routes_`) is **topology-only** state, rebuilt freely
  and cheaply on every capability-view change. `PeerSession` (crypto key
  material, `send_seq`, `recv_high`) is **session-scoped** state, keyed by
  `NodeId`, created once and never touched by a route rebuild. Mixing the two —
  keying crypto/replay state off the freely-rebuilt topology snapshot —
  reintroduces nonce reuse across a relay-set rebuild; this is the exact defect
  the debate's red-team round found and the fix that survived re-proof.
- **v1 replication is `K = 1`**: one relay per room. `RouteTable` degenerates to
  "the live `VirtualBins` snapshot restricted to the eligible subset" with no
  separate per-room map needed at all. `K > 1` is documented future work with no
  dedup story yet (see *Open questions*).
- **Relay-fraction startup validation (claim C5)**: mirrors 026's own
  `B ≥ 16·max_nodes` precedent — a node refuses to start if the advertised
  relay-eligible fraction `R` is too close to `N` (default threshold
  `max(8, ceil(sqrt(N)))`), catching a misconfiguration that would otherwise
  silently degrade `BoundedPartialView`'s relay-mesh bound from O(log R) toward
  O(log N) only at runtime:

  ```cpp
  bool ok = voice_relay_ratio_ok(relay_count, total_nodes);  // threshold==0 -> the default above
  ```

- **Relay↔relay mesh** (K > 1 future work) reuses 026's `BoundedPartialView`
  **verbatim**, constructed over the small eligible-relay roster
  (`std::span<const NodeId>` restricted to `Flag{"voice-relay"}` nodes) rather
  than the full cluster — a supported, tested usage pattern (026), not a novel
  extension.
- **Mid-session handoff across a capability change**: node capabilities are
  static for a node's lifetime — a change is a rejoin (021), never a live
  mutation. When a relay's eligibility changes (it rejoins without the flag, or
  a new relay joins), every node's next `on_capability_view_changed` rebuild
  picks up the new eligible set on its own schedule. In-flight datagrams
  addressed to the old relay are lost under the ordinary best-effort contract,
  bounded by one gossip-convergence round. **No fencing or `PathPin`
  ([026](026-Large-Scale-Cluster-Topology.md) §"Cross-node FIFO under relay") is
  applied** — voice has no ordering guarantee to protect, so there is nothing
  for a fencing mechanism to buy.

## Shared-loop discipline (budgeted, not unbounded)

Sharing `TcpTransport`'s reactor is the reuse this design is chosen for (ADR-030
§3), but it means voice traffic and SWIM/control-plane dispatch now compete for
the same thread. Two budgets make that safe — **required invariants, not tuning
suggestions**:

- **`kMaxDatagramsPerWakeup`** bounds how many datagrams `on_readable()` drains
  synchronously per epoll wakeup. Once exhausted, the remainder is served via a
  bounded, chained `IoContext::post()` continuation — never an unbounded
  synchronous drain.
- **`kMaxFanoutPerDatagram`** bounds how many room members a single received
  datagram fans out to inline. A room at or under the budget fans out entirely
  off the stack (`plain` array) with zero heap allocation; only the overflow tail
  of an oversized room needs a chained continuation, which is where — and only
  where — a heap copy is made.

ADR-030's negative control proved these are load-bearing, not defensive
over-engineering: the **unbudgeted** variant of this design fully starved the
shared loop's other fds (SWIM/control-plane) at room size ≥ 64. With the budget,
p99 control-plane dispatch latency stayed in the 0.2–1.1ms band flat across room
sizes 8 → 1000 in the measured environment (see *Open questions* for the scaling
caveat).

## Lifetime (shared loop, per-session channel)

A `VoiceChannel`'s `State` is heap-allocated and `shared_ptr`-owned
(`std::enable_shared_from_this`). Every self-rescheduling or chained
continuation (`idle_sweep`, `on_readable`'s budget-overflow repost,
`fanout_continuation`) captures a `std::weak_ptr<State>`, **never** a raw `this`.
This is not defensive style — it is required by the shared-loop design itself:
`TcpTransport`'s reactor deliberately outlives any one `VoiceChannel` that might
be torn down mid-session, so a continuation that captured a raw pointer would
fire against freed memory once its `VoiceChannel` is destroyed. This was found as
a real use-after-free under TSan during ADR-030's proof pass, not designed in
from the start — treat the weak_ptr discipline as load-bearing for any change to
this file.

`VoiceChannel::~VoiceChannel()` synchronously removes its fd from the shared
loop's handler registry before returning (bounded wait, with a documented
timeout fallback if the loop was already stopped first) — this is what makes the
`on_readable` fd-handler's *own* raw `State*` capture safe (see the source
comment at `bind()`): by the time the destructor returns, the reactor can never
again look up that fd's handler.

## Security (v1 boundary — a stated decision, not silence)

AEAD-sealed with the wire format above as associated data (so tampering the
header — including the room, sequence, or either identity field — invalidates
the tag exactly like tampering the ciphertext). `Aead::seal_into`/`open_into`
(019, additive, fixed-buffer/non-allocating overloads of the existing `Aead` seam
from 020) are the only crypto surface `VoiceChannel` calls.

**v1's security model is token-possession, not a richer ACL**: any cluster node
that has been admitted to the cluster and holds the room's key material can
fully participate (send and receive) in that room's voice stream. There is no
per-member revocation — a leaked room token grants access until token rotation.
This mirrors 020's "honest exception": `MockCipher` is a keyed XOR
keystream + tag, **not real cryptography**, used only to prove the *framing* is
correct (round-trips, tamper detection). A production deployment needs a real
AEAD adapter (mbedTLS/BoringSSL AES-GCM or ChaCha20-Poly1305, per 019/020) wired
in before this ships live traffic — that adapter does not exist yet (see *Open
questions*).

## Non-goals

- **Reliability, ordering, or retransmission of any kind.** These are the
  properties `Transport` exists to provide (010); voice deliberately opts out of
  all of them.
- **A richer per-member ACL.** v1 is possession-of-token; see *Security*.
- **`K > 1` relay replication.** Named future work; no dedup story exists yet.
- **Real-network validation.** Every claim to date is measured over loopback UDP;
  see *Open questions*.

## Open questions

- **Production AEAD adapter.** `Aead::seal_into`/`open_into` exist only against
  `MockCipher`. `VoiceChannel`'s zero-allocation proof and its entire security
  narrative are contingent on a real adapter (mbedTLS/BoringSSL) landing behind
  the same seam (019/020) before this ships live traffic.
- **Real-WAN validation.** No experiment in ADR-030's debate tested packet loss,
  reordering, or jitter typical of an actual internet path — every proof ran over
  loopback. Given the workload is explicitly loss/latency-tolerant by contract
  this is lower-priority, but the specific p50/p99 latency numbers in ADR-030
  should not be used for WAN capacity planning without re-validation.
- **Shared-loop tail latency at scale.** The budgeted-fan-out bound was measured
  up to 1000-member rooms in the debate's environment; re-benchmark at a target
  deployment's actual scale before relying on the ~1.1ms p99 figure.
- **`K > 1` relay replication and its dedup story.**
- **Live capability reconfiguration.** The relay-fraction startup validation
  (C5) is checked at node startup only, not continuously; a live-reload check is
  needed if hot capability reconfiguration is ever supported.
- **ARM64 / weak-memory.** Like every other Quark subsystem proven to date, all
  sanitizer evidence is x86-64-only (GCC 14.2, Clang 20.1).
