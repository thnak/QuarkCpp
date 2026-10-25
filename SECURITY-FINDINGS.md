# Security Findings — Quark Distributed Wire Boundary

**Date:** 2026-10-25  
**Target:** Quark actor engine (HEAD `a602f29`), `net::TcpTransport`, `SecureTransport`, SWIM
membership, `DistributedRouter`, 016 tagless wire path  
**Environment:** live 3-node cluster on `thnak-server` (Ubuntu, g++ 15.2, mbedTLS 3.6.5), plus
ASan+UBSan builds; real TLS 1.3 mutual handshakes with per-node certificates  
**Method:** live bank-ledger cluster + attackers (plaintext, authenticated node, on-path MITM).
Every finding below was reproduced end-to-end; no finding is inferred from source alone.

---

## Executive summary

A live bank cluster was built with a `Bank` actor (`alice=1000`, `bob=500`, `thief=0`) behind an
ACL that requires `rights=0x4` for `Transfer`. Attackers drained the ledger, evicted the ledger
node from the cluster, crashed it, and did the same again over **real mTLS**.

**Proven:** virtual theft (plaintext and authenticated insider), control-plane impersonation,
AEAD associated-data omission, remote heap OOB, pre-auth memory growth, unauthenticated frame
injection, and a wire-principal reclaim crash.

**Patched in this change:** AEAD AAD coverage, SWIM control-plane sender binding, third-party
`Left` rejection, `to` validation, pre-auth handshake caps, bounds-checked tagless decode.

**Remaining (design/config, not safely patchable in one wire change):** the plaintext transport is
still unauthenticated by design; a `Principal` carried by a peer is still self-asserted (an
authenticated node can claim `rights`); `SecurityMode::Strict` is not wired into startup; the
ADR-044 `pool.sink()` sample pattern is unsafe for principal-bearing wire traffic.

**Post-patch verification:** 18 targeted CTest suites + `security_wire_hardening_test` pass; the
MITM principal patch now fails the AEAD tag; forged `Left` over mTLS is rejected; malformed
tagless frames are dropped; pending handshakes cap at 256.

---

## Findings

| ID | Severity | Finding | Status |
|---|---|---|---|
| F1 | Critical | Plaintext cluster accepts unauthenticated HELLO + Data/Control frames; anyone can move money | Open (config: use mTLS + enforce Strict) |
| F2 | Critical | Authenticated peer can self-assert `Principal.subject/rights`; insider steals with `rights=0x4` | Open (design: bind principal to peer) |
| F3 | Critical | AEAD AAD omits `principal`, `mode`, `kind`, `deadline_ns`, `trace_id`; on-path attacker rewrites authz input | **Patched** |
| F4 | High | SWIM trusts payload `m.from`/`m.subject`, not the authenticated frame sender; forged `Left` evicts the ledger | **Patched** (authenticated links) |
| F5 | Critical | Receiver runs unchecked `decode_tagless` on peer-controlled bytes; truncated length → heap OOB read/leak/crash | **Patched** |
| F6 | High | Pre-auth `pending_` handshakes never capped/reaped; spoofed `from` ids grow memory without bound | **Patched** |
| F7 | High | No `f.to == self_` validation; a frame addressed to another node is applied locally | **Patched** |
| F8 | **Critical (RCE)** | ADR-044 envelope reclaim with the sample-default `pool.sink()` calls `principal.subject` as a function pointer; attacker chose `hijack()` and executed it | **RCE proven** (no-PIE victim); framework default still unsafe |
| F9 | Medium | `SecurityMode::Strict` is not enforced anywhere outside tests | Open (config wiring) |

---

## F1 — Plaintext transport is unauthenticated: virtual theft

The default `TcpTransport` accepts a HELLO naming **any** NodeId and then processes Data/Control
frames from that connection. There is no challenge, no key, and no check that `frame.from` matches
the HELLO identity. The protected `SecureTransport` is opt-in only; `SecurityMode::Strict` does not
reject plaintext at startup (F9), and every sample 15–21 runs plaintext.

Reproducer: `bank_attack.py` connects with HELLO `666`, sends a Transfer for actor `Bank` with
`principal.rights=0x4`.

