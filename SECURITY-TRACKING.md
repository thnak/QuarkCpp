# Security Remediation Tracking

**Source:** `SECURITY-FINDINGS.md` (live plaintext + real-mTLS bank cluster attack pass)  
**Updated:** 2026-10-25 (pass 2: 7-node busy lab)  
**Status key:** `[x]` fixed & verified · `[~]` mitigation in place, follow-up open · `[ ]` open

Verified baseline: **21/21 affected CTest suites pass** on `thnak-server` (real mbedTLS 3.6.5,
ASan/UBSan where applicable). Commits: `bbcf60e` (wire hardening), `f1286ee` (attack matrix),
`81c8d0d` (fail-closed remediation), plus the pass-2 commit adding startup refusal, control budget,
ghost reaping, and configurable framing. Pass 2 also ran a **7-node busy** mTLS/plaintext lab
(34,548 mTLS busy transfers, zero theft) plus the strict-refusal check (exit 3).

---

## Open / follow-up backlog

### P0 — do not operate a plaintext cluster until this is closed

- [x] **F1/F9: refuse plaintext at startup; wire `SecurityMode::Strict`.**
  - **Why:** the hardened build still accepts a plaintext `TcpTransport`; a forged SWIM `Left`
    removes the ledger (`status1=3` in the live run) because there is no authenticated sender.
  - **Next:** call `validate_security` from `validate_engine_config`/node bootstrap with
    `cluster_size > 1` + transport kind; reject `Strict + plaintext`; make mTLS the default for
    multi-node deployments.
  - **Acceptance:** a strict, multi-node, plaintext config fails before `start()` with a clear
    diagnostic; the plaintext bank PoC cannot boot in strict mode; the mTLS path is unaffected.
  - **Verify:** `security_config_validation_test` covers plaintext/secure transport; the production
    `bank_strict` exits **3** with `Strict + multi-node ... plaintext`; the lab uses
    `QUARK_ALLOW_INSECURE` explicitly. (Implemented pass 2.)

### P1 — authorization and internal trust

- [x] **F11: adopt `LocalRouter::set_authorizer` in authorization-sensitive apps (bank sample).**
  - **Why:** the hook exists and blocks the Gate→Bank rights=1→4 escalation, but it is opt-in;
    default internal sends remain trusted by design.
  - **Next:** add a local-ACL example to the bank-style sample/docs; decide whether privileged
    actors should also assert `ctx.principal` in the handler as defense in depth.
  - **Acceptance:** `forward_probe` remains blocked; a documented pattern shows the local ACL;
    no hot-path change when unset.
  - **Verify:** `run_forward.sh`; the 7-node busy ledger installs the local ACL and stamps the
    system thread `{1,4}` — anonymous internal sends are refused. (Implemented pass 2.)

- [x] **F2/F10/F8: publish a migration guide for the new fail-closed defaults.**
  - **Why:** non-anonymous wire principals now drop without `set_principal_policy`; inbound data
    drops without `set_authorizer`; non-anonymous descriptors drop for `pool.sink()` activations.
  - **Next:** README/sample notes: install `AllowAllAuthorizer` (or a real ACL), a principal
    binder, and `LocalRouter::reclaim_sink()` for wire-facing actors.
  - **Acceptance:** every sample still builds/runs; the guide names the three defaults and the
    opt-in APIs.
  - **Verify:** samples 08/15–19/22 build; the integration suite passes. See “Migration for
    applications” below. (Implemented pass 2.)

### P2 — pre-auth and control-plane DoS

- [x] **F13: per-IP/connection handshake cap + reserved slots.**
  - **Why:** the global reaper frees slots after 5s, but 256 distinct spoofed senders can still
    fill the cap for that window; `set_handshake_admission` is a NodeId predicate, not an address
    cap. The legitimate node-0 retry succeeds after the timeout (verified).
  - **Next:** plumb the remote endpoint from `TcpTransport` into the admission decision, or add a
    per-connection token bucket at the transport; reserve a small slot set for known/verified peers.
  - **Acceptance:** a 3000-sender flood cannot deny a known peer's handshake within the window;
    memory stays bounded; the legitimate retry remains successful.
  - **Verify:** **pass 2 implemented.** `pal::accept_one` reports the accepted peer address;
    `TcpTransport` attaches it transiently to the frame (`PeerAddress`, never serialized);
    `SecureTransport` caps pending handshakes per source IP (default 64), reserves 16 slots for an
    allowlisted known-peer predicate, and exposes `set_handshake_admission_addr`. Live:
    3000 same-IP pre-auth senders → **64 pending**; the allowlisted node 0 authenticates
    **during** the flood; an unknown peer is denied in the window and succeeds after the 5s
    reaper. Unit: `security_wire_hardening_test::per_ip_pending_cap` (same-IP cap, different-IP
    unaffected, reserved-slot exhaustion).

