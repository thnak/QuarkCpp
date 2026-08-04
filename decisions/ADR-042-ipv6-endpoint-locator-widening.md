# ADR-042 — IPv6 support: PAL network seam + cluster Endpoint locator widening

## Status

Accepted

## Question

`quark::Endpoint` (`include/quark/core/cluster.hpp`) carries an opaque `std::uint64_t addr`
interpreted by both PAL network backends (`pal/linux_x86_64/net.hpp`,
`pal/windows_x86_64/net.hpp`) as an IPv4 address packed into the low 32 bits, hardcoded to
`sockaddr_in` on every bind/connect/accept/getsockname call. Quark cannot address an IPv6 peer.

How should `Endpoint`'s locator widen to carry either a v4 or v6 address without breaking the
existing IPv4 wire format and call sites (ADR-018/ADR-021/ADR-039 pass `Endpoint` around per the
task brief — cross-examination independently found this citation to be **factually wrong**, see
Residual risks), how do both PAL backends gain `sockaddr_in6`/`sockaddr_storage` support alongside
the existing `sockaddr_in` path, and how do dual-stack bind/connect and peer
advertisement/resolution (Discovery/SWIM gossip) work — while staying zero-heap-allocation on any
path touched by the mailbox/dispatch hot path and not silently breaking an existing IPv4-only
caller?

## Designs (one-line summaries)

1. **Tagged-Union IpAddr** — `pal::IpAddr{ IpFamily family; std::array<std::byte,16> bytes; }`
   (17 B) becomes `Endpoint::addr`'s type directly (`Endpoint` grows 24 B → 32 B); a non-explicit
   `IpAddr(uint64_t)` converting constructor reproduces the legacy host-packed IPv4 decoding
   exactly, so every existing IPv4 call site recompiles with zero source edits; both PAL backends
   gain a family-branching `to_sockaddr()` codec and in-place-widened `tcp_listen`/`tcp_connect`;
   `TcpTransport` gains a `dual_stack` flag (default `false`, preserving today's `IPV6_V6ONLY=1`
   behavior for any v6 bind).

2. **Always-128-bit Endpoint locator** — `Endpoint::addr` becomes an always-128-bit
   `IpAddr{hi,lo}` with no family tag; every IPv4 peer is stored as its IPv4-mapped IPv6 form
   (`::ffff:a.b.c.d`); both PAL backends switch their **default** socket family to AF_INET6 with
   `IPV6_V6ONLY=0` (dual-stack by default), falling back to the legacy `sockaddr_in`-only path only
   under a build-time `QUARK_NET_IPV4_ONLY` flag.

3. **External v6 locator table** — `Endpoint` gains only a 1-byte trailing `AddrFamily family` tag
   (NSDMI-defaulted to V4, landing in existing tail padding, so `sizeof(Endpoint)` stays 24 B); the
   real 128-bit v6 address for a peer is **not** stored in `Endpoint` at all — it lives in a
   separate `TcpTransport::v6_addrs_` / `SeedListDiscovery` side table keyed by `NodeId`, populated
   once via a new `add_peer(Endpoint, pal::Ipv6Addr)` overload.

## Evidence table

Legend: Survived = survived cross-examination (possibly in revised form). Proven = executed C++
verdict from the prover (CORRECT / WRONG / INCONCLUSIVE — INCONCLUSIVE carries no weight per the
judging rubric).