```
EVENT authz ... subject=0   rights=0 required=4 allowed=0        # anonymous: denied (control)
EVENT authz ... subject=666 rights=4 required=4 allowed=1        # forged principal: allowed
EVENT bank_transfer from=1 to=666 amount=150 alice=820 ... thief=150
...
EVENT bank_transfer from=2 to=666 amount=500 alice=200 bob=50 thief=1250
```

Result: `alice 1000→200`, `bob 500→50`, `thief 0→1250`; the ledger total (1500) is conserved —
this is a valid transfer set, not a corrupted sum.

**Fix direction:** treat plaintext as dev-only at runtime (wire `validate_security` into
`validate_engine_config`/bootstrap), and never run a multi-node ledger on an unauthenticated
transport. F3/F5/F7 reduce the damage but cannot authenticate a plaintext peer.

---

## F2 — Authenticated insider self-asserts the Principal

Even over a **real mTLS** session (valid CA-signed cert, CN `quark:<cluster>:0`), the peer chooses
`MessageFrame::principal`. The boundary `Authorizer` sees the peer-supplied `rights` and allows the
transfer. The transport authenticates the **node**, not the principal on the frame, and there is no
node→principal binding or ingress signature.

`mtls_bank_attacker ... theft` (authenticated as node 0, cert valid):

```
EVENT authz ... subject=0   rights=0 required=4 allowed=0
EVENT authz ... subject=666 rights=4 required=4 allowed=1
EVENT bank_transfer from=1 to=666 amount=150 ... thief=150
... five transfers plus bob's 500 ...
EVENT bank_transfer from=2 to=666 amount=500 alice=200 bob=50 thief=1250
```

**Fix direction:** the principal must be bound to the authenticated peer: either (a) the transport
stamps the principal from the session identity (node id + an attested/ingress-signed delegation),
or (b) the `Authorizer` is given the authenticated node id out-of-band and refuses a frame whose
principal was not issued to that node. Rights in a peer-controlled frame must never be the
authorization input.

---

## F3 — AEAD associated data omitted the fields the receiver acts on (PATCHED)

`SecureTransport` seals only `MessageFrame::payload`. `build_aad` covered
`from/to/target/msg_type/lane/seq` but **not** `principal`, `mode`, `kind`, `deadline_ns`, or
`trace_id`. Those fields travel in the clear. An on-path proxy flipped 8 bytes of
`principal.rights` from 0 to 4; the tag still verified and the bank executed the transfer:

```
# direct authenticated client, anonymous principal:
EVENT authz ... subject=0 rights=0 required=4 allowed=0
# through mitm.py --rights 4:
EVENT authz ... subject=0 rights=4 required=4 allowed=1
EVENT bank_transfer from=1 to=666 amount=100 ... thief=100
```

The same omission let an attacker flip `mode` (selecting the unchecked decoder) and `kind`
(Data↔Authenticate share lane 0).

**Patch:** `build_aad` now binds `mode`, `kind`, `deadline_ns`, `trace_id`, `principal.subject`,
and `principal.rights` (`include/quark/core/secure_transport.hpp`). Post-patch, the proxy's edit
produces `tamper_rejected` and no delivery; the direct anonymous frame is still denied.

---

## F4 — SWIM control-plane impersonation / forged eviction (PATCHED)

`SwimMembership::handle_control` trusted `ControlMsg::from`/`subject` from the decrypted payload,
never comparing them to the transport-authenticated sender. One admitted node could therefore
speak as any other node, and `apply_update` accepted a second-hand `Left` (it is authoritative and
cannot be refuted). Sending `m.from=1, updates=[{node=1, inc=UINT64_MAX, Left}]` to node 2 removed
the ledger from node 2's roster (`status1=3`, `roster=2,3`) and broke placement.

The same frame over **real mTLS** was accepted before the patch; post-patch it is rejected
(`status1=0`). Plaintext still spoofs it trivially because there is no authenticated sender at all
(F1): the attacker sets both `frame.from` and `m.from` to 1, even though its HELLO said 667.