- [x] **F1 (control plane): rate-limit SWIM Control frames.**
  - **Why:** the F15 bucket covers `DistributedRouter` **data** frames; a peer can still flood
    Control/Gossip frames (CPU/republish cost).
  - **Next:** per-peer budget for `SwimMembership::on_frame`, or fold Control into the transport
    admission gate.
  - **Acceptance:** a Control flood is capped while normal gossip/probe rates pass.
  - **Verify:** `security_wire_hardening_test` floods 100 Gossip frames with a tiny budget and
    asserts `control_sheds() > 0`; `cluster_swim_*` stay green. (Implemented pass 2:
    `control_rate_per_sec`/`control_burst`, fixed 256-bucket table.)

- [x] **F12: cap/GC unverified members and bound probe work.**
  - **Why:** `max_live_members=4096` bounds the table, but ghosts still enter the convergence
    `view()` (512 in the live test) and each creation triggers `republish()`; a ghost flood can
    consume view/CPU memory even though placement is protected.
  - **Next:** lower/tune the cap per deployment; evict long-lived unverified members; bound
    `next_probe_target` work; consider not republishing unverified entries in `view()` behind a
    config flag if consumers can use `placement_view()`.
  - **Acceptance:** ghost injection is bounded in memory and CPU; `placement_view` remains clean;
    join/rejoin convergence still passes.
  - **Verify:** `run_place.sh` (owner=1/pview=1 with 512 ghosts) + `cluster_swim_partition_heal_test`.
    **Pass 2:** `unverified_ttl_ns` reaper (default 30s; test at 1ms) erases gossip-only ghosts
    from the convergence view; placement stayed clean.

### P3 — resource governance and framing

- [x] **F15: hard mailbox bound / backpressure; tune the per-peer budget.**
  - **Why:** the token bucket caps ingress (10k → 1,277 admitted, RSS 6.5→7.1 MB), but it is a
    rate limit, not a hard queue bound; the default 4096/s may be too low for a legitimate
    high-throughput peer.
  - **Next:** expose the budget in config; add a bounded inbox/drop policy at the actor mailbox;
    document the tuning knobs (022).
  - **Acceptance:** a slow actor's backlog is bounded by config; a legitimate bulk peer can be
    configured without dropping; no regression in the distribution tests.
  - **Verify:** **pass 3 implemented.** `PostCourier::post_checked` and
    `Engine::post_checked` enforce the LIVE `HotCell` mailbox bound before enqueue;
    `LocalRouter` reclaims and counts every Block/Shed (`mailbox_dropped()`), and
    engine-internal posts use the admission-exempt governed enqueue so Dormant/lazy
    reactivation and depth accounting stay correct. Wire-facing actors opt in with
    `eng.enable_governance(sched)`; `BANK_MAILBOX_BOUND` tunes the local default.
    Live: 10,000-frame flood at bound 64 → **1,258 rate-admitted, 1,126 refused at
    admission**, RSS flat 6.6→6.9 MB; 7-node busy mTLS **33,568** transfers,
    **drops=0**, thief=0; 17 targeted engine/governance suites pass. Unit:
    `distribution_routing_test::wire_mailbox_bound` (8 enqueued, 92 reclaimed).
    `set_peer_budget` remains the rate knob.

- [x] **F16: configurable frame size + 024 chunking guidance.**
  - **Why:** the 64 MiB single-frame cap is now 4 MiB; 32 MiB is rejected (HWM stays 6.7 MB). A
    legitimate large payload now needs chunking/streaming.
  - **Next:** make the cap configurable per deployment; document 024 streaming for large logical
    messages; add a sample/test.
  - **Acceptance:** an operator can raise/lower the cap explicitly; large payloads have a
    supported streaming path; the oversized-frame test still fails closed.
  - **Verify:** `security_wire_hardening_test` builds a `FrameStream{64}` and rejects a 65-byte
    prefix; `TcpTransport::set_max_frame_body` applies it to new/existing connections;
    `run_big.sh` rejects 32 MiB at the 4 MiB default. (Implemented pass 2.)

