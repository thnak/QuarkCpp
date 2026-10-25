# Security Remediation Tracking

**Source:** `SECURITY-FINDINGS.md` (live plaintext + real-mTLS bank cluster attack pass)  
**Updated:** 2026-10-25  
**Status key:** `[x]` fixed & verified · `[~]` mitigation in place, follow-up open · `[ ]` open

Verified baseline: **21/21 affected CTest suites pass** on `thnak-server` (real mbedTLS 3.6.5,
ASan/UBSan where applicable). Commits: `bbcf60e` (wire hardening), `f1286ee` (attack matrix),
`81c8d0d` (fail-closed remediation).

---

## Open / follow-up backlog

### P0 — do not operate a plaintext cluster until this is closed

- [ ] **F1/F9: refuse plaintext at startup; wire `SecurityMode::Strict`.**
  - **Why:** the hardened build still accepts a plaintext `TcpTransport`; a forged SWIM `Left`
    removes the ledger (`status1=3` in the live run) because there is no authenticated sender.
  - **Next:** call `validate_security` from `validate_engine_config`/node bootstrap with
    `cluster_size > 1` + transport kind; reject `Strict + plaintext`; make mTLS the default for
    multi-node deployments.
  - **Acceptance:** a strict, multi-node, plaintext config fails before `start()` with a clear
    diagnostic; the plaintext bank PoC cannot boot in strict mode; the mTLS path is unaffected.
  - **Verify:** new config test + `run_bank.sh` strict variant.

### P1 — authorization and internal trust

- [ ] **F11: adopt `LocalRouter::set_authorizer` in authorization-sensitive apps (bank sample).**
  - **Why:** the hook exists and blocks the Gate→Bank rights=1→4 escalation, but it is opt-in;
    default internal sends remain trusted by design.
  - **Next:** add a local-ACL example to the bank-style sample/docs; decide whether privileged
    actors should also assert `ctx.principal` in the handler as defense in depth.
  - **Acceptance:** `forward_probe` remains blocked; a documented pattern shows the local ACL;
    no hot-path change when unset.
  - **Verify:** `run_forward.sh`.

- [ ] **F2/F10/F8: publish a migration guide for the new fail-closed defaults.**
  - **Why:** non-anonymous wire principals now drop without `set_principal_policy`; inbound data
    drops without `set_authorizer`; non-anonymous descriptors drop for `pool.sink()` activations.
  - **Next:** README/sample notes: install `AllowAllAuthorizer` (or a real ACL), a principal
    binder, and `LocalRouter::reclaim_sink()` for wire-facing actors.
  - **Acceptance:** every sample still builds/runs; the guide names the three defaults and the
    opt-in APIs.
  - **Verify:** samples 08/15–19/22 build; the integration suite passes.

### P2 — pre-auth and control-plane DoS

- [ ] **F13: per-IP/connection handshake cap + reserved slots.**
  - **Why:** the global reaper frees slots after 5s, but 256 distinct spoofed senders can still
    fill the cap for that window; `set_handshake_admission` is a NodeId predicate, not an address
    cap. The legitimate node-0 retry succeeds after the timeout (verified).
  - **Next:** plumb the remote endpoint from `TcpTransport` into the admission decision, or add a
    per-connection token bucket at the transport; reserve a small slot set for known/verified peers.
  - **Acceptance:** a 3000-sender flood cannot deny a known peer's handshake within the window;
    memory stays bounded; the legitimate retry remains successful.
  - **Verify:** extend `run_slot_exhaust.sh` with a known-peer reservation.

- [ ] **F1 (control plane): rate-limit SWIM Control frames.**
  - **Why:** the F15 bucket covers `DistributedRouter` **data** frames; a peer can still flood
    Control/Gossip frames (CPU/republish cost).
  - **Next:** per-peer budget for `SwimMembership::on_frame`, or fold Control into the transport
    admission gate.
  - **Acceptance:** a Control flood is capped while normal gossip/probe rates pass.
  - **Verify:** a control-flood harness; `cluster_swim_*` suites stay green.

- [ ] **F12: cap/GC unverified members and bound probe work.**
  - **Why:** `max_live_members=4096` bounds the table, but ghosts still enter the convergence
    `view()` (512 in the live test) and each creation triggers `republish()`; a ghost flood can
    consume view/CPU memory even though placement is protected.
  - **Next:** lower/tune the cap per deployment; evict long-lived unverified members; bound
    `next_probe_target` work; consider not republishing unverified entries in `view()` behind a
    config flag if consumers can use `placement_view()`.
  - **Acceptance:** ghost injection is bounded in memory and CPU; `placement_view` remains clean;
    join/rejoin convergence still passes.
  - **Verify:** `run_place.sh` + `cluster_swim_partition_heal_test`.

### P3 — resource governance and framing