**Patch** (`include/quark/core/cluster.hpp`):
- `handle_control(m, authenticated_from)` drops any frame with `m.from != authenticated_from`.
- A gossip `Left` is accepted only for the sender's own node (`u.node == m.from`); a peer cannot
  tombstone a third party.
- `on_frame` drops frames with `f.to != self_`.

Regression coverage: `security_wire_hardening_test`.

---

## F5 — Unchecked tagless decode on peer-controlled bytes (PATCHED)

`DistributedRouter::inbound_thunk` ran the **unchecked** `decode_tagless` whenever the frame said
`mode=Tagless`. The tagless format has no envelope; a peer controls its own plaintext, so a short
payload with a large declared string/vector length made `std::string::assign` /
`std::vector::resize` read far past the payload. ASan (plaintext and real mTLS):

```
ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 64 ... thread T1
#6 quark::detail::tagless_read_value<std::string> ... wire.hpp:406
#10 quark::decode_tagless<Transfer> ... wire.hpp:497
#11 quark::DistributedRouter::inbound_thunk<Bank, Transfer> ... distribution.hpp:198
```

In a non-sanitizer build the over-read bytes are placed in the actor's `memo` and printed
(heap data leak); a larger length crashes the node.

**Patch:** `wire.hpp` adds `decode_tagless_checked(buf, len, out)`, a bounds-checked mirror of the
writer (truncation, declared length > remaining, multiplication overflow, and a 1M element cap are
all rejected). `inbound_thunk` uses it for every wire frame. Post-patch the same frames are
dropped with no sanitizer report.

---

## F6 — Pre-auth handshake state growth (PATCHED)

Every `FrameKind::Authenticate` frame with a new `frame.from` constructed and parked a
`HandshakeEngine` in `SecureTransport::pending_`. There was no cap and no reaper: an entry is only
removed when a later frame arrives from the **same** NodeId. A partial ClientHello returns
`WANT_READ`, so an unauthenticated client can mint one real mbedTLS engine per spoofed NodeId.

Evidence: `pending_handshakes=20000` (mock engine) and `pending_handshakes=3000` with **real
mbedTLS 3.6.5** engines, all from one connection that never completed a handshake.

**Patch:** `SecureTransport::kMaxPendingHandshakes = 256`, checked at all three engine-creation
sites; `kMaxAuthenticateFrameBytes = 64 KiB` before an engine is handed the bytes; and the mbedTLS
adapter caps its own input buffer at 64 KiB. Post-patch the same flood reports
`pending_handshakes=256`.

---

## F7 — Missing destination check (PATCHED)

Neither `DistributedRouter::deliver` nor `SwimMembership::on_frame` checked `f.to == self_`. A
peer could aim a frame at NodeId 999 and the receiving node still applied it. `f.to` is now
enforced at both entry points. Regression: `security_wire_hardening_test` and
`distribution_routing_test`.

---

## F8 — ADR-044 reclaim-sink contract: remote crash → attacker-controlled call (RCE)

ADR-044 deliberately introduced `EnvelopePool` for non-anonymous principals and documented that an
activation receiving wire principals **must** be constructed with `LocalRouter::reclaim_sink()`,
not `MessagePool::sink()` — the latter reinterprets `DescriptorEnvelope::principal` bytes as a
`MessagePool::Cell::destroy` function pointer. Every public sample (01–22, README, wiki) uses
`pool.sink()`, which is safe only because those actors never receive a non-anonymous principal.

First proof was a crash on the second non-anonymous frame, with `pc=0x29a` — exactly the
`principal.subject` value `666`:

```
ERROR: AddressSanitizer: SEGV on unknown address 0x00000000029a (pc 0x00000000029a ...)
```

**Second pass — full control-flow hijack:** the victim was rebuilt from the same sample pattern
(`pool.sink()`) with `-no-pie` and a never-referenced `extern "C" void hijack(void*)` that writes
`EVENT RCE_HIJACK` and calls `_exit(42)`. The attacker learned its address (`0x4062d6`) from the
startup line and sent one Transfer with `principal.subject = 0x4062d6, rights=4`:

