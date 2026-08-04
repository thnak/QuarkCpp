# ADR-040 — mTLS Node-to-Node Transport with Live Certificate Rotation

## Status

Accepted (with required pre-merge hardening item, see Decision).

## Question

`SecureTransport` (`include/quark/core/secure_transport.hpp`) wraps AEAD sealing + replay
protection over a `MockCipher`. 020-Security §2 explicitly defers the production handshake
(mutual auth, per-session key derivation) and a real cipher to a "thin adapter over mbedTLS or
BoringSSL." This ADR settles:

1. Which crypto backend, for a Windows/MSVC-only toolchain (no GCC/Clang, no TSan/UBSan).
2. How node-to-node mTLS mutual authentication is wired into cluster admission (021 AUTHENTICATE).
3. How a node's certificate/key rotates on a live cluster — expiry or compromise — without a
   coordinated cluster-wide restart, without dropping existing membership, and without ever
   silently accepting a session under an already-revoked key.

Two designs went through the `design-debate-prove` workflow (architect → red-team → prove →
judge) on this machine (MSVC 19.51, `/std:c++latest`, `/fsanitize=address`; no TSan/UBSan
available). Full transcripts (claims, attacks, rebuttals, executed evidence) are in the workflow
record this ADR summarizes; only the evidence load-bearing to the verdict is reproduced below.

## Designs (one-line summaries)

- **Design 1 — "Short-Lived Certs + In-Band Rekey"**: mbedTLS adapter; short-lived (10–60 min
  TTL) node certs sidestep revocation via expiry; per-connection `SessionEpoch` (current +
  retiring-previous) behind `atomic<shared_ptr<const SessionEpoch>>`; rotation is driven by a
  *second* mbedTLS handshake tunneled as the payload of AEAD-sealed `FrameKind::Control` frames
  over the **same already-open TCP connection**; epoch rides in the AEAD nonce/AAD.
- **Design 2 — "mTLS-A2: Hot-Reload Identity + Dual-Cert/Root Overlap"**: mbedTLS adapter; node
  identity and cluster trust root each live in a generic `OverlappedMaterial<T>` (mutex-guarded
  current + grace-windowed previous), hot-reloaded by a cold watcher; mTLS runs **once**, purely
  as an authenticated key-exchange at 021 AUTHENTICATE, then closes — the existing AEAD/replay
  envelope carries traffic; rotation is a periodic cold sweep (piggybacked on the SWIM keepalive
  tick) that renegotiates via a **fresh, standard mTLS handshake** or closes-and-lets-021-redial;
  revocation is enforced continuously against **already-open** sessions via a retained peer
  certificate fingerprint, not just at handshake time.

Both designs independently picked **mbedTLS over BoringSSL** for the same reason: this box is
Windows/MSVC-only with no GCC/Clang, and mbedTLS has official, tested CMake+MSVC/vcpkg support,
while BoringSSL's supported build path is Bazel/GN + Go/Ninja with an explicit no-stable-ABI,
no-external-consumption policy from Google. This choice was not contested by either red team and
is adopted without qualification.

## Evidence table

Only claims that both survived red-teaming (post-rebuttal `survivingClaims`) and were run through
the prove stage are counted. Conceded claims (falsified pre-proof, replaced by a fixed
claim) are marked accordingly and do not count for their design.

### Design 1 — Short-Lived Certs + In-Band Rekey

