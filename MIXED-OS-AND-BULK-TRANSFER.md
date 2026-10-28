# Mixed-OS clusters and the bulk-transfer path (issue #17)

Factual status document. Everything below was read from the code in this working tree; line numbers
are approximate (other work is editing `tcp_transport.hpp`/`cluster.hpp` concurrently) so each
citation also names the symbol. Items marked **UNVERIFIED** were not exercised by any test or run.
When this file and an Accepted spec disagree, the spec wins (CLAUDE.md).

## 1. Bulk transfer (multi-MB to multi-GB payloads, one peer to another)

**Short answer: Quark has no network bulk-transfer API today. Chunk it yourself above Quark.**

### 1.1 What carries a payload between nodes

The only cross-node data path is `DistributedRouter::send_remote` (`include/quark/core/distribution.hpp`):
it serializes a whole message with 016 into ONE `MessageFrame.payload` (`std::vector<std::byte>`),
hands it to `Transport::send`. `TcpTransport` writes it as one `[u32 body_len][body]` record
(`net/wire_codec.hpp`, `encode_frame`, ~l.103). A message is therefore at most one frame.

### 1.2 Limits actually enforced (constants quoted from code)

| Limit | Value | Where | Behaviour at the limit |
|---|---|---|---|
| Max single frame body | `max_frame_body = 4u * 1024u * 1024u` (4 MiB) | `net/wire_codec.hpp:32`, applied per connection via `FrameStream::set_max_body` (`tcp_transport.hpp` ~l.626/668), tunable with `TcpTransport::set_max_frame_body(n)` (~l.106, before `start()`) | Receiver: `FrameStream::feed` returns false when `body_len > max_body_` (`wire_codec.hpp:165`); `TcpTransport::do_read` treats it as a protocol error and drops the connection (`tcp_transport.hpp` ~l.821). **The sender does not check the cap** (`encode_frame` has no size test; `send_remote` neither), so an oversized message is sent, then the receiver kills the connection and the frame is lost. Not covered by a test (UNVERIFIED at runtime; read from code). |
| Receiver memory per connection | one in-progress frame, <= `max_frame_body` (+ the decoded `MessageFrame.payload` copy), read in 64 KiB chunks | `tcp_transport.hpp` ~l.773 (`std::array<std::byte, 64 * 1024>`); comment at `wire_codec.hpp:27-31` ("one connection ... ~128 MiB peak" was the old 64 MiB cap) | Bounded by cap x number of connections. |
| Sender outbound backlog, data | `max_outbound_data_bytes_ = 64ull << 20` (64 MiB) per peer | `tcp_transport.hpp` ~l.1384; `set_max_outbound_data_bytes` ~l.138 | Frame is **shed** (counted in `outbound_data_drops_`/`drops_`, "dead-lettered") when the peer's data backlog + frame would exceed the cap and the lane is non-empty. A single frame into an empty lane always goes through (~l.551-552). Shedding is silent to the sender actor: `Transport::send` is fire-and-forget (`transport.hpp`). Also enforced on the cross-thread post queue before encoding (~l.280). |
| Sender outbound backlog, control | `max_outbound_control_bytes_ = 256ull << 20` | ~l.1385 | Control/handshake/schema traffic keeps flowing past a data backlog but is also bounded. |
| Per-peer inbound admission | 4096 frames/s, burst 1024 (`peer_rate_per_sec_`, `peer_burst_`) | `distribution.hpp` ~l.351-352, `set_peer_budget` ~l.103 | Excess frames from one peer are dropped before authz/decode (`peer_sheds()`). A fast chunk sender above Quark must stay under this or raise it. |
| Schema/handshake frames | `kMaxAuthenticateFrameBytes = 64 KiB`, `kMaxSchemaFrameBytes = 64 KiB`, `kMaxSchemaTypes = 4096` | `core/secure_transport.hpp:176-178` | Oversized frames are refused. |
| Frames per session key | `kDefaultMaxFramesPerKey = 1'000'000` | `secure_transport.hpp:168` | Triggers re-keying (C5), relevant to long chunk streams (UNVERIFIED under load). |

There is no stream-size limit because there is no network stream (1.3). Total size is bounded only by what the application chooses to send.

