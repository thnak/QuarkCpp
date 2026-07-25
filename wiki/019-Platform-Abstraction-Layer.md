# 019 — Platform Abstraction Layer (PAL)

The **one place** in Quark that touches OS APIs. Every OS-specific facility the
other specs reference — the event loop and sockets (010), thread affinity and NUMA
(002/003), durable file flush (012), the canonical monotonic clock (018) — is
defined here behind a thin, uniform surface with a per-OS backend. Consolidates
what was described piecemeal across four specs.

## The one rule

> Subsystem code includes PAL headers and **never** `<sys/*>`, `<netinet/*>`,
> `<windows.h>`, `<mach/*>`, or any OS header directly.

This single discipline is what keeps the cross-platform target (Linux, Windows,
macOS; x86-64 + ARM64) maintainable: porting to a new OS means writing one backend,
not hunting `#ifdef`s through the engine.

## Layering

```
PAL (compile-time OS primitives)
  └─ default Transport / WAL StateStore / scheduler (use the PAL)
       └─ Transport & StateStore seams (runtime-swappable adapters: gRPC, RocksDB…)
```

The PAL sits **below** the runtime-swappable seams of 010/012. Those seams exist
for heavy optional adapters; the PAL is the zero-cost floor the *default*
implementations stand on.

## Design decisions (self-debate)

### Compile-time backend selection, not a vtable

- **Runtime-polymorphic PAL** (virtual dispatch, swap backend at startup): flexible
  but puts a vtable call on the hottest primitives (clock reads, socket ops).
- **Decision: compile-time selection.** The OS is known at build time, so the
  backend is chosen then and monomorphized — no virtual dispatch on the hot path.
  Where in-process coexistence of two backends is needed (the simulation backend,
  below), the engine can be *templated* on the PAL backend (`Engine<Pal>`),
  monomorphized per instantiation — still zero-cost.

### Completion (proactor) I/O model, not readiness

