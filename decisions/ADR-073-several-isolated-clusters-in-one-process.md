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