### 1.3 024 inbound streams are NOT wired to the network

`StreamChannel` / `StreamActivation` / `ReplyStream` (`core/stream_channel.hpp`, `stream_activation.hpp`,
`reply_stream.hpp`) give a bounded, credit-controlled, lossless SPSC ring **inside one process** between
a producer thread and an actor. They are referenced only from `actor_ref.hpp`, `fanout.hpp` and each
other; `distribution.hpp`, `transport.hpp` and `net/` contain no stream concept (grep: no `Stream`
symbol in `distribution.hpp`/`transport.hpp`/`net/`). The by-reference regime
(`StreamMode::ZeroCopyRetained`) is declared and explicitly "NOT wired" (`stream_channel.hpp:52`), inline
frames are capped at `kStreamInlineMax = 56` bytes (`stream_channel.hpp:56`). So:

* Backpressure of 024 (producer stalls on credit depletion, lossless) holds locally, **not across a
  TCP hop**. Nothing converts remote receiver credit into sender stall.
* Cross-node, the only backpressure signals are: (a) `Transport::admit_send` / ADR-046 congestion
  gate (`SwimMembership` broadcasts `Congested` on a resident-message watermark; `DistributedRouter`
  sheds locally when `admit_send` is false and counts `remote_sheds()`), and (b) the 64 MiB per-peer
  outbound cap above. Both **shed**; neither stalls the sender. A bulk sender must therefore treat
  `send` as lossy and run its own ack/credit protocol.
* Ordering: per-(sender,receiver) FIFO over the single connection (`tcp_transport.hpp` banner;
  `tcp_distribution_integration_test`), but a shed frame leaves a gap; re-ordering above Quark needs
  sequence numbers.

### 1.4 Cancellation / sender disappearance

* Sender cancels or dies mid-frame: the partial frame lives only in that connection's `FrameStream`
  and is discarded with the connection (`handle_dead` ~l.942; no partial frame is ever delivered to an
  actor). Whole frames already decoded were already handed to the receiving actor's mailbox and are
  **not recalled**. Quark has no "abort transfer" primitive; the data lifetime requirement in the
  issue (nothing half-delivered after cancel) must be enforced by the application (transfer id +
  commit message, receiver discards uncommitted chunks and clears them on `NodeDead`/`Left`).
* Sender-side queued-but-unsent frames: `close_peer` (called when SWIM declares the peer dead)
  erases the peer's queue (frames are dead-lettered, `tcp_transport.hpp` `close_peer`, `handle_dead`
  give-up branch); on a plain reconnect they are salvaged (`salvage_out`) and resent.
* Mid-flight drops are silent to the sending actor (fire-and-forget).

### 1.5 Recommendation (not implemented)

A real bulk path would need: sender-side frame-size check, a network stream frame kind carrying
credit grants (extending the 024 credit ring across the transport), a receiver-side commit/abort
frame, and an inbound cap tied to a registered stream. That is a design item (ADR via
`design-debate-prove`), not part of this change.

## 2. Mixed-OS behaviour and what is identical across PAL backends