- [x] **F17: `PeerSchema`/ABI negotiation for real schema changes.**
  - **Why:** tagged is the default (safe against tagless positional confusion), but the 016
    fingerprint folded only `(tag, wire_type)`: a reused tag changing `int32`→`int64` collided, so
    the gate still chose tagless and silently reinterpreted fields (`schema_probe`). Tagged also
    reuses tag IDs, so a genuine layout change needs schema/ABI verification.
  - **Next:** fixed in pass 3 — see verify.
  - **Acceptance:** a schema mismatch falls back to tagged/rejects; matched peers can opt into
    tagless; `schema_probe` documents the behavior. **Met.**
  - **Verify:** **pass 3 implemented.** (a) `FingerprintFolder` now folds each member's category,
    size, signedness and nested schema fingerprint, so reused-tag layout changes get distinct
    TypeKeys (`schema_probe`: matched=1, mismatch_key=0, mismatch_abi=0). (b) `register_remote`
    publishes the node's decodable message TypeKeys + ABI; `SecureTransport` sends an authenticated
    `FrameKind::Schema` advert after the mTLS handshake and exposes `Transport::peer_schema`
    (`TcpTransport`/`Loopback` too). (c) `DistributedRouter`'s default provider consults the
    transport: tagless only on an exact TypeKey+ABI match, otherwise tagged; an unknown TypeKey is
    dropped before decode. Tests: `security_handshake_test` (advert), `distribution_routing_test`
    (advertised tagless, advertised mismatch→tagged, unknown-key reject), `serialize_evolution_test`;
    **21/21**. Live 7-node mTLS: **33,564** transfers, ledger `fallbacks=0`; plaintext senders stay
    tagged (fallbacks counted).
  - **Migration (TypeKey bump):** the layout-aware fingerprint changes EVERY TypeKey. Old
    snapshots/durable records and old-binary peers carry the old key. Re-encode/migrate durable data
    with the old reader, roll the whole cluster to one schema generation, or use an explicit
    compatibility adapter. A mixed generation is never silently tagless; the receiver rejects the
    unknown key.

### P4 — supply chain and at-rest

- [~] **Supply chain: pin the remaining mutable inputs; add provenance.**
  - **Fixed:** actions pinned to SHAs; `llvm.sh` SHA-256 verified before `sudo`; mbedTLS ≥ 3.6.2
    at configure + compile time; vcpkg baseline pinned.
  - **Open:** exact `apt-get`/MSVC patch versions (runner-managed); no released-binary signing.
  - **Implemented pass 4:** one pin source `ci/pins.env`; `ci/install-deps.sh` (hash-verified LLVM
    installer, no pipe-to-sudo, apt allowlist); `ci/check_pins.sh` fails closed on an unpinned
    Action, a `*-latest` runner, a malformed LLVM hash, a vcpkg/mbedTLS mismatch, or an unlisted
    apt package — with `--self-test` negative controls; `ci/gen_sbom.sh` emits CycloneDX 1.5 + file
    hashes; the `supply-chain` CI job audits, uploads the SBOM + `sbom.sha256`, and **attests**
    it via GitHub OIDC (pinned Action); every test/perf job is `needs: supply-chain`; Windows is
    pinned to `windows-2022`. See [`SUPPLY-CHAIN.md`](SUPPLY-CHAIN.md).
  - **Acceptance:** a changed declared upstream input fails closed until deliberately bumped (audit
    + self-test); the build's exact toolchain/apt versions are recorded in the attested SBOM.
  - **Residual:** apt/MSVC patch versions follow the release-pinned runner image and are visible in
    the SBOM, not byte-locked; a container digest / exact package lock and release-binary signing
    remain future distribution work.
  - **Verify:** `bash ci/check_pins.sh --self-test` rejects a mutable action tag and a bad LLVM
    hash, then the real audit passes; `gen_sbom.sh` emits valid CycloneDX 1.5 (18 components) on
    the lab Linux box; the CI `supply-chain` job is the gate.