- [ ] **F15: hard mailbox bound / backpressure; tune the per-peer budget.**
  - **Why:** the token bucket caps ingress (10k → 1,277 admitted, RSS 6.5→7.1 MB), but it is a
    rate limit, not a hard queue bound; the default 4096/s may be too low for a legitimate
    high-throughput peer.
  - **Next:** expose the budget in config; add a bounded inbox/drop policy at the actor mailbox;
    document the tuning knobs (022).
  - **Acceptance:** a slow actor's backlog is bounded by config; a legitimate bulk peer can be
    configured without dropping; no regression in the distribution tests.
  - **Verify:** `run_flood.sh` at multiple budgets.

- [ ] **F16: configurable frame size + 024 chunking guidance.**
  - **Why:** the 64 MiB single-frame cap is now 4 MiB; 32 MiB is rejected (HWM stays 6.7 MB). A
    legitimate large payload now needs chunking/streaming.
  - **Next:** make the cap configurable per deployment; document 024 streaming for large logical
    messages; add a sample/test.
  - **Acceptance:** an operator can raise/lower the cap explicitly; large payloads have a
    supported streaming path; the oversized-frame test still fails closed.
  - **Verify:** `run_big.sh` at 4 MiB and a configured larger cap.

- [ ] **F17: `PeerSchema`/ABI negotiation for real schema changes.**
  - **Why:** tagged is now the default (safe against tagless positional confusion), but tagged
    reuses tag IDs; a genuine layout change with reused tags still needs schema/ABI verification.
  - **Next:** wire the handshake-advertised `PeerSchema` into `set_peer_schema_provider`; require a
    match before tagless; define a version/migration policy for reused tags.
  - **Acceptance:** a schema mismatch falls back to tagged/rejects; matched peers can opt into
    tagless; `schema_probe` documents the behavior.
  - **Verify:** `schema_probe`, `serialize_evolution_test`, a mixed-schema integration test.

### P4 — supply chain and at-rest

- [ ] **Supply chain: pin the remaining mutable inputs; add provenance.**
  - **Fixed:** actions pinned to SHAs; `llvm.sh` SHA-256 verified before `sudo`; mbedTLS ≥ 3.6.2
    at configure + compile time; vcpkg baseline pinned.
  - **Open:** `apt-get install` versions, `windows-latest`/MSVC, and the LLVM hash/action-SHA
    update process; no SBOM/signature verification.
  - **Next:** a container image digest or pinned package versions; a documented bump procedure;
    SBOM/provenance generation in CI.
  - **Acceptance:** a changed upstream input fails closed until deliberately bumped; the build is
    reproducible from pinned inputs.
  - **Verify:** CI dry-run + a hash-mismatch negative test.

- [ ] **At-rest data: document/encrypt persistence snapshots.**
  - **Why:** `PersistenceAdapters.md` has no encryption-at-rest story; a storage reader can read
    actor state. This is outside the network threat model but relevant to a bank.
  - **Next:** document the storage threat, key management, and whether the adapter/volume
    encrypts; add a deployment checklist.
  - **Acceptance:** an explicit at-rest encryption decision and ownership are recorded.

### P5 — coverage

- [ ] **Expand fuzzing beyond the 2M-input decoder campaign.**
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
| F17 | Tagged default; tagless explicit opt-in | `81c8d0d` | routing/integration tests pass |
| Supply chain | Action SHAs, LLVM hash, mbedTLS version | `81c8d0d` | CI no longer pipes to sudo |

---

## Operating checklist (current expectations)

- [ ] Multi-node deployments use mTLS, not `TcpTransport`; do not expose plaintext.
- [ ] Every wire-facing node installs an `Authorizer` (or an explicit `AllowAllAuthorizer`).
- [ ] Non-anonymous wire principals require a `PrincipalPolicy`; never trust the raw frame principal.
- [ ] Wire-facing activations use `LocalRouter::reclaim_sink()`.
- [ ] Authorization-sensitive internal forwarding installs a `LocalRouter` authorizer.
- [ ] Set a per-peer budget and a single-frame cap appropriate to the deployment.
- [ ] Treat `Membership::view()` as the convergence roster; use `placement_view()` for placement.
- [ ] Pin/verify build inputs; never pipe a remote script into `sudo`.

---

## Repro / verification commands (lab)

On `thnak-server:~/quark-sec/sec` (mirror: `.commandcode/tmp/sec`):

```bash
bash run_tests.sh          # 21 affected suites
bash run_batch1.sh         # RCE + fail-open (both blocked)
bash run_forward.sh        # local ACL (blocked)
bash run_place.sh          # placement view (ghosts in view, not placement)
bash run_slot_exhaust.sh   # reaper: retry succeeds after 5s
bash run_flood.sh          # per-peer budget (10k -> ~1.3k admitted)
bash run_big.sh            # 32 MiB rejected at 4 MiB cap
bash run_bank.sh           # plaintext + mTLS bank attacks
./relay_probe              # forged token rejected (exit 1)
./fuzz_wire 2000000        # decoder fuzz clean
./schema_probe             # tagless schema reinterpretation evidence
```
