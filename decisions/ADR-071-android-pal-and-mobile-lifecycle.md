# ADR-071 — Android PAL and the mobile member lifecycle (019, 021; issue #43, AeroCoWorker ADR-044 Q4)

Status: **Proposed.** Evidence below is real but narrow: an Android **x86-64 emulator** (API 37) and a Windows host. **arm64-v8a is built
and linked, never executed. Nothing has run on a physical device, no real Wi-Fi/cellular switch, no battery measurement.** Every claim names the
experiment that breaks it; the claims that could not be tested here say so (section 8). Direction: [ADR-067](ADR-067-direction-aerocoworker-network-center.md)
(Android is a first-class target). Spec text (Draft): [019 §"Android target"](../019-Platform-Abstraction-Layer.md),
[021 §"Mobile member profile"](../021-Cluster-Formation-and-Lifecycle.md).

## 1. Decision

1. **Android is a toolchain target, not a new I/O backend.** Android = Linux kernel + bionic. The existing Linux PAL (epoll) is reused; the
   `android` target adds an NDK contract (`cmake/QuarkAndroid.cmake`), its own ABI tag, and a C ABI library for the app's JNI glue.
2. **The mobile lifecycle is a three-call contract the host application makes into the node** — `suspend(reason, lease)`, `resume()`,
   `network_changed()` — and Quark never calls up into Java/Kotlin. The app owns the foreground service, `WifiManager.MulticastLock`,
   `NsdManager`, Doze and the decision of when to suspend.
3. **A "mobile member" profile in SWIM, with no new membership state.** A suspending member announces a *graceful, bounded-lease* suspension
   (`ControlKind::Suspend`); each peer holds a lease it alone bounds; the member stays `Alive` and in the view, is not probed, and
   resumes by rejoining at a higher incarnation. A lapsed lease ends in the ordinary Suspect -> Dead path.
4. **One transport rule changes** (found by the tests, section 5): a fresh inbound connection from an identified peer supersedes an existing
   *inbound* connection from it. The node-id election stays for a dial race (one self-initiated, one inbound).

## 2. Build target and what it was verified with

| Item | Choice | Why |
|---|---|---|
| NDK | r28c (28.2.13676358), clang 19.0.1 | latest stable r28 at install; r27 was the alternative |
| ABIs | `arm64-v8a` (first-class), `x86_64` (emulator/test); 32-bit refused | phones are arm64; the emulator here is x86-64 |
| Min API | **35** (Android 15) | AeroCoWorker's client floor; a lower `ANDROID_PLATFORM` is a configure error |
| STL | `c++_static` | the `.so` ships its own libc++ (no system libc++ on Android) |
| I/O backend | epoll (the Linux backend, unchanged) | section 3 |
| 16 KiB pages | `-Wl,-z,max-page-size=16384` on the `.so` | Android 15+ devices may use 16 KiB pages |

Toolchain gaps found by building (each fixed at its cause, none suppressed):

- **`std::stop_token`/`stop_source` need `-fexperimental-library`** on libc++ 19 (the core uses them). Set globally so `try_compile` probes agree.
- **`std::atomic<std::shared_ptr<T>>` does not exist in libc++ 19** (7 sites: `Topic`, `FanOut`, `CapabilityRegistry`, `SecureTransport`). New
  `quark::detail::AtomicSharedPtr` (`include/quark/detail/atomic_shared_ptr.hpp`): *exactly* `std::atomic<shared_ptr>` where the standard feature
  macro exists (Linux/Windows unchanged), a mutex-guarded pointer otherwise. **Not benchmarked against the 023 budgets** (section 8).
- **`std::atomic_ref` does not exist in libc++ 19** (one reader, the ADR-022 histogram scraper snapshot). `quark::detail::load_relaxed`
  (`std::atomic_ref` where present, `__atomic_load_n` otherwise).
- **bionic fortify warning (a latent bug):** `pal/linux_x86_64/file_io.hpp` passed a mode argument to `open(O_RDONLY|O_WRONLY)` without `O_CREAT`
  (`'open' has superfluous mode bits`). Mode removed from those two calls.
