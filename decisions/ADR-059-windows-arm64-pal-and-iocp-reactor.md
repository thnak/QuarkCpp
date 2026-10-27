# ADR-059 — Windows/ARM64 PAL backend selection, CI leg, and an opt-in IOCP reactor (#13)

Status: **Partially accepted.** Windows/ARM64: *cross-compiles only* — never executed. IOCP reactor:
implemented and verified on Windows/x86-64 as an **opt-in** (`-DQUARK_WIN_IOCP=ON`); the default
`pal::IoContext` stays WSAPoll. Spec: 019 §2 (event loop/sockets), §"Per-OS backends".

## What was and was not verified (read this first)
| Claim | Evidence | Status |
|---|---|---|
| PAL selects a Windows/ARM64 backend and compiles for ARM64 | `cmake -G "Visual Studio 18 2026" -A ARM64` cross build (MSVC 14.51, `Hostx64\arm64\cl.exe`) of `quark`, `tcp_transport_loopback_test`, `engine_lazy_activation_test`, `pal_iocp_context_test`; `dumpbin` shows `AA64 machine (ARM64)` | **Compiles. Not run.** No ARM64 hardware available. |
| Windows/ARM64 behaves correctly (weak memory, scheduler, mailbox) | none | **Unproven.** Depends on the `windows-11-arm` CI leg actually running green. |
| `windows-11-arm` CI job works | none | **Unrun.** YAML parses (`yaml.safe_load`); jobs not executed. `continue-on-error`. |
| IOCP reactor correct on Windows/x86-64 | `tests/pal_iocp_context_test.cpp` + the network-facing tests (`tcp_*`, `security_secure_transport_*`, `tcp_distribution_*`, `mixed_abi_*`, `cluster_*`, relay/topology) rebuilt with `QUARK_WIN_IOCP=ON` in a separate build dir: 39/39 and 2/2 pass. Default (WSAPoll) full suite: 239/239 pass. | **Verified locally on one machine; no stress/soak.** |
| IOCP is faster / scales better than WSAPoll | none measured | **Not measured.** No bench written. |

## 1. Windows/ARM64 audit
Everything x86-specific in `pal/` and the engine, found by grep:
- `pal::cpu_relax()` — MSVC/ARM64 had no valid branch: the AArch64 branch used GNU `asm volatile`,
  which MSVC does not accept. **Fixed**: MSVC ARM64 uses `__yield()` (`<intrin.h>`); `_mm_pause` stays
  x86-only.
- `platform_abi_tag` — Windows/ARM64 now gets its own tag `'A','64','W'` (`0x41'36'34'57`) instead of
  falling onto the x86 Windows tag; peers fall back to the tagged path (016) until ABI-equivalence is
  proven. (Pre-existing, **not changed**: Linux/arm64 still carries the x86-64 `'L'` tag — flagged for
  a separate decision, since changing it alters wire negotiation between Linux x86 and Linux arm64.)
- Producer-side Dekker elision (`mailbox.hpp` `producer_close_out_fence`, `engine.hpp`
  `producer_wake_fence`) is guarded by `__x86_64__ || _M_X64`; on `_M_ARM64` the else-branch emits a
  real `dmb ish` (`std::atomic_thread_fence(seq_cst)`). Correct by construction; the weak-memory
  re-gate (the `TODO(arm64)` seams) is **still open** and is what the CI leg would start to exercise.
  MSVC ARM64 defaults to `/volatile:iso`; the engine uses `std::atomic`, not `volatile`, for its
  protocols — not audited exhaustively.
- Cache line: `quark::cache_line_size` is a pinned 64. Windows ARM64 parts seen so far use 64 B lines;
  unverified on real hardware. Not changed.
- No TSC / `rdtsc`, no inline asm, no `__builtin_ia32_*` elsewhere in `include/`, `pal/`, `src/`
  (`bench/serialize_bench.cpp` already has an MSVC `_ReadWriteBarrier` path).
- Clock (`QueryUnbiasedInterruptTimePrecise`), CSPRNG (`BCryptGenRandom`), file I/O and Winsock code are
  Win32-API only and arch-generic.

**Layout decision.** `pal/windows_arm64/{clock,file_io,net}.hpp` are thin selector headers that include
the arch-generic `pal/windows_x86_64/*` code (same arrangement as `pal/linux_x86_64`, which also serves
arm64), selected by `_WIN32 && (_M_ARM64 || _M_ARM64EC)` in `pal/pal.hpp`, `pal/net.hpp`,
`pal/file_io.hpp`. Copying ~800 lines would only create drift; the directory gives ARM64-specific
divergence a home. The CMake platform guard now accepts `ARM64|arm64` on Windows.

