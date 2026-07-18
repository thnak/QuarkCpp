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

- Whether the proactor interface should expose vectored/scatter-gather I/O and
  registered buffers (io_uring fixed buffers) generically, or only where the
  backend supports them.
- Minimum OS versions (io_uring kernel floor, Windows IOCP feature level) and the
  fallback matrix (io_uring → epoll) as a support policy.
- Whether `SimEngine` uses build-time backend selection or the templated
  `Engine<Pal>` form for in-process coexistence with a real engine in one test.