- [x] **At-rest data: document/encrypt persistence snapshots.**
  - **Why:** a storage reader could read actor state; the adapter had no encryption-at-rest story.
  - **Decision (P4):** durable records are sealed with `EncryptedStore<S>` backed by a KMS/HSM
    `Keyring`; a bank deployment **MUST** wrap every store (`FileStore`/SQLite/RocksDB) and also
    encrypt volumes/snapshots/backups. Plain adapters stay plaintext by default; at-rest crypto
    links only when configured; `MockCipher` is test-only.
  - **Implementation:** `AtRestContext` binds `(actor, kind, seq, fence)` as AEAD associated data;
    `EncryptedStore<S>` seals snapshots/events/batches on write and opens on read, delegating
    fencing/strict-seq/atomic-batch/snapshot≤tail unchanged. `PersistenceAdapters.md` records the
    threat model, ownership, key management/rewrap, metadata that remains visible, and a deployment
    checklist.
  - **Ownership:** the platform operator owns the KMS/keys/access policy/backups/rotation; the
    application owns the decision to wrap and the `Keyring` implementation.
  - **Verify:** `security_at_rest_store_test` (InMemory + FileStore): raw ciphertext on disk, batch
    sealing, real reopen/replay, tamper rejection, cross-actor splice rejection, wrong-key
    rejection; `security_at_rest_seal_test` retains the primitive fence/seq controls.
  - **Residual:** the envelope has no key-id and online rotation is not implemented (rewrap or a
    dual-key `Keyring`); actor ids/sequence/size/timestamp metadata remain visible (volume
    encryption); a compromised process/KMS is out of scope.

### P5 — coverage

- [~] **Expand fuzzing beyond the 2M-input decoder campaign.**
  - **Done:** 2,000,000 random inputs across `decode_tagged`, `decode_control`,
    `decode_tagless_checked` under ASan+UBSan — clean.
  - **Next:** snapshot/persistence decoders, stream credit/frame reassembly, and mixed-schema
    inputs; add a CI fuzz target with a corpus.
  - **Acceptance:** continuous fuzz coverage for every untrusted decode path; crashes triage to an
    issue, not a flaky test.

---

## Fixed and verified (reference)

| ID | Fix | Commit | Verification |
|---|---|---|---|
| F3 | AEAD AAD binds mode/kind/deadline/trace/principal | `bbcf60e` | mTLS MITM rights patch rejected |
| F4 | SWIM control sender binding; no third-party gossip `Left`; `f.to` check | `bbcf60e` | mTLS forged `Left` rejected (`status1=0`); plaintext residual F1 |
| F5 | Bounds-checked tagless decode on the wire | `bbcf60e` | ASan OOB gone; malformed frame dropped |
| F6 | Pending-handshake cap + Authenticate frame cap | `bbcf60e` | pending capped at 256 |
| F7 | `f.to == self_` in both routers | `bbcf60e` | regression test |
| F8/F18 | Dual reclaim sink required; fail closed on non-dual | `81c8d0d` | RCE victim exits 0, no hijack |
| F2 | `set_principal_policy`; deny unbound principals | `81c8d0d` | mTLS theft produces no transfer |
| F10 | Deny when no `Authorizer`; explicit `AllowAll` | `81c8d0d` | no-auth anonymous transfer gone |
| F11 | `LocalRouter::set_authorizer` | `81c8d0d` | forward probe local Bank denied |
| F12 | Verified placement view, Join-self, live cap | `81c8d0d` | 512 ghosts, `owner=1`, `pview=1` |
| F13 | Global stale-handshake reaper + admission hook | `81c8d0d` | retry after 6s succeeds |
| F14 | Random per-instance relay secret | `81c8d0d` | forged token stays Suspect |
| F15 | Per-peer token bucket | `81c8d0d` | 10k flood → 1,277 admitted |
| F16 | 4 MiB single-frame cap | `81c8d0d` | 32 MiB rejected, HWM 6.7 MB |
| F17 | Layout-aware fingerprint + authenticated PeerSchema advert | `06c69c9`+ | `schema_probe` gate; mixed-schema tagged/reject; live mTLS tagless |
| P4 at-rest | `EncryptedStore<S>` + KMS Keyring decision/checklist | pass 4 | store test: ciphertext/reopen/tamper/splice/wrong key |
| P4 supply chain | Pin manifest + audit self-test + CycloneDX SBOM + attestation | pass 4 | mutable action/bad hash rejected; SBOM valid (18 components) |
| Supply chain | Action SHAs, LLVM hash, mbedTLS version | `81c8d0d` | CI no longer pipes to sudo |

---

## Remediation pass 2 — 7-node busy lab (2026-10-25)

Heavier case: **7 nodes**, ledger pinned to node 1, six authenticated client nodes generating
sustained traffic (2 threads/client, 200 tx/s/thread target), then theft/eviction/MITM under load.
The production build refuses the plaintext variant before `start()`.