| Claim | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|
| C1 mutual-auth gate before any Data delivery | yes (revised) | **CORRECT** | mismatched-CA handshake: 0 deliveries, 2 AuthnFailure audits; pre-session `open_received` returns `NoSession`, no crash |
| C2 rekey drops zero in-flight frames | **conceded** (fatal seq-vs-wire-order race found) | — | replaced by C14 |
| C3 rotated-out epoch never accepted | yes | **CORRECT** | replay of valid epoch-1 frame after 2 rotations + grace elapsed → `RetiredEpoch`, never delivered |
| C4 rekey glare converges to one handshake, lower-NodeId = client | yes | **CORRECT** | 1000/1000 real loopback TLS1.3 trials, exactly one Finished handshake each |
| C6 atomic\<shared_ptr\> lookup ≤ mutex+map baseline | yes (rescoped to receive path, multi-session) | **CORRECT** | single-thread: 48.1ns vs 50.7ns; 4-thread/8-session: p99 300ns vs 2700–3100ns (atomic ~9–10x better) |
| C7 own-cert rotation propagates within poll+RTT | **conceded** (traffic-gated, not timer-driven) | — | replaced by C15 |
| C8 default build links zero mbedTLS symbols | yes | **CORRECT** | `dumpbin /symbols` on default-build core object: 0 matches for `mbedtls_`; `find_package(MbedTLS)` never invoked |
| C9 gossip-borne revocation force-closes live sessions | yes (new, closes conceded gap) | **CORRECT** | 3-node harness: session force-closed in one sweep after gossip watermark lands; post-revocation frame rejected |
| C10 pinned mbedTLS config actually gets TLS1.3 + exporter | yes (new) | **CORRECT** | both peers negotiate `TLSv1.3`, no silent 1.2 fallback; export callback returns 32B on both sides |
| C12 hard deadline independent of rekey success | yes (new) | **CORRECT** | epoch enforces hard_deadline even when rekey keeps failing; `HardExpired` before AEAD is attempted |
| C13 wide (C14) lock adds no meaningful throughput regression | yes | **WRONG** | measured ratio 0.81–0.97 (mean ~0.90) across 8 runs, **below the claimed ≥0.95 bound in 5/8 runs** — a real 10–19% regression with zero cross-peer contention |
| C14 (replaces C2) session_mu_ spans seq+seal+handoff → zero drops under concurrent senders + concurrent rekey | yes | **CORRECT** | 102000/102000 delivered, 0 drops, strict per-sender FIFO, 32–34 real rekeys interleaved; ASan-clean |
| C15 (replaces C7) timer-driven poller rekeys every session unconditionally | yes | **CORRECT** | 3 idle sessions rekeyed within ~612ms with zero application traffic, zero restarts |
| C5 concurrent send + rekey: 0 ASan reports | yes | **CORRECT (ASan-only)** | dedicated reader/writer race + smoke test, 0 reports; TSan/UBSan unavailable on MSVC |
| C11 asymmetric glare (only higher-NodeId's cert rotates) | yes (new) | **CORRECT** | 200/200 trials, lower-NodeId always ends up as client |

**Design 1 totals: 12 CORRECT, 1 WRONG (C13, a `fast`-kind claim, not safe/correct-kind), 2
conceded-and-replaced.**

### Design 2 — mTLS-A2 (Hot-Reload Identity + Overlap)

| Claim | Survived red-team? | Proven? | Number / evidence |
|---|---|---|---|
| F1 real cipher ≤200ns p50 over MockCipher | yes | **CORRECT** | real cipher is *faster* at every size: 64B −300ns, 256B −1200ns, 1024B −4900ns delta |
| F2 (**conceded**: literal sketch calls seal/open *inside* the session-map lock) | no | — | replaced by F2-revised |
| F2-revised: crypto never runs under a lock; send()=1 lock/call, deliver()=2 locks/call | yes | **CORRECT** | instrumented: 0 crypto-under-lock violations over 10^5 calls; 4-thread/8–32-peer: FIXED 4.59–4.95M ops/s, p99 1500–1800ns vs REGRESSED (crypto-under-lock) 1.09–1.12M ops/s, p99 11000–62400ns |
| S1 `OverlappedMaterial<T>` race-free, monotonic generations | yes | **CORRECT** | 1 writer + 4 readers, 4M reads, 870,533 rotations, 0 ASan reports |
| S2 no session ⇒ no delivery | yes | **CORRECT** | unrecognized-NodeId frame dropped, audited, never delivered |
| S3 revoked-but-unexpired cert rejected at handshake | yes | **CORRECT** | chain-valid, unexpired, revoked-fingerprint cert: handshake fails, `AuthnFailure` |
| S4 old CA root accepted only inside overlap window | yes | **CORRECT** | inside window: accept; past window: reject (required adding a union CA chain — implementation refinement, not a design change) |
| S5 (new — closes the **fatal** invariant gap: revocation of an *already-open* session) | yes | **CORRECT** | live session closed within one sweep tick after fingerprint lands in registry; further sends never delivered |
| S6 (new — closes leak-on-handshake-rejection DoS) | yes | **CORRECT** | 300 rejected dials, RSS flat (1060KB → 1024KB), no LSan on MSVC so RSS-plateau used as documented substitute |
| S7 (new — safe `session_snapshot()` DTO, no dangling refs) | yes | **CORRECT** | 0 ASan reports racing snapshot vs. concurrent teardown; sub-16µs snapshot at 256 peers |
| C1 own-identity rotation doesn't drop/delay traffic before grace elapses | yes | **CORRECT** | 25 frames delivered, 0 rotation-attributable closes before window |
| C2 renegotiate-fails ⇒ close+redial carries new generation, first frame opens | yes | **CORRECT** | reconnect within budget, generation matches, first post-reconnect frame opens (validates C4 end-to-end) |
| C3 revocation gossip convergence ≤ 2× ordinary membership convergence | yes | **CORRECT** | ratio 0.998–1.018 (two runs), well under 2.0× bound |
| C4 (new — closes a **fatal** key-derivation bug: undifferentiated client/server split guaranteed 0% interop) | yes | **CORRECT** | 200/200 fresh real handshakes, both directions open successfully after fixing the role-aware split (bug caught mid-proof, fixed, re-verified) |
| C5 (new — closes disclosed-but-unenforced AES-GCM frames-per-key volume risk) | yes | **CORRECT** | threshold enforced inline; no frame sealed past `kMaxFramesPerKey`; seamless rekey-and-continue |

**Design 2 totals: 14 CORRECT, 0 WRONG, 1 conceded-and-replaced (F2 → F2-revised).**

## Decision

**Winner: Design 2 — "mTLS-A2: Hot-Reload Identity + Dual-Cert/Root Overlap over
mbedTLS-as-Key-Exchange."**

Ranking rationale, in the stated order:

1. **Safety gate.** Neither design has a *safe*- or *correct*-kind claim marked WRONG.
   Design 1's single WRONG verdict (C13) is `fast`-kind (a throughput-regression claim about its
   own C14 correctness fix), not a safety/correctness claim, so it does not trigger the gate for
   either design. Both designs are gate-clean.