| Design | Claim | Kind | Survived red-team? | Proven | Number / evidence |
|---|---|---|---|---|---|
| **1. Tagged-Union** | C1 sizeof(Endpoint)≤32, trivially-copyable, standard-layout | correct | yes | **CORRECT** | static_assert compiles across 417 TUs |
| | C2 zero-source-edit IPv4 call-site compat | correct | yes (footgun risk disclosed, roadmap noted) | **CORRECT** | full build + 207/207 (later 203/203 under ASan) ctest pass, samples 15-22 link clean |
| | C3 new codec byte-identical to old `to_sockaddr_in` | correct | yes | **CORRECT** | 1000/1000 random tuples, 0 mismatches |
| | C4 IPv6 loopback handshake, ASan/UBSan clean | safe | yes | **INCONCLUSIVE** | no Linux toolchain in session; substituted by C5 |
| | C5 same test passes on Windows/MSVC | safe | yes | **CORRECT** | OK, exit 0, clean under MSVC ASan |
| | C6 `local_port()` sockaddr_storage fix | safe→correct (reclassified) | yes, revised | **CORRECT** | OLD fn reproduced WSAEFAULT on Windows exactly as predicted; FIXED returns correct port; Linux-side "no overflow" claim conceded true by red-team, not independently re-run here |
| | C7 zero mailbox/dispatch hot-path overhead | fast | yes, tooling corrected | **CORRECT** | grep 0 hits on real hot-path file set; mailbox_bench 78.65→78.76 M msg/s/core, sched_bench 22.7→21.8 M msg/s/core, all within noise, no Hard-budget MISS |
| | C8 dual-stack single-listener accepts v4+v6, checked setsockopt | correct | yes, revised (setsockopt failure now propagated) | **CORRECT** | happy path OK; forced-setsockopt-failure path returns `std::unexpected`, `start()` returns false |
| | C9 add_peer closure allocation delta | safe | yes, revised (framing corrected) | **CORRECT** | pre-patch 1.000 alloc/closure, post-patch 1.000 alloc/closure — delta 0 (SBO already exceeded pre-patch) |
| **2. Always-128-bit** | F1 zero-heap conversions | fast | yes | **CORRECT** | 200k iterations, 0 allocations |
| | F2 sockaddr build cost ≤20% slower | fast | yes (claim itself) | **WRONG** | 1.86-1.90 ns/call → 3.20-3.23 ns/call, **+71%**, over budget |
| | F3 PeerSession cache-line packing | fast | yes, revised (repack) | **CORRECT** | static_assert + offset check; Linux perf_event sub-check unavailable, INCONCLUSIVE for that part |
| | S1 dual-stack concurrent v4+v6, no race | safe | yes, methodology substituted (no TSan on MSVC) | **CORRECT**(non-TSan) | 100/100 runs clean |
| | S2→S2b full call-site compat incl. voice_channel.hpp | correct | **no — original S2 falsified, revised** | **CORRECT** (revised) | diff -rq samples/tests/ empty; 202/202 → 203/203 ASan pass |
| | S3 checked IPV6_V6ONLY, fail-fast | safe | yes, revised (new fix) | **CORRECT** (logic only) | unit-level gate logic verified; real syscall-failure injection not run (no LD_PRELOAD-equivalent on Windows) |
| | S4 local_port sockaddr_storage on both backends | safe | yes, revised | **CORRECT** | positive-only proof; pre-fix WSAEFAULT not re-reproduced |
| | C1-C5 (mapping/round-trip/connect/v4-only fail-fast/scope_id) | correct | yes, C5 (scope_id) added post-attack | **CORRECT** (5/5) | incl. real link-local dial on host interface |
| **3. External table** | C1 Endpoint stays 24 B | correct | yes | **CORRECT** | static_assert + isolated probe |
| | C2 send()/send_on_loop() untouched, no hot-path regression | fast | yes | **CORRECT** | source diff shows 0 changes to send path; bench ranges overlap (noisy, corroborating only) |
| | S1 equal allocation count v4 vs v6 dial | safe | yes, revised (scope narrowed to exclude add_peer) | **CORRECT** | v4=309, v6=309 allocations, exact match |
| | S2 no new race on v6_addrs_ | safe | yes | **INCONCLUSIVE** | no TSan on MSVC; substituted stress (40/40 runs clean) is not race-detector-grade |
| | C3 full call-site compat | correct | yes, revised (false ADR-018/021/039 citation conceded) | **CORRECT** | diff -rq clean outside patched files; 205/205 ctest pass |
| | C4 dual-stack single listener | correct | yes, revised (start()-branch fix added) | **CORRECT** | both families reach St::Open on one listener fd |
| | C5 FIFO delivery over v6 | correct | yes | **CORRECT** | 400 frames/direction, bit-for-bit match to v4 baseline |
| | S3 (new) mis-tagged-Endpoint guard added | safe | yes (fix for red-team-found silent misroute) | **CORRECT** | Release: clean drop; Debug: assert fires |
| | C6 (new) Discovery::resolve_v6 seam | correct | yes (fix for red-team-found seam gap) | **CORRECT** | v6-only seed list bootstraps through Discovery alone |

**Tally (CORRECT / INCONCLUSIVE / WRONG):** Design 1: 8 / 1 / 0. Design 2: 11 / 1 / **1**. Design 3: 8 / 1 / 0.

## Decision

