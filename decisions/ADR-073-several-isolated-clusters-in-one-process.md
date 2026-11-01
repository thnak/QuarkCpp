# ADR-073: Several isolated clusters in one process (#44, AeroCoWorker ADR-044 Q5)

Status: Proposed. Spec: 020 §1a (admission), 021 §1 (cluster identity).

## Finding
Quark already supports it by construction; what was missing was the proof and one defence in depth. Every piece of security state is an
INSTANCE member: `AdmissionAuthority` (authority and revocation of that cluster), `SecureTransport` (sessions, pending handshakes, schemas,
revoked-fingerprint snapshot), the handshake factory and its identity/trust (so each cluster can hold different device keys), `TcpTransport`
(connections, relay and dial state, admission binding) and the SWIM membership. The statics in the tree are resource sharing only: the message
pool, thread-local re-entrancy flags (`admission.hpp`, `secure_transport_admission.hpp`), the mbedTLS RNG, a wire-warning hook and a test-only CSPRNG
override. Nothing a cluster decides (trust, admission, revocation, relay) is process-global.

## Recipe
One cluster instance = one `AdmissionAuthority` + one `TcpTransport` (its own listen port) + one `SecureTransport` with that cluster's `ClusterId`,
identity/trust (or pinned factory) and `set_admission(authority)` + one `SwimMembership` with that `ClusterId`. A process in N clusters builds N
instances; threads (one IO thread per `TcpTransport`) are the shared resource, memory is per instance.

## Change
`SecureTransport::build_aad` binds the cluster id. Sessions of two clusters already hold different keys (each handshake derives its own and the
pinned Noise handshake commits the cluster id in Msg1), but a device in both clusters must not rely on key freshness alone: a frame sealed in cluster A
now fails the AEAD tag in cluster B even under an identical key. Wire-visible: AAD only, both ends in a cluster compute it, no format change.

## Proof (`multi_cluster_isolation_test`, real TCP, same NodeIds 1 and 2 in clusters A and B, one process)
T1 admitted in A, denied by B's authority: no session, nothing delivered in B (CONTROL: B admits, it connects). T2 evicting in A does not touch B and the
reverse (CONTROL: the eviction took effect). T3 a sealed frame captured in A and injected into B's session is rejected (CONTROL: with the pre-fix AAD
and the mock handshake's identical keys it is ACCEPTED). T4 a cluster-A peer reaching B's endpoint gets no session on either side.
T5 cost per extra instance, measured (Windows, Debug): 1 IO thread, 1 listening socket, about 29 KiB resident across 8 idle instances.

## Not covered
- Relay: a relay path is configured on a `TcpTransport` instance, so one cluster's transport has no route into another's by construction; there is no
  dedicated cross-cluster relay test.
- The 50-device cap is the application's; per-cluster cost with 50 live sessions is not measured (idle instances only).
- Linux/TSan not run for this test.

## Issue #49 evidence (2026-11-01)

thnak-server (Ubuntu, x86-64), libsodium 1.0.18 built from `libsodium-1.0.18.tar.gz` into `~/local/sodium` (sha256 `6f504490b342a4f8a4c4a02fc9b866cbef8622d5df4e5452b46be121e46636c1`, the value libsodium publishes; the `.minisig` was downloaded but `minisign` is not installed there, so only the checksum was verified), `-DQUARK_WITH_SODIUM=ON`, RelWithDebInfo. 28 (ASan/UBSan) / 27 (TSan) CTest tests: the noise/sodium/pinned checks, `admission_*`, `lan_discovery_*`, `multi_cluster_isolation_test`, `tcp_reconnect_supersede_test`, `tcp_admission_redial_test`, `swim_lease_hardening_test`, `security_secure_transport_renegotiation_loop_test`, `pinned_flood_check`, the nm isolation test and its control. Results: GCC 15.2 ASan+UBSan 28/28, Clang 21.1 ASan+UBSan 28/28, GCC 15.2 TSan (-j1) 27/27, GCC 15.2 TSan with `-DQUARK_FORCE_ATOMIC_SHARED_PTR_FALLBACK` 27/27, Clang 21.1 TSan (-j1) 27/27; zero sanitizer reports. Windows MSVC Release: 271/271.

- `multi_cluster_isolation_test` runs clean under GCC/Clang ASan+UBSan and TSan (-j1).