2. **Proven beats claimed.** Design 2 has **14/14** surviving claims proven CORRECT with executed
   evidence and **zero** disproven claims. Design 1 has **12/13** proven CORRECT and **one**
   disproven (C13). Design 1 also required two conceded-and-replaced claims (C2→C14, C7→C15)
   during rebuttal to reach that state; Design 2 required one (F2→F2-revised). Both designs also
   survived and closed genuinely fatal red-team findings before proof — Design 1's fatal
   seq-vs-wire-order replay race (C2) and Design 2's fatal client/server key-split bug (C4) and
   fatal live-session-survives-revocation gap (S5) — so this is not "Design 2 had no bugs," it is
   "Design 2's fixed claim set came out clean under proof and Design 1's did not."

3. **Measured hot-path numbers, among safe survivors.** Design 2's F2-revised is the more
   informative comparison here: it demonstrates that **crypto never runs under a lock** at all —
   send() takes one short lock only to draw `seq` and copy a `shared_ptr` to the cipher, then
   seals unlocked — giving flat throughput (4.59–4.95M sealed-frames/sec) and tight p99
   (1500–1800ns) independent of peer count (8→32), against a *measured* regressed variant
   (crypto held under the lock) at 1.09–1.12M ops/s and p99 11–62µs. Design 1 needed to widen its
   per-session lock to *include* AEAD-seal-and-wire-handoff (C14) specifically to close its own
   fatal race, and its own C13 benchmark shows that widening costs 10–19% single-peer throughput
   even with **zero** cross-peer contention (3.15–3.76M vs 3.87–3.92M sealed-frames/sec) — i.e.
   Design 1's fix moved it *toward* the locked-crypto shape that Design 2's numbers show is the
   worse regime. Design 2 also wins narrowly on cipher microbenchmark shape (F1: real AES-128-GCM
   beats `MockCipher` at every frame size instead of merely staying under a 200ns budget).