| Evidence | Result |
|---|---|
| 7-node mTLS, 35 s | **34,548** busy `Transfer` frames; alice 1,000,000→~966k, bob→~34k, **thief=0**; 6 sessions |
| mTLS theft / eviction / MITM | no attacker `authz`/`bank_transfer`; node 2 `status1=0` (forged `Left` rejected); AAD-patched MITM rejected |
| 7-node plaintext lab | **26,666** busy transfers; theft=0; forged `Left` still evicts (`status1=3`) — the remaining **F1** residual |
| strict refusal | `bank_strict` (no lab macro) with 3-node plaintext exits **3**: `Strict + multi-node ... plaintext` |
| per-IP handshake flood | 3000 same-IP senders → **64** pending; allowlisted peer authenticates mid-flood; unknown peer succeeds after the 5s reaper |
| hard mailbox bound (64) | 10,000 sent → 1,258 rate-admitted → **1,126 refused at admission**; RSS 6.6→6.9 MB |
| 7-node busy mTLS + F15 | **33,568** transfers, **drops=0**, thief=0, 6 sessions |
| F17 schema gate | `schema_probe`: matched=1, mismatch_key=0, mismatch_abi=0 |
| F17 mixed-schema routing | advertised tagless; mismatch→tagged; unknown TypeKey rejected |
| F17 live 7-node mTLS | **33,564** transfers, ledger `fallbacks=0`; plaintext senders stay tagged |
| P4 pin audit | self-test rejects mutable action + bad LLVM hash; real audit passes |
| P4 SBOM | valid CycloneDX 1.5, 18 components, file SHA-256s |
| P4 at-rest | InMemory+FileStore ciphertext; reopen, tamper, cross-actor, wrong key controlled |
| 21/21 affected CTest suites + 17 targeted core | pass |

Implemented in this pass:

- **P0:** `Transport::is_plaintext()` + `validate_startup_security()`; the ledger sets
  `SecurityMode::Strict` and refuses multi-node plaintext. Lab plaintext must build with
  `QUARK_ALLOW_INSECURE` and prints a warning.
- **P1:** the busy ledger installs `LocalRouter::set_authorizer`; its system thread stamps
  `{subject=1,rights=4}` so the local ACL admits legitimate internal work.
- **P2:** SWIM Control token bucket (`control_rate_per_sec=4096`, `burst=1024`, fixed 256 buckets);
  unverified-member TTL reaper (30 s default; 1 ms regression test); transport pending-inbound
  cap (1024); **address-aware handshake admission**: PAL accept reports the peer address,
  `TcpTransport` attaches it transiently, `SecureTransport` enforces a per-IP pending cap
  (default 64) plus 16 reserved slots for allowlisted peering identities.
- **P3:** `FrameStream::set_max_body()/max_body()`, `TcpTransport::set_max_frame_body()`, and
  `TcpTransport::set_max_pending_inbound()`; 4 MiB default retained; **hard mailbox bound**:
  checked post admission (`PostCourier::post_checked` → `Engine::post_checked` →
  `Activation::post_governed`) with drop-and-reclaim at `LocalRouter`, admission-exempt internal
  posts, and `enable_governance` opt-in on the wire-facing ledger.
- **P3/F17:** layout-aware 016 fingerprint (member category/size/signedness/nested schema);
  authenticated post-handshake `FrameKind::Schema` advert; `Transport::peer_schema`; the
  `DistributedRouter` default provider requires an exact TypeKey+ABI match before tagless.
- **P4 at-rest:** `EncryptedStore<S>` + `AtRestContext` AAD; KMS/HSM `Keyring` decision,
  ownership, rotation/rewrap and deployment checklist in `PersistenceAdapters.md`.
- **P4 supply chain:** `ci/pins.env`, `ci/install-deps.sh`, fail-closed `ci/check_pins.sh`
  (+ self-test), CycloneDX `ci/gen_sbom.sh`, and the attested `supply-chain` CI gate.
- **Test seam:** `DistributedRouter::tell_on()` pins the lab ledger; production routing still uses
  placement.

## Migration for applications (fail-closed defaults)