```
EVENT authz ... subject=4219606 rights=4 required=4 allowed=1
EVENT bank_transfer from=1 to=666 amount=1 ... principal_subject=4219606 rights=4
EVENT RCE_HIJACK
victim exit code: 42
```

`4219606 == 0x4062d6`. This is arbitrary code execution in the victim process driven by a wire
field (plaintext, or mTLS because the principal is authenticated but unbound — F2). Caveat: the PoC
uses a no-PIE victim, so the code address is known; on a PIE/ASLR build an info leak or partial
overwrite is needed to complete the chain. The primitive itself is proven.

Fix: make the safe dual sink the default for any activation whose actor can be registered with
`DistributedRouter::register_remote`, or fail closed (drop the frame) when a non-anonymous
principal targets an activation not wired with the dual sink. At minimum, the sample/README pattern
should use `reclaim_sink()` whenever distribution is enabled.

---

## F9 — `SecurityMode::Strict` is a dead knob

`validate_security` exists and has unit tests, but no production path calls it:
`grep -rn validate_security` finds only `security.hpp` and
`tests/security_config_validation_test.cpp`. A cluster configured `Strict` with the plaintext
transport starts normally. Wire it into `validate_engine_config` (with the configured cluster size
and transport kind) before F1 can be closed operationally.

---

## Patch summary

| File | Change |
|---|---|
| `include/quark/core/secure_transport.hpp` | AAD binds `mode/kind/deadline/trace/principal`; `kMaxPendingHandshakes=256`; `kMaxAuthenticateFrameBytes=64 KiB` |
| `include/quark/adapters/mbedtls/mbedtls_handshake.hpp` | 64 KiB buffered-handshake cap |
| `include/quark/core/cluster.hpp` | `m.from == authenticated_from`; no third-party gossip `Left`; `f.to == self_` |
| `include/quark/core/distribution.hpp` | `f.to == self_`; checked tagless decode on the wire |
| `include/quark/core/wire.hpp` | `decode_tagless_checked` (bounds-checked writer mirror) |
| `tests/security_wire_hardening_test.cpp` | regression test for AAD, SWIM binding, checked tagless, pending cap |

Verification on `thnak-server` (patched headers):

```
100% tests passed, 0 tests failed out of 18
# plus: build-tests/tests/security_wire_hardening_test -> OK
```

Post-patch bank run: mTLS forged `Left` rejected (`status1=0`), MITM `rights` patch rejected
(no `allowed=1`/transfer), malformed tagless dropped (no ASan report), pending flood capped at
256. The authenticated insider principal theft (F2) remains reproducible by design.

---

## Reproducers

All scripts and binaries are in `~/quark-sec/sec` on `thnak-server` (source at `~/quark-sec`):

```bash
bash run_all.sh     # plaintext + mock-secure: control injection, principal, tagless, AAD MITM, handshake leak
bash run_mtls.sh    # real mTLS certs + authenticated insider, MITM, tagless, pending cap
bash run_bank.sh    # 3-node bank cluster: theft, eviction, crash; plaintext + real mTLS + MITM
```

They build with `g++ -std=c++23`, link `-lmbedtls -lmbedx509 -lmbedcrypto`, and generate a test
CA + leaf certs (`CN=quark:12648430:<node>`) with OpenSSL. ASan builds are `*/build-*` variants in
the scripts.

---

## Residual risks / recommended next steps

1. **Bind the principal (F2, Highest).** Give `Authorizer`/`DistributedRouter` the authenticated
   peer NodeId and reject a frame whose principal was not issued to that node; propagate ingress
   principals only from an authenticated gateway.
2. **Enforce Strict (F9).** Reject `Strict + cluster_size>1 + plaintext` at startup; make the
   default multi-node transport mTLS, not `TcpTransport`.
3. **Safe reclaim sink (F8).** Default activations that can receive wire messages to the dual
   sink, or fail closed on a non-anonymous principal when they are not.
4. **Schema negotiation on receive.** F5 now bounds the decode, but tagless still trusts the
   sender's claimed mode. Verify the peer's advertised `PeerSchema` before accepting Tagless.
5. **DoS is still a non-goal** (020): the caps above close the specific unbounded-state bug, not
   volumetric exhaustion; that stays a load-balancer/kernel concern per 022.