4. **Core-invariant fidelity.** Design 1's rotation mechanism — tunneling a second, independent
   mbedTLS handshake as the payload of AEAD-sealed `Control` frames over the *same* connection —
   is, by the red team's and the design's own risk list's admission, architecturally unusual with
   "no deployed precedent," the highest-novelty and highest-risk part of that design even after
   proof. Design 2's rotation mechanism (a fresh, standard mTLS handshake, or close-and-let-021-
   redial) is the more conventional, lower-novelty choice and does not bend any stated invariant
   (one-connection-per-peer is preserved either way; 021's existing reconnect/backoff machinery is
   reused rather than a new tunneled sub-protocol). Short-lived (10–60 min) certs as Design 1's
   primary revocation story is also a heavier, continuous operational dependency (an always-live
   CA reachable well inside every TTL window) than Design 2's overlap-plus-fingerprint-sweep
   model, which degrades more gracefully if the CA is briefly unreachable (existing sessions keep
   working; only new rotation stalls, caught by C12-equivalent hardening below).

**This is not a clean pass — one required hardening item before this design is safe to merge as
Accepted**, found independently by this judge, not by either red team:

**Required fix — same-session concurrent-sender ordering.** Design 2's own F2-revised claim
states send()'s critical section is *only* "seq increment + send_cipher shared_ptr copy, then
seal() unlocked" — i.e., `seq` is drawn under the lock, but AEAD-seal and the wire hand-off both
happen after the lock is released. This is **structurally identical** to the shape that Design
1's red team proved fatal for that design's original C2 (`seal_and_send`: `seq` drawn via
`fetch_add`, then GCM-seal and `inner.send()` both happen outside any ordering guarantee) —
concurrent senders on the *same* `PeerSession` can have their `seq`-assignment order diverge from
wire-arrival order, so a later-`seq`, earlier-wire-arriving frame advances the strict monotonic
replay high-water mark and a genuinely fresh, non-replayed, earlier-`seq` frame that arrives after
it is rejected as a spurious "replay." **Design 2's own F2-revised benchmark never tested this**
— its 4-thread contention test sends to *disjoint* peers (no two threads share a `PeerSession`),
which cannot exercise the race. This gap is untested, not disproven (INCONCLUSIVE, no formal
weight against the count above), but it is a well-understood, cheap fix: Design 1 already
implemented and proved it (C14 — widen the per-session critical section to span
seq-draw+seal+wire-handoff) with a known, measured cost (10–19% single-peer throughput). Per this
ADR's own gate rule ("disqualified unless a stated cheap fix exists"), this qualifies as a stated
cheap fix, imported verbatim from the sibling design, so Design 2 is **not disqualified** but
**must** apply it — and re-run Design 1's C14 test (adversarial `yield()`-hook variant, 4 threads,
10^5 sends, concurrent rekey) against Design 2's `send()`/`deliver()` before this ships — as a
condition of moving this ADR from the record to production code.

A second, lower-severity gap: Design 2 has no analog to Design 1's C8 (`dumpbin`-verified proof
that `QUARK_WITH_MBEDTLS=OFF` links zero mbedTLS symbols into the core). The design's summary
describes the same opt-in-adapter posture, but it was never independently proven for Design 2's
concrete CMake wiring. Import C8's test verbatim as a follow-up before merge.

## Residual risks