## 2. CI
`.github/workflows/ci.yml` gains two additive, informational (`continue-on-error: true`) jobs:
`test-windows-arm64` (`windows-11-arm`, `-A ARM64`, full ctest, benches off) and `test-windows-iocp`
(`windows-2022`, `-DQUARK_WIN_IOCP=ON`, network-facing ctest subset). Existing jobs untouched. Both are
**unrun**. Promotion to required needs observed green runs. The ARM64 job does not use the
`ci/pins.env` MSVC pin (that pins the x64 toolset on `windows-2022`); an ARM64 pin is a follow-up once
the image toolset is known.

## 3. IOCP assessment and slice
**Before:** `pal/windows_x86_64/net.hpp` `IoContext` is a WSAPoll readiness loop: O(n) pollfd rebuild per
iteration, a loopback TCP socket pair as the cross-thread wake. `TcpTransport` is written against a
*readiness* surface (`add_fd/mod_fd/del_fd` + `recv_some/send_some` after a callback); it never hands
buffers to the kernel.

**Options.** (A) True proactor (overlapped `WSARecv/WSASend/AcceptEx/ConnectEx`, kernel-owned buffers):
the right end state for Windows throughput, but it changes the transport contract (buffer ownership,
partial completions, cancellation) in `tcp_transport.hpp`; out of this issue's safe scope.
(B) IOCP-driven *readiness* (AFD poll over a completion port, the wepoll/libuv technique): keeps the
transport untouched, makes the completion port the single wait primitive, replaces the socket-pair wake
with `PostQueuedCompletionStatus`.

**Done: (B)** as `pal::IocpContext` (`pal/windows_x86_64/iocp_context.hpp`), same public shape and
threading contract as the epoll/WSAPoll contexts. `pal::IoContext` is an alias selected by
`QUARK_WIN_IOCP` (CMake option, default **OFF**); the WSAPoll class is now `pal::WsaPollContext`.
`tcp_transport.hpp` was **not modified**. Semantics: level-triggered, one-shot AFD poll re-armed after
each dispatch; interest change cancels and re-arms; `del_fd` with a poll in flight parks the `Sock` in a
zombie set until the kernel returns its IOSB (freeing earlier would be a use-after-free); the destructor
drains before freeing. Event mapping: receive/accept/disconnect -> `EPOLLIN` (EOF is observed via
`recv==0`), send/connect-complete -> `EPOLLOUT`, connect-fail -> `EPOLLERR`, abort -> `EPOLLERR|EPOLLHUP`.

**Bug found by the suite, fixed:** `GetLastError()` was read after running timer callbacks, which clobber
it, so a plain wait timeout looked like a broken port and silently ended the loop (symptom: no CPU, no
progress; `tcp_transport_reconnect_test` and `tcp_relay_fallback_test` hung). The error is now captured
before timers run. The pre-existing transport tests caught it, not the new unit test.

**Risks (accepted for an opt-in):**
- `IOCTL_AFD_POLL` / `\Device\Afd` are **not documented by Microsoft**. Stable in practice across
  wepoll/libuv/mio, but not contractual. If the device cannot be opened `ok()` is false and `run()`
  returns immediately — fail-closed, not a silent fallback. This is why it is not the default.
- Not a proactor; no throughput claim made, no benchmark exists yet.
- One AFD handle for all sockets (wepoll groups 32 per handle for scale) — fine for current peer counts.
- No stress/soak (many sockets, churn) and no sanitizer run on it; unrun on ARM64 hardware.

## 4. Remaining work (staged)
1. Run the two new CI legs; fix whatever real ARM64 / hosted-runner behaviour they reveal.
2. Re-gate the `TODO(arm64)` weak-memory seams on Windows/ARM64 (mailbox close-out, run-queue wake).
3. Add a bench (connections x messages): WSAPoll vs `IocpContext` vs (later) proactor; promote
   `QUARK_WIN_IOCP` to default only on measured evidence plus a green CI soak.
4. Proactor (option A) behind a new PAL seam: spec-019 amendment + `TcpTransport` change; separate ADR.
5. ARM64 toolset pin in `ci/pins.env`; decide the Linux/arm64 `platform_abi_tag`.