---

## Second pass — every candidate vector executed (2026-10-25)

Lab artifacts: `~/quark-sec/sec` on `thnak-server` — `run_batch1.sh`, `run_forward.sh`,
`run_place.sh`, `run_slot_exhaust.sh`, `run_flood.sh`, `run_big.sh`, plus `relay_probe`,
`fuzz_wire`, `schema_probe`; logs in `logs_batch1/2/3`. The runtime is HEAD `bbcf60e` (patched);
the only deliberate reversion is F8's sample-default `pool.sink()` victim.

### Newly proven

| ID | Vector | Evidence | Impact | Status |
|---|---|---|---|---|
| F10 | Fail-open authorization | No `set_authorizer` (the sample pattern); an anonymous `Transfer` executed: `subject=0 rights=0` moved 100; a rights=4 frame moved 500; `thief=600` | Any peer can invoke every registered remote type | Open |
| F11 | Internal-forward ACL bypass | Gate/`Forward` needs rights=1; its handler `local.tell<Bank>(Transfer)` reached the rights=4 Bank: `gate_forward rights=1` → `bank_transfer from=1 to=666 amount=1000 alice=0 thief=1000 rights=1` | Low-privilege wire caller drains a privileged actor through a forwarder | Open (app/framework API gap) |
| F12 | SWIM `Alive` gossip → placement hijack | Spoofed control sender injected 512 ghost Alive members; `roster=1 → 1,10000..10511`, `owner=1 → 10199`, remote `bank_ping` stopped while local ledger traffic continued | Ghost-node placement, dead-lettered traffic, service loss | Open (SWIM gossip semantics) |
| F13 | Handshake-slot exhaustion | 3000 partial ClientHellos filled 256 slots; legitimate node-0 mTLS handshake failed immediately and after 6s; `pending=256` persisted | Fixed-capacity pre-auth DoS on new joins | Open |
| F14 | Relay-token forgery / liveness poisoning | Default `seed=0` ⇒ public `relay_key`; forged PingReq as node 1 + forged Ack as node 3 with the computed token flipped node 1's view of node 3 from `Suspect(1)` to `Alive(0)` | Suppress/defeat failure detection (keep a dead node Alive) | Plaintext proven; mTLS sender binding blocks the spoof |
| F15 | Queue/backlog amplification | 10,000 authorized frames in 0.12s; 1,000 processed at first sample; RSS 6.5→9.7MB and still draining; all 10k passed authz | One peer can make an actor unresponsive and grow the backlog without bound | Open (022/backpressure) |
| F16 | Large-frame memory amplification | One 32MiB Data frame (no decode; target type 0) raised `VmHWM` 6,520→71,656KB (~2×); `max_frame_body=64MiB` per frame/connection | Per-peer/aggregate memory exhaustion | Open (volumetric; 020 non-goal) |
| F17 | Tagless schema mismatch | V1 bytes decoded as V2 and vice versa: `amount=8589934593 from=1000 to=0`, both accepted | Silent field reinterpretation in mixed-version clusters without `PeerSchema` enforcement | Open (checked decoder prevents OOB) |
| F18 | RCE via F8 | `principal.subject=0x4062d6` → `EVENT RCE_HIJACK`, victim exit 42 | Arbitrary code execution (no-PIE PoC) | Open; see F8 |

### Tested and blocked / not a finding

- **Tagless OOB at HEAD**: dropped by `decode_tagless_checked`; no ASan report.
- **mTLS forged `Left`**: rejected by `m.from == authenticated_from`; `status1=0`.
- **mTLS AAD tamper**: rejected; no delivery.
- **mTLS identity spoofing**: cannot set `frame.from` to another node without its private key; the session + AAD bind it.
- **Decoder fuzz**: 2,000,000 random inputs across `decode_tagged`, `decode_control`,
  `decode_tagless_checked` under ASan+UBSan — no finding (coverage, not proof of absence).
