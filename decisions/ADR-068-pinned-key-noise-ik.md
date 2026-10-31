# ADR-068 — Pinned-key handshake: Noise IK over libsodium (issue #40)

Status: **Accepted for the handshake engine and its `SecureTransport` composition; NOT accepted as delivering "no identity before
admission" on the shipped TCP transports.** Owner decision 2026-10-31 (Noise + libsodium, replacing a stopped TLS-pinned plan).
Spec: 020 "Pinned-key mode (Noise IK, #40)", 021 "HELLO and dial dedup in pinned-key mode". Consumer: AeroCoWorker RFC 002 section 6e
red-team findings B2 (no identity before admission) and B3 (identity is the full public key).

## What was and was not verified (read this first)

| Claim | Evidence | Status |
|---|---|---|
| The Noise state machine is correct for `Noise_IK_25519_ChaChaPoly_BLAKE2b` | `noise_ik_vectors_check`: the official cacophony vector (`tests/vectors/`, upstream SHA-256 in its header), both handshake messages, the handshake hash and 4 transport messages match byte for byte, both roles. Controls (flipped payload, different prologue, different pinned key, corrupted Msg1) each fail. Mutation: removing the pre-message hash breaks the vector check (3 checks) | **Verified (one vector, this suite).** Only one vector exists for exactly this suite; a second independent implementation was not run. |
| The ChaCha20-Poly1305 `Aead` adapter conforms to the seam | `sodium_aead_check`: round-trips, `*_into` forms agree, ciphertext equals a direct libsodium call with the documented nonce layout, 9 tamper/key/nonce/lane controls fail | **Verified** |
| Pinned pair completes; keys agree per direction and differ between directions; two sessions use different keys | `pinned_handshake_check` H1, H2 (with deterministic-seal control) | **Verified (in memory)** |
| Responder sends ZERO bytes to a prober / an un-admitted initiator / a replay, on real TCP | `pinned_privacy_check` P1-P3 over PAL sockets; the test server does not clear engine output, so `bytes_sent()==0` would show an emission. Controls: an XX responder reveals its static key (96 bytes); always-true provider completes; replay rule off answers the replay | **Verified on loopback, Windows only** |
| No NodeId, key or scope id appears in the handshake bytes; two sessions share no 5-byte run | `pinned_privacy_check` P4 with a planted-needle search control; control: today's `TcpTransport` HELLO shows the NodeId to a silent prober, identically every connection | **Verified for the handshake bytes only** |
| Trust change: un-admit refused on the next handshake, re-admit succeeds; the initiator sends nothing to an un-admitted key | H3, P2 | **Verified** |
| MITM with its own key refused; a peer cannot claim another key's NodeId or carry a key that did not authenticate | H4, H5 with honest-payload controls; mutations removing the trust check, the derived-NodeId check or the key-binding check each break checks | **Verified** |
| CA mode and pinned mode never complete with each other | `pinned_crossmode_check` with REAL mbedTLS engines both ways + same-mode controls; H6 prologue; X1 | **Verified** |
| Frames seal/open through the existing envelope on both lanes and both directions; replay of a sealed frame refused | `pinned_secure_transport_check` (SecureTransport over a logging fabric) | **Verified** |
| Forward secrecy of session keys; documented limit that Msg1 identifies the dialler to a later key thief | H9 on raw Noise with fixed ephemerals (fresh responder ephemeral gives different keys; the real one, the control, gives the same) and the engine-level limit check | **Verified as a construction property; no cryptographic proof** |
| Core links no crypto | `security_sodium_symbol_isolation` scans `quark.lib` for `sodium_`/`crypto_` (0 found) in a build with the adapter ON; control `..._control` scans libsodium's own library and must FAIL (it does) | **Verified (MSVC `dumpbin` only; Linux `nm` variant not written)** |
| Sanitizers | 5 of the 6 new checks under MSVC `/fsanitize=address` (Debug, `build-noise-asan`): clean. The cross-mode check needs mbedTLS and was not built there | **ASan only. UBSan/TSan not run (no MSVC equivalent).** |
| Linux / GCC / Clang build and run | none | **UNRUN. Never compiled on Linux.** |
| CI (`test-pinned-sodium`, pin audit, apt lock) | `ci/check_pins.sh` and its `--self-test` pass on an LF copy; the job is not executed; the apt lock `libsodium-dev=1.0.18-1ubuntu0.24.04.1` was resolved in a clean `ubuntu:24.04` container (2026-10-31; the first draft's `1.0.18-1build3` from memory lacked the security update) | **UNRUN.** `continue-on-error`, informational. |
| `TcpTransport` pinned mode: no HELLO, identification by handshake, sealed headers | not built | **NOT DONE.** Over today's transports a pinned node still sends its HELLO and `from`/`to` NodeIds in the clear. |
| Independent audit of the Noise glue and the key reuse | an independent red-team read the code (below); no audit | **Not audited** |
| Timing equalisation | not measured, not attempted | **Open (see Residuals)** |
| Full suite | see "Test results" | |

## 1. Decision

A second handshake mode beside the CA chain (ADR-040), selected per cluster by installing `PinnedHandshakeEngineFactory` instead of
the mbedTLS factory behind the **unchanged** `HandshakeEngine` seam; the AEAD seam gains `SodiumAeadChaCha20Poly1305`. A pinned-mode
build needs libsodium only.

- Suite `Noise_IK_25519_ChaChaPoly_BLAKE2b` (Noise rev 34). The Noise state machine is written here from libsodium primitives only
  (`crypto_scalarmult`, `crypto_aead_chacha20poly1305_ietf_*`, `crypto_generichash`; HMAC-BLAKE2b and HKDF per Noise 4.3 built on
  `crypto_generichash`). **This is the deviation from "do not implement crypto" that 020 makes explicit:** primitives are libsodium's,
  the state machine is ours (about 350 lines), proven against the official vector. Chosen over adopting a Noise library because none
  was in the owner's chosen dependency (libsodium) and a hand-rolled state machine over vetted primitives with a published vector is
  the smaller risk surface than a second dependency. It still wants an audit.
- Identity = the full Ed25519 public key; Noise static = its X25519 conversion; NodeId = first 8 bytes (LE) of SHA-256 of the key.
  Msg1 carries the Ed25519 key (the map is one-way) and the responder checks the conversion equals the authenticating static key.
- Application trust: `PinnedTrust::admitted(cluster, key)` + `PinnedKeyDirectory::key_of(cluster, node)`, consulted by both ends at
  every handshake. `PinnedKeyBook` is the ready-made implementation.
- Wire: Msg1 154 bytes (`-> e, es, s, ss`, payload: version, cluster, NodeId, timestamp, Ed25519 key), Msg2 58 bytes
  (`<- e, ee, se`, payload: version, NodeId). Prologue `"quark-pinned-key/v1" || mode`.
- Silent refusal; replay rule = skew window + strictly increasing per-initiator timestamp in a bounded record.
- Session keys = `HMAC-BLAKE2b(split_key, "quark-pinned/v1 c2s|s2c" || handshake_hash)`, label domain disjoint from TLS mode's.

### Key reuse (Ed25519 identity doubles as the X25519 static key) — accepted, flagged for audit
Libsodium supports the conversion and the combined use is believed safe when domain-separated (the handshake never signs, the
application never DHes, the prologue is Quark-specific). Accepted because the consumer pins ONE device key at pairing. Known sharp
edge, found by the red-team and handled: A and -A convert to the same X25519 key, so the key book refuses to hold both
(`PinnedKeyBook::admit`).

## 2. Red-team round (independent agent, read-only, then fixes)

An independent red-team (Opus, given the code and the requirements, not this ADR's conclusions) found **no flaw in the Noise state
machine** (init, prologue and pre-message, MixKey truncation, HKDF, nonce layout, Split, message order all match rev 34) and these
engineering findings. Each is dispositioned:

| # | Finding | Disposition |
|---|---|---|
| 1 | Property (1) is false on the production path: `SecureTransport` puts `from`/`to` in clear and takes the responder's `expected_peer` from the claimed `from`; `TcpTransport` still sends HELLO | **Accepted as the central gap.** Spec 020/021 now say so in the property's own bullet; this ADR's status line says so; the checked limit lives in `pinned_secure_transport_check`. NOT fixed: it is a transport redesign (see Follow-ups). |
| 2 | Timing oracle: a Msg1 authenticating under the responder's static key fails later than random bytes, confirming a device to anyone with its public key | **Documented, unmeasured, not mitigated** (020). |
| 3 | The responder buffered partial input; one injected 1-byte Authenticate frame parked an engine for the handshake timeout | **Fixed**: the engine is message-oriented and refuses any length other than 154 at once; test H10 and T1. |
| 4 | Replay residual understated: a replayed Msg1 after a responder restart installs a `SecureTransport` session for the initiator on the replayer's connection (traffic black-holed until the real initiator redials) | **Documented accurately** in 020; the limit is a checked test (H10), not hidden. Not fixed (needs a persisted record or a handshake nonce round trip). |
| 5 | Honest initiator can be locked out after a restart or clock step back until its clock catches up | **Documented** (020), checked as a limit (H10). |
| 6 | Signed overflow in the skew check with a huge skew | **Fixed** (unsigned delta), test H10 (`INT64_MAX`). |
| 7 | A / -A share an X25519 image | **Fixed in the book + documented**, test H10. |
| 8 | Incomplete wiping (hash state, `hk`, derived key buffer, `es` failure path) | **Fixed** where named. Not exhaustive (std::vector payloads, copies of `KeyPair` in `IkHandshake` are wiped only in its destructor). |
| 9 | "Random-looking" is false (ephemeral top bit is 0) | **Corrected** in 020 and the header. |
| - | Prologue separates only Noise from Noise; TLS bytes are refused independently | **Reworded**; the cross-mode test exercises both. |
| - | Vacuous tests: H9 (fresh ephemeral differs regardless), P-tests' `reply.clear()`, HKDF "distinct", H6 | **Fixed**: H9 rewritten with a control that uses the real ephemeral, `reply.clear()` removed, HKDF check removed, H6 reworded. |
| - | Pre-existing: an unauthenticated Authenticate frame claiming an ESTABLISHED peer makes `SecureTransport` treat the failed engine as a failed renegotiation and drop that peer's session (found while writing T1; identical in TLS mode) | **Not changed here** (TLS-mode behaviour is out of scope). Worth its own issue. |

## 3. Residuals and what remains unproven

- Not delivered on shipped transports: no-HELLO, identification by the handshake, sealed frame headers, per-IP pre-auth caps keyed on
  the socket. Until then a pinned node discloses its derived NodeId to a passive observer and, through the HELLO, to a silent prober.
- Timing; restart replay; initiator lock-out (above).
- Msg1 has no forward secrecy for the dialler's identity (inherent to IK); session traffic does.
- The trust provider is consulted at handshake time only: live sessions are untouched when trust changes (#41, out of scope).
- Only one test vector for this suite; the Noise glue has had no independent audit.
- 64-bit NodeId: collisions between admitted keys are refused by the book, but an application with its own provider must do the same.
- Never built or run on Linux, GCC, Clang, ARM64 or Android; ASan only on MSVC x64; no fuzzing of the Msg1 parser (it is a fixed-length
  decrypt-then-fixed-layout parse, but that is an argument, not evidence).

## 4. Follow-ups (not done, not started)

1. A pinned accept/dial path in `TcpTransport`: no HELLO, identify by handshake, dedup after it, sealed frame headers, pre-auth caps on
   the socket. This is what makes requirement B2 true on the wire. Needs its own spec text and ADR.
2. #41 live-session re-check; persisted replay record or a responder-supplied nonce; timing equalisation; a Linux `nm` isolation test.
3. A Linux/CI run; resolve the real noble `libsodium-dev` version.

## 5. Test results

Windows 11, MSVC (VS 18), x64 Release, `build-noise` with `QUARK_WITH_SODIUM=ON` and (for the cross-mode check) `QUARK_WITH_MBEDTLS=ON`:
see the final report for the counts of this run. The 245 pre-existing tests (including `mbedtls_aead_check` and `mbedtls_handshake_check`,
TLS/CA mode unchanged) pass unchanged beside the 8 new ones (6 adapter checks + 2 isolation tests).