Backends: `pal/linux_x86_64/` (used for **both** Linux/x86-64 and Linux/arm64, the directory name is
historical, `pal/pal.hpp` header comment) and `pal/windows_x86_64/`. There is no arm64-specific or
Windows/ARM64 backend (issue #13); `pal.hpp` selects by `_WIN32`, not by architecture. CI covers
Linux/x86-64, Linux/arm64, Windows/x86-64 (`.github/workflows/ci.yml`).

### 2.1 The ABI tag and the negotiation (016)

`pal::platform_abi_tag` (`pal/pal.hpp:46-50`): Windows `0x78'86'64'57` ('x','86','64','W'), every
non-Windows target `0x78'86'64'4C` ('x','86','64','L'). Consequences, verified by reading:

* **Linux<->Windows pairs always take the canonical tagged fallback** by design: the tags differ and
  the comment at `pal.hpp:41-45` says the unified fast path is deliberately not enabled until
  MSVC-vs-GCC/Clang layout equivalence is proven. Same-OS pairs take the tagless fast path **only
  when** an authenticated schema advertisement is present (below).
* **Linux/arm64 advertises the x86-64 'L' tag** (same constant), so Linux/x86-64 <-> Linux/arm64 is
  treated as ABI-equal and may take the tagless path. Whether that is safe for every described type
  (both are little-endian LP64) is **UNVERIFIED**; no test puts an x86-64 and an arm64 node together.
* The check is `negotiate<T>()` (`core/wire.hpp:677`): tagless iff fingerprint AND tag match, else
  tagged + `g_wire_fallback_warn` (009 hook, null by default). The router's default peer-schema
  provider asks `Transport::peer_accepts_type(owner, key, pal::platform_abi_tag)`
  (`distribution.hpp` ctor ~l.79-84). Only `SecureTransport` stores adverts
  (`secure_transport.hpp:289-318`); **plain `TcpTransport` never advertises, so it is always tagged**.
* The tag compared against is a **compile-time constant**; there is no per-node injectable tag.
  The new test forces a mismatch by re-advertising an alien tag through the public
  `Transport::set_local_schema`.
* Handshake direction: `SecureTransport::local_is_client` (`secure_transport.hpp:448`) makes the
  **lower NodeId** the mTLS client. A higher-id node that only knows a lower-id seed sends `Join`
  frames that are dropped (`send` with no session just triggers `ensure_handshake`, which returns for
  the server role) until the lower-id side connects to it. The conformance test therefore gives the
  joiner the lower id; a production cluster must make sure the lower-id side has the other's endpoint
  (or the seed must have the joiner's). Worth a follow-up issue.

### 2.2 Identical across backends (by construction or by shared code)

Shared code above the PAL seam, so identical on both OSes: `TcpTransport` framing and 4 MiB cap, hello
handshake, dial-dedup (lower NodeId's connection wins, `cluster.hpp` `dial_winner`), control-over-data
priority between whole frames (`load_next_frame`, ADR-046 S4; ordering within a lane is FIFO),
per-peer backlog caps, SWIM, 016 canonical tagged encoding (explicitly little-endian), `SecureTransport`.
The PAL surface is deliberately mirrored (`windows_x86_64/net.hpp` banner): same `fd_t`/`IoContext`
shape, same `EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP` mask, same `tcp_listen` default backlog 128
(`net.hpp` in both), `SO_REUSEADDR`, `TCP_NODELAY`.

### 2.3 Different

| Area | Linux | Windows |
|---|---|---|
| Reactor | `epoll` (`epoll_wait`, `pal/linux_x86_64/net.hpp:303`), cross-thread wake via `eventfd` | `WSAPoll` readiness loop (not IOCP) rebuilt from in-memory maps each iteration (`pal/windows_x86_64/net.hpp:8-19, 385`); wake via connected loopback TCP pair. IOCP is a stated future upgrade; #13. |
| Peer-close writes | `MSG_NOSIGNAL` so EPIPE not SIGPIPE | no SIGPIPE concept |
| Kernel buffering | auto-tuned loopback buffers | smaller effective window: `tcp_transport_control_priority_test.cpp:74-75` records the observed backlog at the moment a control frame is queued, ~30/500 on MSVC loopback vs ~300-350/500 on Linux. The assertion is loose on purpose; **what is guaranteed is only that the control frame is not delivered last, not how many data frames precede it.** |
| Clock | `CLOCK_BOOTTIME` class (`pal/linux_x86_64/clock.hpp`) | Windows `BootClock` (`pal/windows_x86_64/clock.hpp`); equal semantics claimed (suspend-counting), cross-OS equality UNVERIFIED |
| Durable I/O | `fdatasync`/`fsync`, `rename`, directory fsync | Windows equivalents in `pal/windows_x86_64/file_io.hpp` (power-loss equivalence UNVERIFIED) |
| CSPRNG | `getrandom` | `BCryptGenRandom` |
| ABI tag | 'L' | 'W' |
| Sanitizer coverage | ASan/UBSan/TSan (Linux), plus arm64 | MSVC ASan only, informational leg; no TSan/UBSan analogue |
| Perf gate | enforced (x86-64 reference) | none |

**Close semantics:** the close/EOF/RST handling lives in shared `tcp_transport.hpp` (`do_read` ->
`handle_dead`), but error code normalization differs per backend (`pal::last_error`/`would_block`
are each backend's own); behaviour on abortive close (WSAECONNRESET vs ECONNRESET) is mapped into the
same `handle_dead` path. Windows abortive-close ordering vs Linux has **no dedicated cross-OS test
(UNVERIFIED)**.

**Not guaranteed identical:** timing and throughput, the amount of data buffered in flight, the order
in which concurrently-ready fds are serviced (epoll vs WSAPoll array order), `listen` backlog behaviour
under SYN floods, and anything in 2.1 involving arm64 vs x86-64 layout.

## 3. Conformance coverage

* `tests/mixed_abi_cluster_conformance_test.cpp` (all OSes): two full nodes over real loopback TCP in
  one process, `SecureTransport` (mock handshake, **not real TLS**), join via `SeedListDiscovery`,
  SWIM membership, request/reply both directions (application-level; `DistributedRouter::ask` is
  local-only, `distribution.hpp` ~l.191), positive control (same ABI => `WireMode::Tagless` on every
  frame, 0 fallback warnings), forced mismatch (alien tags => every frame `WireMode::Tagged`,
  warning fires, values decode correctly), clean `leave()`.
* It does **not** prove: mutual TLS with real certificates, SWIM on a real 3-node mixed cluster, or
  Linux/arm64. (A real Windows<->Linux pair IS now exercised, see "Real cross-OS run" below.)

#### Real cross-OS run (2026-10-28, issue #17)

`tests/netem/mixed_os_node.cpp` is the same protocol as the test above with ONE node per process and the
genuine `pal::platform_abi_tag` (nothing forced); `tests/netem/run_mixed_os.sh` drives it (built, not a
CTest target: it needs two hosts). Run: seed on Linux/x86-64 (g++ 15.2, Release), joiner on
Windows/x86-64 (clang++ 22, Release), the joiner reaching the seed's loopback listener through `ssh -L`.

| Pair | local / peer abi_tag | inbound frames | Result |
|---|---|---|---|
| Windows <-> Windows (control) | 0x78866457 / 0x78866457 | tagless 100, tagged 0 | PASS |
| Linux <-> Linux (control) | 0x7886644C / 0x7886644C | tagless 100, tagged 0 | PASS |
| **Windows <-> Linux** | 0x78866457 / 0x7886644C | tagless 0, **tagged 100** | **PASS** (join via `SeedListDiscovery`, SWIM Alive both ways, 50 Req/Resp each way with the right sums, clean leave -> Left) |

Finding (fixed by ADR-061): the same `Responder`/`Collector` actors declared in an ANONYMOUS namespace got
different actor type keys on the two OSes (GCC spells it `{anonymous}`, Clang `(anonymous namespace)`), so
every remote `tell` was received (tagged, as expected) but never dispatched (`req_handled=0`). Message keys
(`Req`, `Resp`, Described fingerprints) matched throughout. ADR-061 normalises the per-compiler spelling
(MSVC `struct `/`class ` elaborators, the three anonymous-namespace spellings, comma spacing) before hashing
and adds `quark_type_name` for an explicit, rename-stable identity. Re-run with the actors back in the
anonymous namespace: Windows/clang <-> Linux/g++ PASS (`req_handled=50`), and real `cl.exe` 14.51 computes
the same keys as Clang and GCC in `metadata_typekey_conformance_test`. **No same-compiler-family workaround
is needed any more** (ADR-062 then removed compiler spelling from actor keys entirely: every actor now pins an explicit
`quark_type_name`, and this run still passes with those keys); types persisted by `cl.exe` or in a GCC anonymous namespace before ADR-061 have new
keys (accepted pre-1.0 break; pin with `quark_type_name`). A live `cl.exe` <-> g++ *cluster* run was not
done (this host's `mixed_os_node` is built with clang++); only the key derivation was verified on `cl.exe`.
Still NOT covered: Linux/arm64, mTLS with real certificates (mock handshake), and a CI job: GitHub-hosted
runners are not mutually routable, so the cross-host run stays a manual/self-hosted step.
* CI: `.github/workflows/ci.yml` runs the test on every existing leg (it is auto-globbed) and adds a
  `mixed-os-conformance` job documented there as the cross-runner follow-up (see the workflow comment).
  **No GitHub CI was executed to produce this document.**