The OSes split on the I/O model: `epoll`/`kqueue` are **readiness** ("you may read
now"); `io_uring`/IOCP are **completion** ("the read finished"). A single
abstraction must pick one.

- **Decision: a completion-based (proactor) interface.** It maps directly onto
  `io_uring` and IOCP, and readiness backends (`epoll`, `kqueue`) emulate
  completion by performing the operation on readiness. The reverse (emulating
  readiness over IOCP) is awkward. So `epoll`/`kqueue` backends carry a thin
  emulation layer; `io_uring`/IOCP are native.

## The surface

Grouped by the spec that consumes it. All fallible calls return
`std::expected<T, std::error_code>` (008's error model), normalizing OS errno /
`GetLastError` / mach errors into one type.

### 1. Clocks (011, 018)

- `mono_now()` — high-resolution monotonic instant for the timer wheel (011).
- `boot_now()` — the **canonical suspend-counting** monotonic clock for deadlines
  (018): `CLOCK_BOOTTIME` (Linux), `mach_continuous_time` (macOS), unbiased
  interrupt time (Windows).
- `wall_now()` — `system_clock`/UTC for calendar scheduling only.

### 2. Async I/O event loop + sockets (010)

- A proactor `IoContext`: submit connect/accept/send/recv completions; `wait(timeout)`
  drains completions. The `timeout` is how the timekeeper (011) sleeps until the
  next due timer — no separate timer primitive.
- A portable `Socket` handle (hiding `int` fd vs. Windows `SOCKET`), non-blocking,
  bound to an `IoContext`. Framing lives above the PAL.
- **UDP primitives (additive, 028)**: `udp_socket()`/`udp_bind()`/`udp_send_to()`/
  `udp_recv_from()` — non-blocking, same `std::expected<T, std::error_code>` /
  `would_block()` normalization discipline as the `tcp_*` primitives above, on both
  backends (`pal/linux_x86_64/net.hpp`, `pal/windows_x86_64/net.hpp`). Added for
  `VoiceChannel` (028) to register a UDP fd on the same `IoContext` a `Transport`
  already drives, per "the one rule" above — `VoiceChannel` calls these, never a
  raw `<sys/socket.h>`/`<winsock2.h>` symbol.

### Advanced I/O tier (ADR-026, proven)

**Two-tier shape**, the accepted resolution to the open question below:

- **Tier 1** (`submit_recv`/`submit_send` over a single `std::span`) is mandatory,
  identical, and trivially emulatable on every backend including `SimEngine`.
- **Tier 2** is exactly one named extension point:
  `IoContext::advanced_io() -> std::expected<AdvancedIo, std::error_code>`,
  queried **once** (at connection/session setup, never per-op) and cached by the
  caller as `std::optional<AdvancedIo>`. `AdvancedIo` declares the *same* public
  member signatures on every backend — capable backends give them real bodies
  plus a non-owning pointer back into `IoContext` state; non-capable backends
  give the same signatures trivial `std::unexpected(not_supported)` bodies and
  zero data members, staying an empty, `sizeof==1` type. This is **required, not
  optional**, to keep caller source byte-identical across backends (proven
  necessary by the C1r fatal-gap fix).

**Registered buffers are scoped, not a default.** `submit_*_fixed` is a niche
optimization for bulk/streaming transfers — it materially benefits only in the
hundreds-of-KB-to-MB range — and must **not** be the default or recommended path
for ordinary actor message frames, where the proven win is **vectored I/O**
(`submit_send_v`/`submit_recv_v`), not registered buffers.

**Registered-buffer lifetime contract (C9, normative).** Registered memory must
be owned by the caller (e.g. a shard's `std::pmr` allocation) for at least
`[register_buffers .. unregister_buffers-after-drain]`. `unregister_buffers`
returns `std::expected<void, std::error_code>` (never `void`, per 008's
error-model rule, which applies to every fallible PAL call without exception)
and must reject with `std::errc::device_or_resource_busy` — without issuing the
underlying kernel deregister call — while any submitted op against that region
has not yet completed. The reference mechanism is a per-region in-flight
counter, decremented in the same completion handler that would otherwise
release a slot.

**Concurrency limitation (documented, not silently shipped).** The reference
`FixedBufferFreeList` bitmask free-list is safe (TSan-clean, C4a) but **does not
scale** past ~1 uncontended thread once more than a handful of shard threads
contend for registered-buffer slots (measured: 4-thread aggregate throughput
below 1-thread throughput). The required fallback — pool-exhausted → vectored
`submit_send_v`, itself proven fast (C3) — is the *load-bearing* mitigation. A
cache-line-padded or per-shard-affine free-list redesign is follow-up work,
required before recommending registered buffers under multi-shard contention.

**Canonical two-tier caller pattern (proven).** Query `advanced_io()` once at
connection setup; branch on the cached `std::optional<AdvancedIo>` at the top of
the send/recv hot function body (proven zero measurable overhead vs. a
Tier-2-stripped baseline, C2); always keep the Tier-1 baseline call reachable as
the correctness fallback on every backend, including `SimEngine` (proven
deterministic per seed, C6).

### 3. Threads & CPU affinity (002)

- Thread spawn is `std::thread` (portable) — **not** re-abstracted.
- `pin_thread(core)` — affinity, which *is* OS-specific and, on macOS, only a
  *hint* (no hard pinning). The PAL exposes it as a best-effort hint; the scheduler
  already degrades gracefully (002).

### 4. NUMA topology & node-local memory (002, 003)

- `topology()` — nodes, and the node owning each core. Single-node on platforms
  without NUMA (macOS).
- `alloc_on_node(bytes, node)` — node-local backing for shard allocators (003);
  falls back to the default allocator where NUMA is absent. Optional huge-page
  backing behind the same call.

### 5. Durable file I/O & flush (012)

- Append/read/write over a portable path handle (hiding Windows wide-char paths),
  with `preallocate` (`fallocate`/`SetFileValidData`).
- `durable_flush(file)` — the sharp one: **`fdatasync` (Linux), `F_FULLFSYNC`
  (macOS — plain `fsync` does *not* reach the platter), `FlushFileBuffers`
  (Windows).** Getting this wrong silently breaks the WAL durability contract
  (012), so it is centralized here.

### 6. Cryptographic randomness (020)

- `secure_random(bytes)` — a single canonical **CSPRNG** so no subsystem reaches
  for a non-cryptographic `std::mt19937` when it needs a nonce, session key, or
  token: **`getrandom` (Linux), `arc4random_buf`/`getentropy` (macOS/BSD),
  `BCryptGenRandom` (Windows).** This is the OS-service half of security (020); the
  crypto *algorithms* stay in the vetted library, not the PAL. The sim backend
  stubs this (deterministic tests run with a fixed/stubbed source), distinct from
  014's non-cryptographic seeded PRNG.

  The `Aead` seam itself (`aead.hpp`, not PAL — see 020's "honest exception") gained
  additive, fixed-buffer `seal_into`/`open_into` overloads (028) so `VoiceChannel`
  can seal/open a datagram with zero heap allocation on the hot path. These exist
  today only against `MockCipher` — **NOT** real cryptography, same caveat as
  `seal`/`open`. A production AEAD adapter (mbedTLS/BoringSSL) implementing this
  interface, including `seal_into`/`open_into`, is required before `VoiceChannel`
  ships live traffic; it does not exist yet.

## Per-OS backends

| Facility | Linux | macOS | Windows |
|---|---|---|---|
| Event loop | `io_uring` (default) / `epoll` | `kqueue` | IOCP |
| Monotonic (suspend-counting) | `CLOCK_BOOTTIME` | `mach_continuous_time` | unbiased interrupt time |
| Affinity | `pthread_setaffinity_np` | **hint only** | `SetThreadAffinityMask` |
| NUMA | topology syscalls | **none (uniform)** | `GetNuma*` |
| Durable flush | `fdatasync` | **`F_FULLFSYNC`** | `FlushFileBuffers` |
| CSPRNG | `getrandom` | `arc4random_buf` | `BCryptGenRandom` |

The io_uring-vs-epoll choice on Linux is a build option, not a runtime one.

### Support matrix (ADR-026, proven — corrects a previously fabricated floor)

- **Linux io_uring backend:** floor kernel **5.1** (covers `io_uring_setup`,
  `IORING_REGISTER_BUFFERS`, `IORING_OP_READ_FIXED`/`WRITE_FIXED`, `READV`/
  `WRITEV` — the exact syscalls the proven code uses). The earlier cited 5.19
  figure was fabricated and conceded during ADR-026's cross-examination.
- **epoll fallback:** universal, selected as a **build-time** option per the
  existing rule above.
- **macOS kqueue:** capability entries currently **structural/unproven** —
  flagged pending a macOS host.
- **Windows IOCP:** vectored I/O available via `WSASend`/`WSARecv` (baseline
  Windows); registered buffers (RIO) are explicitly **out of scope /
  unavailable** pending a dedicated future design.

Kernel/backend selection is resolved at **CMake configure time** via a compile
probe, never `uname()`/runtime detection. Construction (`IoContext::create()`/
equivalent) must **fail fast** with `std::unexpected` if the compiled-in
backend's minimum requirement is not met at runtime (proven: C8) — never
silently degrade per-op.

## Simulation backend — how deterministic testing works (014)

The PAL is also the seam that makes the deterministic simulator real. `SimEngine`
(014) is the production engine compiled/instantiated against a **simulation PAL
backend**:

- **clocks** → a virtual clock the test advances explicitly (no wall-clock sleep);
- **I/O** → in-memory sockets with seeded, controllable delay/loss/reorder (the
  fault injection of 014, and the transit that 018's deadlines account for);
- **files** → an in-memory store with injectable flush faults (012).

So the same compile-time backend-selection mechanism serves production (per-OS
backends) *and* testing (the sim backend) — the simulator is not a mock universe,
it is the real engine on a different PAL floor. This is the "small internal
interface" 014 referred to: the PAL clock/I/O/file backends (the scheduler seam is
separate).

## Not in the PAL (non-goals)

- **The memory model.** Atomics, fences, and thread coordination use `std::atomic`
  / `std::thread`, which are correct on both x86-64 (TSO) and ARM64 (weaker
  ordering) without PAL help. The PAL abstracts OS *services*, never the C++ memory
  model — engine code must still use atomics correctly.
- Framing, serialization (016), scheduling policy, actor logic — all above the PAL.

## Dependencies

Std + direct OS syscalls/headers per backend. Third-party OS-helper libraries
(e.g. `libnuma`, `liburing`) are **optional** conveniences behind a backend; the
default backends prefer direct syscalls to keep the dependency surface minimal,
consistent with the whole-RFC posture.

## Open questions

- *(Whether the proactor interface should expose vectored/scatter-gather I/O and
  registered buffers generically, or only where the backend supports them:
  resolved — a mandatory Tier-1 baseline plus one named, uniformly-shaped Tier-2
  extension point (`advanced_io()`/`AdvancedIo`), so caller source stays
  byte-identical across backends. See "Advanced I/O tier" above, ADR-026.)*
- *(Minimum OS versions and the fallback matrix as a support policy: resolved —
  see "Support matrix" above, ADR-026.)*
- Whether `SimEngine` uses build-time backend selection or the templated
  `Engine<Pal>` form for in-process coexistence with a real engine in one test.
