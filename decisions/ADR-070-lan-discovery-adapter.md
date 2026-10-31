# ADR-070 — LAN discovery adapter (DNS-SD/mDNS) with application-supplied, rotating, unnamed ids (021, issue #42)

Status: **Proposed** (2026-10-31). Implements the Draft amendment "LAN discovery adapter (DNS-SD)" in
[021](../021-Cluster-Formation-and-Lifecycle.md). Origin: AeroCoWorker ADR-044 section 3.4 / rule L14 / gate GL9 and its
RFC 002 section 6e items 2, 3, 7, 8, 9 and gate G10 (e); issue #42 (Q3). Direction: [ADR-067](ADR-067-direction-aerocoworker-network-center.md).
Evidence below is from this machine (Windows 11, MSVC, Release); nothing here was run under ASan/UBSan/TSan or on Linux.

## 1. Decision

Add a **discovery adapter outside the core** (`include/quark/net/lan_discovery.hpp`) behind a **platform seam** (`DnsSdPlatform`).

1. **Discovery grants nothing.** Output is `DiscoveredPeer` (address, port, advertised id). No `NodeId`, no trust. A feed
   (`lan_discovery_feed.hpp`) only teaches `TcpTransport::add_peer`; handshake and `NodeAuthority` still decide.
2. **The advertisement is exactly** `{service type, base32(id), host label, port, TXT}` where every value except the encoding and
   the port is application-supplied; TXT is empty unless supplied. Nothing is advertised if the id or host label is missing.
3. **Rotation is the application's:** a `WindowProvider` gives advertise id, accept ids (current + previous) and host label; the
   application signals `window_boundary`. Quark never sees the key and never owns the clock. Browsing matches by **prefix** of the decoded label.
4. **Per-scope runtime control:** several scopes advertise at once; stop/close withdraws adverts; destruction withdraws everything.
5. **Platform seam:** the application may supply `DnsSdPlatform` (AeroCoWorker HAL, A2). Quark ships an in-memory fake and a Windows
   implementation (`QUARK_WITH_DNSSD`, dnsapi), never in the core. **Linux/Avahi is out of scope.**
6. **Client isolation:** `visibility()` returns `Unknown`; no probing (see section 5).

## 2. What was built

Spec: 021 subsection (Draft). Code: `lan_discovery.hpp` (adapter, seam, encoding), `lan_discovery_fake.hpp` (in-memory LAN with
mDNS-style name conflicts, goodbyes, isolation knob), `lan_discovery_feed.hpp` (to `TcpTransport`),
`adapters/dnssd/windows_dnssd.hpp` (opt-in), `cmake/QuarkDiscoveryAdapters.cmake`. Tests in section 3.

## 3. Falsifiable claims and the test that would break each

| # | Claim | Test (file) | Control, and what it showed |
|---|---|---|---|
| D1 | Two nodes with no seeds find each other and connect over real loopback sockets | `lan_discovery_connect_test` | Discovery not started: frame never delivered (0 of 1) |
| D2 | The platform receives byte-exact `{type, base32(id), host label, port, no TXT}`; encoding matches RFC 4648 vectors | `lan_discovery_privacy_test` | A leaky wrapper (node id appended, product TXT) fails the identical assertion |
| D3 | Across a boundary, a rotated node finds a not-yet-rotated one via the previous id; a two-windows-old id is not accepted | `lan_discovery_rotation_test` | Current-only browser finds the rotated peer, loses the unrotated one (1/0) |
| D4 | After `stop_advertising` a fresh browser finds nothing; existing browsers get a Lost | `lan_discovery_stop_test` | A fresh browser before the stop does find it |
| D5 | A discovered, fed node the authority does not admit is never admitted or selected by `place_admitted` (2000 ids) | `lan_discovery_grants_nothing_test` | After `admit(3)` placement selects it (668/2000) |
| D6 | Real Windows DNS-SD registers, and a second browser in the same process sees the PTR+SRV (instance id, host label, port) | `dnssd_windows_smoke_check` (only with `QUARK_WITH_DNSSD`) | exit 77 with an explicit reason if mDNS is blocked; never a silent pass |

## 4. Findings (things the design had to face)

- **F1: identical ids collide.** ADR-044's id is group-wide (same for every member in a window), but mDNS instance names are unique
  per network; the second registrant is refused (modelled by the fake, tested in D1). The application must append per-member random
  bytes; the adapter matches by prefix. The random suffix is unlinkable noise but is **stable within a window**, so an observer can
  count members and track one within a window (not across windows). AeroCoWorker should accept or amend ADR-044 here.
- **F2: the host name.** Windows `DnsServiceRegister` requires a host name; the adapter passes the application-supplied label
  (+ `.local`) and fails to advertise without one. If an application passes a stable label, that label is a stable identifier on the
  wire and defeats rotation. A platform that forces the machine's own name leaks that name in the A/SRV records. Not preventable in Quark.
- **F3: no NodeId before the handshake.** `TcpTransport` dials by `NodeId`; a discovered endpoint has none. The feed takes an
  application `NodeResolver` (tests map port to node as a stand-in). A true dial-by-address needs the pinned-key handshake (#40) so the
  answering node's identity is learned from the handshake. **Owed.**
- **F4: Windows resolution.** On the bring-up host `DnsServiceResolve` returned pending and never completed for a service registered by
  the same machine, so the implementation reads port and host from the SRV delivered by the browse callback and looks the host's A record up with a
  multicast `DnsQuery_W`. For a same-machine registration that query timed out (the registrant did not answer its own multicast query),
  so D6 verified instance id, host label and port but **not the address**; sightings then carry `ipv4 == 0`. Address resolution across two real
  machines is **unverified**.
- **Other leaks:** the listening port is in the SRV; the advertised IPv4 is in the A record; an observer can see that *something*
  of the application's service type exists on the network (the type is application-supplied, not hidden).

## 5. Owed / not done

- "No peers visible" vs "no peers present": **not distinguishable passively**; reported as `Unknown`. No heuristic was invented. Owed (needs a passive platform signal or an app-level rendezvous).
- Linux (Avahi), Android (NsdManager) platforms; plain UDP multicast; IPv6 addresses; AAAA; TXT size limits.
- Foreground-only / named-network policy is the application's (it calls start/stop); the adapter has no network-identity notion.
- No sanitizer or arm64 run; the adapter uses a recursive mutex and calls user callbacks under it (documented), which TSan has not seen.

## 6. Consequences

Quark gains one adapter, one seam and one optional Windows backend; the core and SWIM are untouched. Privacy rests on the
application's window/host-label discipline; Quark only guarantees it adds nothing of its own (D2).