- **Same-session send-path ordering (see Required fix above)** — the single most important open
  item; must be closed with Design 1's proven C14 pattern before production use.
- **Opt-in adapter build isolation unproven for Design 2** — import Design 1's C8 `dumpbin`
  symbol-scan test against Design 2's concrete CMake target.
- **No TSan/UBSan on this MSVC toolchain.** All concurrency claims for both designs are
  ASan(+debug-CRT)-only. This is a real coverage gap versus the project's normal Linux+GCC/Clang+
  TSan bar (per `VERIFICATION.md`'s standard); re-verify the adopted design's concurrency claims
  (S1, S5, S7, C1, C2, C5-equivalent) on Linux under TSan before treating this ADR's safety
  verdicts as final.
- **Revocation propagation latency under partition.** Design 2's revocation still rides gossip
  convergence (proven ~1.0× ordinary membership convergence, C3) — during a network partition, a
  node holding a stolen key can still authenticate to peers on the far side until the partition
  heals and gossip catches up. This is the exact "decentralized revocation propagation latency"
  open question already flagged in 020-Security §"Open questions"; this ADR gives it a concrete,
  measured mechanism but does not eliminate the latency window.
- **AES-128-GCM key size choice.** Design 2's adopted `MbedtlsAeadGcm` uses AES-128-GCM (Design
  1's used AES-256-GCM for its epoch keys). Both are NIST-approved; this ADR does not mandate a
  key-size policy — record it explicitly in 020-Security rather than leaving it implicit in code.
- **`try_renegotiate()`'s internals were never separately proven** — C2 exercises the
  renegotiate-fails-then-close-and-redial fallback path but the *successful* in-place renegotiate
  path (as opposed to close+redial) has no dedicated executed claim; low severity since the
  fallback path is proven to work correctly and 021's redial machinery is pre-existing and
  separately relied upon.
- **Two real gaps found exercising the real async transport for the first time (found post-merge,
  2026-08-04, root-caused, reproduced, and fixed — see below).** A manual 4-container
  docker experiment (`net::TcpTransport` wrapped by `SecureTransport` with `enable_handshake()`, real
  mbedTLS 3.6.7) initially appeared to show the CLIENT-role handshake stalling non-deterministically.
  A follow-up investigation disproved that specific symptom — it was a measurement artifact in the
  experiment's own harness (it waited for sessions to establish *before* ever calling `send()`, the
  only trigger that initiates a client-role handshake, so it could never succeed by construction; the
  0/1/2/3-session "staircase" across nodes was container-start skew, not a real fault). The suspected
  `FrameKind::Authenticate` partial-record desync is ruled out: `net::wire_codec.hpp`'s `FrameStream`
  correctly reassembles `[u32 len][body]` before `SecureTransport::deliver()` ever sees a frame, and
  `MbedtlsHandshakeEngine::advance()` accumulates across calls regardless. This path had, however,
  ONLY ever been exercised via `tests/adapters/mbedtls_handshake_check.cpp`'s synchronous
  hand-fed-bytes-between-two-engines harness (zero latency, one complete flight per `advance()` call)
  before this experiment drove it through the real epoll-driven async transport — and doing so
  surfaced two genuine, reproduced bugs neither the mock nor the synchronous harness could catch:
  - **A lost opening handshake frame wedges that peer permanently.**
    `SecureTransport::ensure_handshake()`/`handle_authenticate_frame()`
    (`include/quark/core/secure_transport.hpp`) park a client-role engine in `pending_` with no
    timeout, eviction, or retry; `TcpTransport::send_on_loop` silently drops a frame (no reconnect
    armed) whenever the peer's endpoint isn't registered yet (021 discovery/SWIM gossip feeds
    `add_peer` asynchronously, so this is a normal, expected race, not a rare fault). Reproduced with
    two loopback processes and a 2s-delayed `add_peer`: the client engine parks after `WantWrite`,
    the frame never arrives, and `sessions=0 pending=1` holds forever — no `Failed` step, no audit
    record, indistinguishable from a genuine stall.
  - **An mTLS session outlives the TCP connection it was negotiated on.** `SecureTransport` never
    drops a session when the underlying connection dies. After a peer restarts: the survivor keeps
    sealing frames under dead keys (never delivered), the restarted peer has no session so drops
    everything (S2), and — since the restarted peer is server-role and cannot self-initiate, while
    the survivor already "has" a session so never re-handshakes — the pair is a permanent
    bidirectional blackhole until `sweep_rotation`'s 1h idle timer (itself only armed if
    `SwimMembership::set_sweep_hook` is wired). Reproduced by killing/restarting one of two loopback
    peers mid-stream: `delivered` freezes permanently on both sides at the pre-restart count.

  Applied: (1) a progress deadline on `pending_` handshakes (default 5s,
  `SecureTransport::set_handshake_timeout()`/`handshakes_timed_out()`) so a lost opening frame gets
  abandoned and retried instead of parking forever; (2) `TcpTransport::set_peer_down_hook()` (mirrors
  the existing `set_reset_hook`/`reset_peer_connection` shape) fires on connection death for an
  established, identified peer; `SecureTransport::on_peer_disconnected()`
  (`sessions_dropped_on_disconnect()`) drops that peer's session/pending-handshake so the next send
  re-triggers a clean handshake instead of sealing into a void. Each fix has a dedicated, deterministic
  regression test (QueuedFabric + a settable virtual clock, no real sockets/timing needed):
  `tests/security_secure_transport_handshake_timeout_test.cpp` (a permanently-undelivered opening
  frame is abandoned and retried only once the virtual clock crosses the deadline, never before) and
  `tests/security_secure_transport_peer_disconnect_test.cpp` (`on_peer_disconnected()` drops the
  session, the next send() re-handshakes, and delivery resumes — proving the pair is not permanently
  blackholed). Verified: full `security_*`/`secure_transport_*`/`tcp_transport_*` test filter passes
  (MSVC Debug) after the change; the project's normal cross-platform GCC/Clang + sanitizer prove/verify
  pass (see the "Cross-platform re-verification" item below) still applies before this is considered
  fully closed.