```cpp
// 1. Refuse insecure startup.
EngineConfig ec{/*workers*/1, /*shards*/1, /*budget*/64, 64};
ec.security_mode = SecurityMode::Strict;
if (auto r = validate_startup_security(ec.security_mode, cluster_size, transport); !r) {
    // multi-node plaintext is rejected; log r.error().detail and do not start.
}

// 2. Boundary: install an ACL and, only if delegated, a principal binder.
dist.set_authorizer(authz);                 // AllowAllAuthorizer is an explicit DEV opt-in.
dist.set_principal_policy(binder);          // required for non-anonymous wire principals.
dist.set_peer_budget(4096, 1024);           // tune for a legitimate bulk peer.

// 3. Internal: dual reclaim for wire-facing actors, optional local ACL.
act = std::make_unique<Activation>(&actor, Actor::dispatch_table(), local.reclaim_sink());
local.set_authorizer(authz);                // internal forwards become a checked boundary.
// Stamp system-originated local work with a privileged Principal via AmbientContextScope.

// 4. Wire-facing actors opt in to the hard mailbox bound (live HotCell, 022).
auto* sched = eng.register_activation(actor_id_of<Ledger>(1), *act);
eng.enable_governance(sched);               // Overflow::Block sheds at admission
// Monitor local.mailbox_dropped(); a slow actor's backlog cannot exceed the bound.

// 5. Framing / connections (TcpTransport).
transport.set_max_frame_body(4u * 1024u * 1024u);
transport.set_max_pending_inbound(1024);

// 6. Placement: use the verified placement view, not the convergence roster.
owner = place(id, membership.placement_view());

// 7. Schema rollout: register_remote publishes each message TypeKey; the mTLS handshake advertises
//    them. Tagless is chosen only on an exact peer TypeKey+ABI match; a mismatch uses tagged, and
//    an unknown TypeKey is dropped. A layout change changes the TypeKey: migrate durable data and
//    roll the whole cluster to one schema generation (never mix fingerprints silently).
dist.register_remote<Ledger, Transfer>();
```

---

## Operating checklist (current expectations)

- [ ] Multi-node deployments use mTLS, not `TcpTransport`; do not expose plaintext.
- [ ] Every wire-facing node installs an `Authorizer` (or an explicit `AllowAllAuthorizer`).
- [ ] Non-anonymous wire principals require a `PrincipalPolicy`; never trust the raw frame principal.
- [ ] Wire-facing activations use `LocalRouter::reclaim_sink()`.
- [ ] Authorization-sensitive internal forwarding installs a `LocalRouter` authorizer.
- [ ] Set a per-peer budget, a single-frame cap, and a mailbox bound appropriate to the deployment;
  opt wire-facing actors into `enable_governance` and alert on `mailbox_dropped()`.
- [ ] Treat `Membership::view()` as the convergence roster; use `placement_view()` for placement.
- [ ] Roll schema changes as one generation; the fingerprint is layout-aware, so mixing binaries
  mixes TypeKeys and disables tagless/rejects unknown keys.
- [ ] Wrap every durable store in `EncryptedStore` with a KMS/HSM key; encrypt volumes and backups;
  run `security_at_rest_store_test`; never ship `MockCipher`.
- [ ] Pin/verify build inputs; never pipe a remote script into `sudo`; review the attested SBOM diff
  on every pin bump.

---

## Repro / verification commands (lab)

On `thnak-server:~/quark-sec/sec` (mirror: `.commandcode/tmp/sec`):

```bash
bash run_tests.sh          # 21 affected suites
bash run_batch1.sh         # RCE + fail-open (both blocked)
bash run_forward.sh        # local ACL (blocked)
bash run_place.sh          # placement view (ghosts in view, not placement)
bash run_slot_exhaust.sh   # reaper: retry succeeds after 5s
bash run_flood.sh          # per-peer budget + 64-entry mailbox (10k -> ~1.3k admitted, ~1.1k shed)
bash run_big.sh            # 32 MiB rejected at 4 MiB cap
bash run_bank.sh           # plaintext + mTLS bank attacks
bash run_bank_heavy.sh     # 7-node busy mTLS/plaintext + strict-refusal case
./relay_probe              # forged token rejected (exit 1)
./fuzz_wire 2000000        # decoder fuzz clean
./schema_probe             # F17 gate: matched=1, mismatch_key=0, mismatch_abi=0
bash run_f17_live.sh       # schema probe + 7-node mTLS tagless / plaintext tagged
./security_at_rest_store_test   # P4 at-rest: ciphertext/reopen/tamper/splice/wrong key
bash ci/check_pins.sh --self-test   # P4 supply chain: negative controls + real audit
bash ci/gen_sbom.sh > sbom.json     # P4 CycloneDX SBOM (attested in CI)
```