- **Type-key collision**: not attempted/found; 64-bit hash, low practical risk.
- **Log injection**: the demo prints `memo` as hex; app-level only.
- **Metrics/health endpoints**: none in this codebase; N/A.
- **At-rest encryption**: no encryption mention in `PersistenceAdapters.md`; adapter-dependent,
  separate storage threat, not a network finding.

### Supply chain / package injection (inspection, not exploited)

- CI pipes `wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 20` with no checksum or
  signature — runner RCE if that endpoint/network is compromised.
- `actions/checkout@v4` and `actions/upload-artifact@v4` are mutable tags, not commit SHAs.
- `apt-get install` is unpinned; `windows-latest`/MSVC is unpinned.
- `vcpkg.json` pins a `builtin-baseline` (good) and requires `mbedtls>=3.6.2`; the CMake
  `find_package(MbedTLS CONFIG QUIET)` / `find_library` fallback accepts a system mbedTLS with no
  version or hash check — a malicious sysroot/`LD_LIBRARY_PATH` can inject.
- No `FetchContent`/`ExternalProject` remote downloads were found (good).

Fix: SHA-pin actions, verify the LLVM script and apt packages, require the vcpkg manifest or a
version/hash-checked mbedTLS, and add an SBOM/provenance gate.

### Priority order after the second pass

1. **F8 / F18** — RCE: default the dual reclaim sink or fail closed.
2. **F2** — bind the Principal to the authenticated peer.
3. **F10** — deny by default when any remote type is registered.
4. **F12** — membership admission/verification and a live-member cap; don't place on unverified peers.
5. **F13** — global handshake reaper + per-IP/per-connection cap + reserved slots.
6. **F11** — local authorizer hook / require privileged handlers to check `ctx.principal`.
7. **F15 / F16** — per-peer admission control, bounded mailboxes, aggregate frame budgets.
8. **F14** — random per-process relay secret; bind the token to the authenticated peer.
9. **F17** — enforce `PeerSchema` on connect; tagged fallback by default.
10. **Supply chain** — pin actions to SHAs; verify the LLVM/apt inputs; vcpkg-only mbedTLS.

---

## Remediation pass (2026-10-25) — statuses below supersede the matrix above

Implemented and re-tested on `thnak-server` (real mTLS 3.6.5, ASan+UBSan where applicable).
**21/21 affected CTest suites pass**, including the new placement/principal/verification regressions.

| ID | Fix | Post-fix evidence |
|---|---|---|
| F8/F18 | `ReclaimSink::dual` + `Activation::accepts_envelopes()`; wire/local delivery **drops** a non-anonymous descriptor for a non-dual activation instead of reclaiming it | RCE victim (`pool.sink()`, no-PIE) now exits **0**; no `RCE_HIJACK`, no attacker transfer. `local.reclaim_sink()` remains the supported path |
| F2 | `DistributedRouter::set_principal_policy`; **default denies** non-anonymous wire principals. A policy maps `(authenticated peer, claimed)` → effective principal or nullopt | mTLS insider theft produces no `authz`/`bank_transfer`; `security_boundary_wire_test` opts in with an identity binder and still passes |
| F10 | `DistributedRouter::deliver` **denies when no Authorizer is installed** (fail-closed). `AllowAllAuthorizer` is the explicit opt-in | No-auth bank victim: the anonymous 100 transfer disappears; only local traffic. Samples 08/15–19/22 install `AllowAllAuthorizer` explicitly |
| F12 | `Member::verified`; **placement_view()** excludes gossip-only members; `Join` may only admit the sender itself; `max_live_members=4096`; local Suspect/Dead never verifies; `view()` keeps convergence/probing | 512 ghost `Alive` members in the convergence roster, but `owner=1`, `pview=1`, remote pings continue; `cluster_swim_partition_heal` + join/rejoin pass; gossip member is in view, not placement, until a Direct Ack |
| F13 | Global `reap_stale_pending` (any Authenticate frame frees entries older than 5s); optional `set_handshake_admission(NodeId)` hook for known-peer reservation | First legitimate mTLS handshake during the flood fails; after 6s the retry **succeeds**, `pending=0`. The 5s window remains (per-IP reservation is a deployment hook, not plumbed address-level) |
| F11 | `LocalRouter::set_authorizer` — optional ACL on `tell`/`ask`/`ask_stream` before descriptor construction | Forward probe: Gate boundary allowed (rights=1), local Bank/Transfer **denied** (required 4); no transfer. Opt-in; default internal trust unchanged |
| F15 | Per-peer token bucket (`set_peer_budget`, default 4096/s, burst 1024, fixed 256-bucket table) before authz/decode | 10,000-frame flood in 0.03s → **1,277** admitted; RSS 6.5→7.1MB (was 6.5→9.7MB and growing); actor no longer buried |
| F16 | `max_frame_body` 64MiB → **4MiB** per frame | 32MiB frame → connection reset; `VmHWM 6,584→6,712KB` (was 71,656KB). Large payloads must use 024 streaming/chunking |
| F14 | Per-instance random relay secret (`Config::relay_secret`, 0 ⇒ random); token no longer derivable from seed+NodeId | Forged relay Ack no longer flips node 3: `Suspect(1)` stays `Suspect`; `relay_probe` exits 1. mTLS sender binding already blocked impersonation |
| F17 | **Tagged is the default** (`peer_schema_` placeholder mismatch); tagless requires `set_peer_schema_provider(matched_peer_schema)` | `distribution_routing_test` explicitly opts into tagless; `tcp_distribution_integration_test` passes on tagged; checked tagless remains |
| Supply chain | `actions/checkout@11d5960…` and `upload-artifact@ea165f8…` pinned to commit SHAs; `/tmp/llvm.sh` downloaded + SHA-256 verified (`03878e08…`) before `sudo bash`; `find_package(MbedTLS 3.6.2)` + compile-time `MBEDTLS_VERSION_NUMBER >= 0x03060200` | CI no longer pipes a remote script into sudo; no `FetchContent`/remote build downloads; vcpkg baseline remains pinned |