- **ABI tag:** Android gets its own `platform_abi_tag` (`'A','N','D','R'`), not Linux's: bionic struct layout is unproven equal to glibc's, so no
  tagless 016 fast path to a Linux peer. Observed in section 6 (A5): Android<->Windows negotiates the tagged path as expected.

What was built: the whole `tests/` set (246 test/control executables + `mixed_os_node`) for **x86_64** with **zero warnings in the final build**, and the **same whole set for arm64-v8a** (497 build steps, 0 failures, 0 warnings, built and linked only), plus
`libquark_android.so` and `quark_mobile_runner` for **both ABIs** (section 7).

## 3. No `io_uring` on Android (claim C8)

Android's app seccomp allowlist is `SYSCALLS.TXT - SECCOMP_BLOCKLIST_APP.TXT + SECCOMP_ALLOWLIST_APP.TXT`, "applied only to zygote spawned
processes" (header of `bionic/libc/SECCOMP_BLOCKLIST_APP.TXT`, AOSP `main`, fetched 2026-10-31). `grep -i io_uring` over `SYSCALLS.TXT`,
`SECCOMP_BLOCKLIST_APP.TXT`, `SECCOMP_BLOCKLIST_COMMON.TXT` and `SECCOMP_ALLOWLIST_APP.TXT` returns **nothing**, so `io_uring_setup/enter/register`
are not in the allowlist and an app process that calls them is killed with `SIGSYS`.

- **This is a source citation, not an on-device test of an app process.** On the emulator, from `adb shell` (not a zygote process, so not under this
  filter), a raw `io_uring_setup(8, NULL)` returned `-1`, `errno = EFAULT` — the syscall *exists* and is reachable there. That neither proves nor
  disproves the app case and must not be read as support for io_uring on Android.
- **Experiment that breaks C8:** an APK (or any zygote-spawned process) calling `syscall(__NR_io_uring_setup, ...)` does **not** receive `SIGSYS`
  on an Android 15+ device. Owed (needs an APK). Until then the PAL's rule — epoll only, never probe io_uring at run time — costs nothing if C8 is
  wrong and avoids a process kill if it is right.

## 4. The mobile member profile (021)

Mechanism (all in `SwimMembership`, with the lease table in `mobile_lease.hpp` and the host entry point in `mobile_member.hpp`):

- `ControlKind::Suspend = 10`: `seq` = requested lease (ns, relative), `subject_incarnation` = reason. Additive; an N-1 node ignores the unknown kind.
- Receiver policy (`MobileProfile`): `max_lease_ns` 15 min (clamps any request), `max_suspended` 64 (table cap), default self-lease 10 min.
- A leased member: not probed, not an indirect-ping relay, not a gossip target, status stays Alive, events `Suspended`/`Resumed`,
  `NodeHealth::suspended`. Second-hand Suspect/Dead about it at or below its suspend incarnation are dropped (`suspicions_suppressed()`).
- `resume()`: incarnation + 1, in-flight probe voided, held Suspects get a fresh window, Gossip to every live peer at once. A non-Suspend frame from a
  leased member at a *higher* incarnation clears the lease on arrival.