- **Cross-platform re-verification.** Both designs were built and proven only under
  Windows/MSVC per this task's explicit constraint; neither has been compiled or sanitizer-run
  under the project's primary GCC/Clang targets. Standard practice per CONVENTIONS.md (dual
  g++/clang++ compile, TSan/UBSan) must still be satisfied before this ADR's design is considered
  fully verified against the project's normal bar.
- **mbedTLS supply chain.** Both designs inherit mbedTLS's CVE surface, release cadence, and (if
  vcpkg-sourced) an additional registry/build supply-chain surface — the same ongoing
  pin-and-track-advisories obligation already accepted for the SQLite/RocksDB adapters.

## Spec recommendations

**020-Security.md**
- §2 "Transport security seam": replace the "deferred: production handshake … real cipher" text
  with the adopted design — mbedTLS as an opt-in adapter (`QUARK_WITH_MBEDTLS`, mirroring
  `QUARK_WITH_SQLITE`/`QUARK_WITH_ROCKSDB`); mTLS used *once* per connection, purely as an
  authenticated key-exchange (TLS 1.3 handshake + RFC 8446 §7.5 exporter or mbedTLS's directional
  traffic-secret export callback), never as the record layer for application data; the existing
  AEAD/sequence/replay envelope in `secure_transport.hpp` carries all application traffic,
  extended with a per-peer `PeerSession` (cipher pointers + `generation` + retained peer
  fingerprint) folded into the existing session map/lock.
- §"104: The honest exception: crypto is not self-implemented": name mbedTLS as the selected
  backend for the Windows/MSVC-primary posture, with the BoringSSL-rejection rationale (no stable
  ABI, Bazel/GN/Go build path, no official MSVC support) recorded so a future contributor doesn't
  reopen the question without new facts.