**Winner: Design 1 — Tagged-Union IpAddr.**

Reasoning against the ranking order given:

1. **Safety gate.** No design has a *safe*- or *correct*-kind claim marked WRONG, so the hard gate
   does not eliminate anyone outright. Design 2's one WRONG claim (F2, sockaddr-build cost +71%
   over its own stated 20% budget) is kind `fast`, not `safe`/`correct`, so it does not trigger the
   gate by the letter of the rubric — but it is weighed under rule 2/4 below.

2. **Proven beats claimed.** Designs 1 and 3 both closed cross-examination with **zero** disproven
   claims — every attack that landed was answered with a concrete design revision (Design 1: C6's
   Windows-specific buffer fix, C8's setsockopt-failure propagation, C9's honest re-framing; Design
   3: S3's mis-tagged-Endpoint guard, C6's Discovery seam, C4's start()-branch fix) and the revised
   claim was then proven CORRECT. Design 2 has one claim (F2) that failed even after revision — its
   own stated performance budget was not met by a wide margin (+71%, not "within noise"). That is a
   genuine, not merely tooling-related, empirical miss.

3. **Bends a core invariant (rule 4, applied before comparing raw counts).** Design 2's mechanism
   for "dual-stack by default" is to make **every** socket, including ones a caller only ever
   intended to be IPv4, an `AF_INET6` socket with `IPV6_V6ONLY=0`, with the only way back to the
   legacy IPv4-only path being a **compile-time** flag (`QUARK_NET_IPV4_ONLY`) the operator must set
   in advance. On any host or container image with IPv6 disabled or restricted at the kernel level
   — common in minimal/sandboxed deployments, explicitly flagged as a residual risk by the design's
   own author — every existing pure-IPv4 caller's dial/listen silently starts failing at runtime
   with no source change on the caller's part. That is exactly the failure mode the task's
   invariant ("does not silently break an existing IPv4-only caller") rules out; a build flag set by
   an operator who does not yet know they need it is not a caller-side escape hatch. Designs 1 and 3
   both keep IPv4 the unconditional, unchanged default and require an explicit, per-call opt-in
   (`IpAddr::v6_*()` / a distinct v6 constructor overload) to touch `AF_INET6` at all — no existing
   deployment's socket family changes under them. This disqualifies Design 2 from winning regardless
   of its otherwise-higher raw CORRECT count (11 vs. 8), consistent with rule 4's precedence over
   rule 3's raw-count/measured-numbers comparison.

4. **Design 1 vs. Design 3, among the two safety survivors.** Design 1 is preferred for three
   concrete reasons:
   - **It actually answers the question asked.** The task asks how `Endpoint`'s *locator* widens to
     carry a v4-or-v6 address. Design 1's `Endpoint::addr` **is** the widened locator — a peer's
     full address travels with the value. Design 3 does not widen the locator at all: `Endpoint`
     keeps its 8-byte `uint64_t addr` field family-blind and tags only which family a peer belongs
     to; the actual 128-bit v6 address lives in a side table the value type never carries. Cross-
     examination confirmed this is architecturally real, not cosmetic — it produced a genuine latent
     bug (a mis-tagged `Endpoint` silently misrouting a v6 peer's traffic to loopback, fixed only
     after red-teaming via new claim S3) precisely because two separate data structures must stay in
     sync for one logical peer address.
   - **More targeted, directly-measured hot-path evidence (rule 3, among survivors).** Design 1's
     C7 benchmarked the actual mailbox/dispatch/scheduler suite named in the invariant
     (`mailbox_bench`, `sched_bench`) before and after the patch and found every number within
     existing run-to-run noise, with **zero** Hard-budget misses. Design 3's C2 benchmarked
     `TcpTransport::send()` call latency/throughput — a real but less targeted proxy (it is
     connection-plane, not the mailbox/dispatch core itself) — and its own reported ranges overlap
     so widely (baseline p50 5.6–7.5 µs vs. patched p50 5.5–7.8 µs) that the evidence is explicitly
     reported as "corroborating rather than dispositive."
   - **Design 3's own residual gap needed two follow-on fixes mid-debate** (the Discovery seam for
     V6 seed bootstrap, and the mis-tagged-Endpoint silent-misroute guard) to reach parity with what
     Design 1 delivered in its first pass — a sign the split-locator architecture is more failure-
     prone to extend correctly, even though both ultimately closed clean.

   Design 1's own residual gap (C4 INCONCLUSIVE — no Linux ASan/UBSan run in this session) is judged
   non-fatal: the identical test source was proven CORRECT on Windows/MSVC (C5) and separately under
   MSVC's `/fsanitize=address` (a real, if narrower-coverage, sanitizer), and nothing in the design
   is platform-conditional in a way that would make the Linux leg plausibly behave differently — see
   Residual risks for the concrete follow-up this still requires before full sign-off.