- Lease lapse: Suspect (a full suspicion window to refute), then the ordinary Dead and removal. A **lapsed lease is not renewable at the same
  incarnation** (found in review of this ADR's own design: a member that never answers could otherwise re-announce forever).

`MobileMember::network_changed()` drops each live peer connection through `TcpTransport::reset_peer_connection` (an existing narrow seam) and calls
`SwimMembership::network_changed()` (void the in-flight probe, forgive held Suspects). `suspend()` is a no-op for an offline, left or already-suspended node.

## 5. Claims, each with the experiment that breaks it

Tests run on the Windows MSVC host (the normal suite: **full `ctest -C Release -j 4`, 246 / 246 passed, 0 failed, 0 warnings in the build** — the Windows build registers 246 tests; the 3 new ones are included) and again unchanged on the Android x86-64 emulator (section 6). Virtual clock, deferred-delivery
fabric (`tests/swim_deferred_fabric.hpp`), no sleeping — except C7 (real sockets) and A5/A6 (real processes).

| # | Claim | Test | Control (must show the opposite) | Result |
|---|---|---|---|---|
| C1 | A member suspended 10 min (lease 12 min) is **not** suspected, killed or removed by peers; zero Suspect/Dead/Left events | `mobile_member_suspend_test` | the same 10-min silence with `set_online(false)` and **no** `suspend()`: first Suspect at +360 ms, then Dead at all 3 peers, removed from every view | passes; control fires |
| C2 | A lapsed lease ends in the ordinary removal path, no zombie | `mobile_member_lease_expiry_test` | 2 s before the 10-min lease ends the member is still Alive and in the view; Suspect at +600000 ms, Dead at +600600 ms (suspicion 500 ms); no lease left behind | passes |
| C2b | A lapsed lease is not renewable at the same incarnation | same | a refresh *inside* the lease is accepted | passes |
| C3 | `resume()` rejoins with **0 protocol ticks** (one fabric flush): leases cleared, status Alive, incarnation raised, exactly one `Resumed` event per peer, no Suspect/Dead event anywhere before, during or after, views full 5 s later | `mobile_member_suspend_test` | (C1 control: without it, Dead) | passes. **Stated bound: rejoin completes within one frame delivery of `resume()`; over a real link one one-way delay; if that frame is lost, the next gossip round (<= 1 protocol period).** |
| C4 | A peer that missed the announcement suspects/kills the member locally, but the lease holders are **not** dragged along, and the member heals on resume (R3: strictly higher incarnation beats a tombstone); all views full within 40 periods | same | holders stay Alive while the deaf peer is Dead (`suspicions_suppressed() > 0`) | passes |
| C5 | A stale (incarnation below known), zero-lease, or non-member Suspend is ignored | same | the same frame at the current incarnation is accepted | passes |
| C6 | The receiver bounds the lease (100 days asked, 15 min cap: Alive at cap - 2 s, Dead after) and the table (`max_suspended = 2`: two leased stay Alive, the third is removed) | `mobile_member_lease_expiry_test` | inside the cap the member stays Alive | passes |
| C7 | After a network switch the node reconnects cleanly: 50 frames sent at once after `network_changed()` arrive exactly once, in order; receiver `replays_rejected() == 0`; 1 reconnect; 1 connection open; the PC received exactly `[0..49] ++ [70..119]` | `mobile_member_network_changed_test` (real sockets, black-hole proxy) | **P3, no `network_changed()`: 20 frames into the dead path, 0 delivered, transport sees 0 reconnects and 1 healthy connection — the pre-existing behaviour (TCP would only notice on its own timeout)** | passes after the transport fix below |
| C8 | No `io_uring` for apps | section 3 | — | cited, **not tested on a device** |

**The C7 finding (a real defect, fixed).** The first run of the C7 test **failed**: after `network_changed()` the phone reconnected (2 connections
through the proxy, `reconnects() == 1`) but **0 of the 50 frames were ever delivered**. Cause: the PC (node id 1) still held the old, half-open
*inbound* connection from the phone (id 2); when the phone's new inbound connection arrived, `register_identified` ran the dial-dedup election,
`keep_local_dial(1, 2)` wants the *self-initiated* connection, neither connection was self-initiated, so the **old dead one won and the live one was
closed** — a permanent lock-out whenever the reachable side has the lower node id. The election exists for a dial race; two *inbound* connections
from one peer are a reconnect. Fix: when both are inbound, the new one supersedes (`include/quark/net/tcp_transport.hpp`, `register_identified`).
All existing TCP/dedup/relay tests still pass (section 7).

Residual risk: **a source-address change.** A Wi-Fi to cellular switch changes the phone's source IP. `TcpTransport`'s default
`strict_peer_addr_binding` refuses an inbound connection for an identified live peer from a different IP (pre-auth protection, `drops_++`), so on a
real switch the reachable side would still refuse the phone until the old connection dies. `set_strict_peer_addr_binding(false)` exists for exactly
"multi-homed peers that reconnect from a different address" but weakens that protection (anyone who can claim the HELLO identity could evict a
live connection; with `SecureTransport` the evictor still cannot deliver, so it is a DoS, not an impersonation). **Not tested here**: the loopback
proxy keeps one source IP, and the PAL has no bind-to-source call to vary it. Recommendation to Q-owners: accept phones with
`set_strict_peer_addr_binding(false)` *and* a handshake-gated `SecureTransport`; test it with a two-address (127.0.0.2) variant once the PAL can bind a source.

## 6. Emulator and cross-device evidence (Android 17 / API 37 x86-64 AVD `Pixel_10_Pro_Fold`, kernel 6.12, run headless)

- **A4 — the repo's own suite on the emulator.** All **246** test/control executables that have a binary (of 248 `ctest` entries; the other two,
  `serialize_fingerprint_mismatch_oob` (ASan-only) and `stream_drain_zero_rmw` (an `objdump` script), are not runnable on a device) were
  `llvm-strip`ped, `adb push`ed to `/data/local/tmp` and run with `timeout 300`: **246 / 246 exit 0, 0 failures**, including the three mobile tests
  and the two Dekker-litmus tests (the slowest, 28-29 s). x86-64 only; the arm64 weak-memory behaviour is untouched by this.