Prior fixes remain in force: AAD binds mode/kind/deadline/trace/principal (F3); SWIM sender binding + no third-party
gossip `Left` (F4); checked tagless decode (F5); pending cap + Authenticate size cap (F6); `f.to == self_` (F7).

### Residuals / deliberate behavior changes

- **F1/F9 plaintext**: the plaintext transport still has no authenticated sender; the bank's plaintext Control-plane
  `Left` eviction still removes the ledger (`status1=3`). Only mTLS/`SecureTransport` closes that; `SecurityMode::Strict`
  is still **not wired** into startup. Do not run a ledger on `TcpTransport`.
- **Breaking defaults**: with no `Authorizer`, no `PrincipalPolicy` for non-anonymous frames, and a non-dual activation,
  inbound messages are now **dropped**. Apps must install `AllowAllAuthorizer`/a real ACL, a principal binder, and use
  `LocalRouter::reclaim_sink()` for actors that receive wire principals.
- **F11** is opt-in: internal sends remain trusted unless `LocalRouter::set_authorizer` is installed.
- **F12** bounds ghosts at 4096 and keeps them out of placement, but the convergence view still carries them; an
  application that consumes `Membership::view()` directly must use `placement_view()` for placement.
- **F13** per-IP/connection caps are not plumbed (the transport does not surface the peer address); the admission
  predicate can reserve known ids, but a 5-second 256-slot fill window remains.
- **F15** is a rate limit, not a hard mailbox bound; tune the budget for a legitimate high-throughput peer.
- **F16** 4MiB is a single-frame limit; larger logical payloads need 024 chunking/streaming.
- **F17** tagged still uses reused tag numbers, so a real schema change with reused tags needs `PeerSchema`/ABI
  negotiation; the default only removes the tagless positional-confusion path.
- **Supply chain** still has unpinned `apt-get`/`windows-latest`/MSVC; the LLVM hash and action SHAs must be bumped
  deliberately (a changed upstream script will fail the checksum step, as intended).

### Live verification commands

`run_batch1.sh` (RCE/fail-open), `run_forward.sh`, `run_place.sh`, `run_slot_exhaust.sh`, `run_flood.sh`,
`run_big.sh`, `run_bank.sh`, `relay_probe`, `fuzz_wire 2000000`, `schema_probe`, plus `run_tests.sh` (21 suites).