- §"Open questions" (revocation): resolve "gossip a revocation list, or short-lived certs?" in
  favor of **both, layered**: normal-case identity rotation via `OverlappedMaterial<T>`
  (current+grace-windowed-previous, operator-controlled TTL, no forced short TTL), plus
  compromise-case revocation via a retained-peer-fingerprint `RevocationRegistry` gossiped over
  the *same* channel 021/026 already use for membership convergence (proven to converge at ~1.0×
  ordinary membership-change latency, not a separate slower path) and enforced against
  **already-open** sessions on a bounded sweep tick, not just at handshake time.
- Add an explicit invariant statement (new subsection under §2): "a revoked peer fingerprint must
  close any live session to that peer within one sweep-tick interval, independent of whether that
  session predates the revocation" — this was the fatal gap this debate found and closed (S5); it
  should be a first-class spec invariant, not an implementation detail.
- Add an explicit hot-path invariant: "AEAD seal/open for an established session never executes
  while holding a lock shared with any other peer's session" (per the proven F2-revised
  regression), and require that any per-session serialization added for ordering correctness
  (see 010 recommendation below) be scoped to that one session's mutex, never a cluster-wide one.

**021-Cluster-Formation-and-Lifecycle.md**
- §"1. Source of trust": specify that AUTHENTICATE performs a real mTLS mutual handshake against
  the cluster-CA trust anchor (`TrustStore`, itself an `OverlappedMaterial<TrustedRoots>` to allow
  CA-root rotation with an explicit overlap window — proven S4), and binds `NodeId` + cluster-id
  from the verified leaf certificate (mismatch ⇒ reject, proven C1/S3-equivalent evidence in the
  design record).
- Add a new subsection, "Certificate rotation and revocation on a live cluster": document the
  `OverlappedMaterial<TlsIdentity>` hot-reload model (rotate() keeps the outgoing cert usable for
  an overlap window; a cold sweep piggybacked on the SWIM keepalive tick renegotiates or
  close-and-lets-021-redial by a deadline — proven C1/C2), and the immediate-evict revocation path
  (no grace window — proven S5) as the two distinct triggers a node's session sweep must evaluate.
  State explicitly that neither path requires a coordinated cluster-wide restart or a
  cluster-wide barrier (proven C9-equivalent/C1: gossip-only propagation, zero restarts observed).
- §"Dial deduplication — the concurrency hazard": note that certificate rotation reuses the
  existing reconnect/backoff machinery on renegotiate failure (proven C2) rather than inventing a
  parallel rekey protocol — no new wire-level glare-breaking rule is needed beyond what 021
  already specifies for connection establishment.

**010-Distribution.md**
- §"Transport seam" / §"Sibling seams": record that `SecureTransport`'s per-peer session state
  (cipher pointers, sequence counter, replay high-water mark, `generation`, retained peer
  fingerprint) is folded into the transport's existing per-peer lock/map — **add the explicit
  invariant this ADR's required fix establishes**: *"sequence-number assignment, AEAD seal, and
  the hand-off to the wire transport for one peer's session must be ordered atomically with
  respect to other same-session senders — assigning `seq` without serializing it through to wire
  submission permits wire-arrival order to diverge from `seq` order under concurrent same-session
  senders, causing genuine non-replayed frames to be rejected by the strict-monotonic replay
  guard."* This must be listed as a load-bearing invariant with its own test (per CLAUDE.md's "a
  load-bearing invariant without a test … is not done"), reusing the adversarial
  `yield()`-hook/concurrent-rekey test already proven for this exact property in the design
  record.
- §"Membership": cross-reference 021's new rotation/revocation subsection rather than duplicating
  it.

## Artifacts

Executed-evidence artifacts for both designs (build logs, prove binaries, benchmark outputs):
`C:\Users\thanh\AppData\Local\Temp\quark-prove-mtls`.

This ADR record: `decisions/ADR-040-mtls-node-transport-cert-rotation.md`.