**Winner: Tagged-Union IpAddr (Design 1).**

## Residual risks

- **C4 not run on Linux/GCC+Clang under ASan+UBSan in this proving session** (no toolchain
  available on the Windows-only sandbox). This is the single tie-breaking experiment still owed
  before this ADR's evidence set is complete: run
  `taskset -c 0-3 ctest --test-dir build-asan -R tcp_transport_ipv6_loopback_test --output-on-failure`
  on the project's actual Linux CI image. Nothing in the design is expected to be Linux-specific,
  but this is asserted, not yet proven, on that backend.
- **The implicit `IpAddr(std::uint64_t)` converting constructor is a permanent, disclosed footgun.**
  A future call site that passes an unrelated `uint64_t` (a `NodeId.value`, a hash) into an
  `IpAddr`-typed parameter compiles silently and is misinterpreted as an IPv4 host-packed address —
  demonstrated compiling with zero warnings under `-Wall -Wextra`. Mitigation is a documented
  process commitment (code review requires new call sites to use `IpAddr::v4()/v6()` explicitly),
  not a type-system guarantee — see spec recommendation below for a scheduled hardening step.
- **`IpAddr` has no `scope_id`/zone-index field**, so link-local IPv6 addresses (`fe80::/10`) are
  not correctly representable/resolvable on a multi-homed host under this design as proven (Design
  2's cross-examination independently added and proved a `scope_id` fix for its own `Endpoint`; the
  equivalent field was not added to Design 1's `IpAddr`/`Endpoint` in this debate). This should be
  folded into the accepted design before it ships: add a trailing `std::uint32_t scope_id = 0` to
  `Endpoint` (not `IpAddr`, to keep `IpAddr`'s 16-byte RFC-portable shape for wire/hash use) — every
  real call site in the repo constructs `Endpoint` with 3 positional aggregate-init args, so this is
  provably a zero-source-edit addition (verified by grep against the real repo in the sibling
  design's cross-examination) with `to_sockaddr` setting `sin6_scope_id` from it.
- **Discovery/SWIM gossip still cannot propagate a resolved peer address to third parties.**
  `MemberUpdate` carries only `NodeId` + incarnation + status today, for either family — this was a
  pre-existing gap the target flagged as in-scope to design for ("peer advertisement/resolution
  (Discovery/SWIM gossip)") but Design 1 only hands the follow-on work a ready
  `encode_endpoint`/`decode_endpoint` byte-writer primitive, not a wired implementation. A follow-on
  ADR is required to actually extend `ControlMsg`/`MemberUpdate`'s wire format; until then, IPv6
  peers must still be registered via out-of-band `add_peer()` calls, same as IPv4 peers are today.
- **`IPV6_V6ONLY`'s OS default differs by platform** (Linux: usually 0, sysctl-tunable; Windows: 1).
  Design 1's `tcp_listen` now sets it explicitly and checks the return value (C8), which closes the
  silent-failure gap the red team found — but this explicit-set-and-check behavior itself was only
  verified via a forced-failure interposer on Windows/MSVC in this session, not against a real
  restrictive container policy on Linux.
- **`Endpoint` growing 24 B → 32 B** was not checked against any bulk/cached-`Endpoint` structure in
  026's large-scale topology layer (VirtualBins) — grep found no such usage during design, but that
  subsystem was only spot-checked, not read in full.
- **This decision's benchmark evidence for C7 could not run the full 023 `bench-gate` aggregate
  target** in this session (an unrelated pre-existing bench, `bench/serialize_bench.cpp`, uses
  GCC-style inline `asm` unsupported by MSVC's `cl.exe`); the relevant individual benches
  (`mailbox_bench`, `sched_bench`) were run directly instead. Re-run the full `bench-gate` target on
  Linux/GCC or Linux/Clang to confirm no other benchmark in the gate regresses.

## Spec recommendations

### `019-Platform-Abstraction-Layer.md`

- **§"2. Async I/O event loop + sockets (010)"**: document the new `pal::IpAddr` type (family tag +
  16-byte network-order array, living in a new portable `pal/ip_addr.hpp` with no OS headers,
  included by both backend `net.hpp` files) as the PAL's address-locator type, alongside the
  existing socket primitives. State explicitly that `to_sockaddr(const IpAddr&, uint16_t) ->
  {sockaddr_storage, socklen_t}` is the *only* place either backend is permitted to branch on
  address family via `sockaddr_in`/`sockaddr_in6` — no `std::variant<sockaddr_in,sockaddr_in6>`, no
  virtual dispatch, per "no RTTI/reflection on the hot path" and "The one rule" (PAL is the only
  OS/arch seam).
- **§"2. Async I/O event loop + sockets (010)"**: record the `local_port()` sockaddr_storage-vs-
  sockaddr_in buffer-size finding as a documented PAL invariant: any function that calls
  `getsockname`/`getpeername` on a socket that may be `AF_INET6` MUST size its buffer for
  `sockaddr_storage`, not assume `sockaddr_in`'s 16 bytes — this was proven Windows-breaking
  (WSAEFAULT) once dual-stack/AF_INET6 listeners exist, and the mitigation should be a named rule,
  not tribal knowledge.
- **§"Support matrix (ADR-026, proven ...)"**: add a row (or footnote) recording that IPv6
  dual-stack (`IPV6_V6ONLY=0`) accept-both-families-on-one-listener behavior is proven on Windows
  (this ADR) and still owed a Linux/GCC+Clang run before it can be marked proven there (see Residual
  risks) — do not silently upgrade the matrix's Linux cell to "proven" until that run happens.
- **§"Not in the PAL (non-goals)"**: explicitly note that per-address `scope_id`/zone-index handling
  for link-local IPv6 (`fe80::/10`) is *not yet* implemented (tracked as a residual risk of this ADR,
  targeted at a small follow-on to `Endpoint`, not `IpAddr`) — callers must not assume link-local
  peers are addressable until that lands.

### `010-Distribution.md`

- **§"Transport seam"**: update `Endpoint`'s definition to `NodeId node; pal::IpAddr addr; uint16_t
  port;` (32 bytes, was 24), and note the backward-compatible `IpAddr(uint64_t)` converting
  constructor that keeps every 3-arg `Endpoint{node, addr, port}` call site (including the legacy
  `pal::ipv4_loopback`-style constants) compiling unchanged. Flag the converting constructor's
  footgun explicitly in prose (a stray `uint64_t` silently becomes a v4 address) and record the
  planned hardening: gate it behind an opt-in `QUARK_LEGACY_IPV4_CTOR` flag (default ON for one
  release, then OFF) once new call sites have migrated to `IpAddr::v4()/v6()`.
- **§"Transport seam"**: document `TcpTransport`'s new `dual_stack` constructor parameter (default
  `false`, preserving today's exact `IPV6_V6ONLY=1` IPv4-compatible behavior for any bind), and that
  `tcp_listen` now checks and propagates the `IPV6_V6ONLY` `setsockopt` failure instead of silently
  proceeding — a listener that cannot achieve the family posture it was asked for now fails `start()`
  loudly rather than reporting success while silently serving only one family.
- **§"Membership"** and **§"Cross-node broadcast fan-out (Draft — ADR-019)"**: record the open gap
  that `MemberUpdate`/`ControlMsg` gossip still carries no address for either family (pre-existing,
  unchanged by this ADR) and that this ADR's `pal::IpAddr`/`Endpoint` shape hands a follow-on ADR a
  ready, fixed-size `encode_endpoint`/`decode_endpoint` wire primitive (15 B for V4, 27 B for V6) —
  but that primitive is not wired into any gossip path yet and must not be assumed to exist by any
  caller.
- **§"Alternatives considered"** (under Transport seam): add a paragraph recording why an
  always-128-bit / IPv4-mapped `Endpoint` locator (no family tag) was considered and rejected —
  it required making `AF_INET6` + `IPV6_V6ONLY=0` the *default* socket family for all connections,
  which breaks existing pure-IPv4 deployments at runtime on any host with IPv6 disabled/restricted
  unless a compile-time flag is proactively set, violating the "no silent break for IPv4-only
  callers" invariant — and that its own measured sockaddr-construction cost (+71% vs. the legacy
  path, single-core microbench) missed its own stated performance budget.