- **A5 — emulator <-> Windows, over TCP.** Network path: **`adb reverse tcp:47002 tcp:47002`** (the emulator's `127.0.0.1:47002` is carried by the adb
  server to the host's `127.0.0.1:47002`; the host's `mixed_os_node a` listens there, the emulator runs `mixed_os_node b`). Result: both
  sides `PASS`: 50 Req/Resp pairs in each direction (`req_handled=50 resp_got=50 sum=2450`), `peer_abi` 0x414E4452 (Android) vs 0x78866457 (Windows),
  `same_abi=0 tagless=0 tagged=100` — the differing ABI tags correctly forced the canonical tagged path.
- **A6 — a real-time suspension across the two devices** (`quark_mobile_runner`, the C ABI the JNI glue will call; Windows `seed` <-> emulator `phone`,
  same `adb reverse` path; see `src/android/quark_mobile_runner.cpp`):
- **The 10-minute acceptance run, real time (600 s suspend, lease 720 s, then `network_changed()`):** the seed's state log for node 2 reads
    `ALIVE t=2.1` -> `SUSPENDED t=3.7` -> `ALIVE t=603.8`: **600 s suspended, never Suspect, Dead or Left**, and **Alive again 0.1-0.2 s after the
    phone's `resume()`** (phone clock 601.6 = seed clock 603.7). The phone then reported `PASS` (`seed_alive_after_resume=1`, 605 probe frames
    received from the seed throughout; wall time 642 s). The seed log goes `SUSPECT t=607.1` / `DEAD t=612.2` only *after* the phone process
    had printed its result and exited without leaving (a crash, as plain SWIM must treat it) — the control, observed for free. That run used the
    Windows runner built before the `--seconds` auto-exit tweak, so the seed printed no `RESULT` line; it ran before the C2b guard was added.
  - **The same run repeated with the final binaries (30 s suspend, lease 120 s, `--network-change`)**: `RESULT seed PASS frames_from_peer=100
    probes_sent=36 saw_alive=1 saw_suspended=1 alive_after_resume=1 suspect_or_dead_or_left=0` and `RESULT phone PASS joined=1 suspended=1
    seed_alive_after_resume=1 frames_from_seed=35`; states `ALIVE 2.1` -> `SUSPENDED 3.7` -> `ALIVE 33.7`; the 100 frames are the 50 sent before and the 50
    after the suspension (+ `network_changed()`), none lost.
  - **Stated rejoin bound (measured): <= 0.2 s after `resume()` on this path (adb reverse over loopback); the unit bound is one frame delivery.**

## 7. Size (stripped `libquark_android.so`, Release, `-O3`, `c++_static`, `--gc-sections`)

`libquark_android.so` = one cluster node (`TcpTransport` + `SwimMembership` + `MobileMember`) behind the C ABI; `llvm-strip --strip-unneeded`;
`NEEDED`: `libc.so`, `libm.so`, `libdl.so` only; LOAD segments aligned to 0x4000 (16 KiB).

| ABI | unstripped | **stripped** | stripped + gzip -9 | `.text`+`.rodata` / `.data` / `.bss` |
|---|---|---|---|---|
| `arm64-v8a` | 8,777,032 B | **474,784 B (463.7 KiB)** | 162,149 B | 453,896 / 17,776 / 5,808 B |
| `x86_64` | 8,387,816 B | **468,960 B (458.0 KiB)** | 166,496 B | 448,448 / 17,480 / 3,352 B |

Honest scope: this is the *network node* (transport + membership + lifecycle), **not** the actor engine (the engine is header templates the app instantiates
on top; its size depends on the actors it declares) and **not** a `SecureTransport`/mbedTLS-linked build (ADR-044's stack adds to it). The size gate in
the issue is therefore met for the library as built and must be re-measured when the security stack and the JNI glue are linked.

## 8. Owed, not faked

- **No physical device.** The owner's phone is not attached; the emulator cannot measure battery. **Idle battery cost: not measured** (the acceptance
  line "binary size and idle battery cost are measured and published" is half met: size yes, battery no).
- **No real Wi-Fi <-> cellular switch**, in particular the source-address change (section 5).
- **arm64-v8a is built and linked, never run** (no arm64 device or emulator here): the arm64 correctness and weak-memory gates are CI/device work.
- **ASan/UBSan/HWASan on the NDK, TSan (unavailable on Android):** not run. The Android fallback `AtomicSharedPtr` is a *new synchronisation
  point*, so it needs the Linux/Windows TSan legs to see it exercised on a build where it is forced on (owed: a `QUARK_FORCE_ATOMIC_SHARED_PTR_FALLBACK`
  test build); and it is **not benchmarked** against the 023 budgets.
- **`io_uring` blocked for apps:** cited from source, not observed (C8).
- **Multicast on the emulator** (the app holds the `MulticastLock`; discovery is Q3's) and **`NsdManager`**: not exercised.
- **The Kotlin/JNI side** in AeroCoWorker: not here. `libquark_android.so`'s C ABI is the contract; `quark_mobile_runner` is its only caller.
- **The suspend announcement is not transitive.** A member sends `Suspend` to its live peers only; under a `BoundedPartialView`/Gateway topology (026)
  a node outside its direct view holds no lease and treats the silence as plain SWIM (C4's heal-on-resume covers correctness, not the churn).
- **A member can buy at most `max_lease_ns` of silence per proof of life**, repeatedly, by refuting and re-announcing: it is "present" by SWIM's
  definition each time it answers. Quantifying that abuse is a security-review item, not a safety bug.
- **The announcement is only as authenticated as the transport.** On `SecureTransport` it is AEAD-sealed per peer session; on the plaintext dev
  transport the HELLO identity is a claim (as for every control frame). The runner and library use the plaintext dev transport
  (`allow_insecure_dev`), the same posture as every test in this repo; a shipping phone node must run the ADR-044 security stack.
- **A6 lease semantics on a really frozen process:** `suspend()` here keeps the process running and merely stops the protocol; a frozen process also
  stops its I/O thread. The two are equivalent for the peers; only a device proves it for the OS.

## 9. Files

Spec: `019-Platform-Abstraction-Layer.md`, `021-Cluster-Formation-and-Lifecycle.md`, `CONVENTIONS.md`, `README.md`. Build: `CMakeLists.txt`,
`cmake/QuarkAndroid.cmake`. Core: `include/quark/core/{mobile_lease,mobile_member}.hpp` (new), `cluster.hpp`, `membership.hpp`,
`include/quark/net/tcp_transport.hpp`, `include/quark/detail/{atomic_shared_ptr,atomic_relaxed}.hpp` (new), `pal/pal.hpp`,
`pal/linux_x86_64/file_io.hpp`. Android: `include/quark/android/quark_android.h`, `src/android/{quark_android,quark_mobile_runner}.cpp` (new). Tests:
`tests/mobile_member_{suspend,lease_expiry,network_changed}_test.cpp` (new).
