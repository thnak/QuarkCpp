# Quark — production-readiness checklist

**Verdict: not yet "production-ready" as a platform commitment; usable in production for the
narrow profile below.** The engine is implemented, heavily gated, and unusually well documented for
a pre-1.0 project. What is missing is not one big feature but a set of named correctness,
portability, release-engineering, and operational gaps. This file is the honest ledger — each item
names its spec/ADR and the exit criterion that closes it.

Snapshot: `master` @ `a602f29` (2026-10-25). Sources: `OpenQuestions.md`, the spec status blocks,
`CONVENTIONS.md`, `VERIFICATION.md`, `PERFORMANCE.md`, and the runs recorded there.

## Supported profile today

Safe to run in production **if all of these hold**:

- **Linux / x86-64**, built from source at a pinned commit with **g++ 15.x or clang 21.x**, C++23,
  CMake. This is the only fully gated target.
- **Per-actor mailbox traffic.** Do not put one hot mailbox behind many producers (see B9).
- **Direct TCP reachability** between nodes. Relay *fallback* exists, but relay discovery,
  health/failover, and the relay trust boundary are unbuilt (B6) — no NAT/firewall traversal.
- **One toolchain per durable store.** Do not share persisted records or wire identities across
  GCC/Clang/MSVC until B1/B2 close.
- **Single datacenter / low-latency links.** The SWIM timers are tick-quantized defaults; WAN
  deployments must raise `indirect_timeout_ns` per 021's `4d` rule.
- **Cluster sizes near the tested range** (the multi-process harness exercises 5 nodes; 10³–10⁴ is a
  design claim backed by in-process/simulation gates, not a chaos-tested deployment).
- **In-house operations.** There is no packaging, release artifact, or ops guide (B10/B11).

## Gate status

| Dimension | Status | Evidence / gap |
|---|---|---|
| Correctness gate | **Strong** | 211/211 Release, 194/194 TSan, ASan+UBSan clean on the changed slice, 5/5 SWIM chaos scenarios |
| Hot-path performance | **Strong, gated** | `bench-gate` with 023 budgets; ~45–46 M msg/s/core, p50 38–39 ns, enqueue 25–28 ns |
| Memory/ABI footprint | **Stable** | 56 B descriptor, 192 B idle mailbox, backlog-depth-flat p99 |
| API/ABI stability | **Not established** | No `VERSION`, `CHANGELOG`, or stated compatibility/deprecation policy |
| Platform coverage | **Asymmetric** | Linux x86-64 full; ARM64 CI correctness (perf informational); Windows MSVC correctness only (no TSan/UBSan, no perf gate); macOS has no PAL backend |
| Cluster/distribution | **Core accepted** | FIFO-under-relay proven, SWIM heal proven; relay discovery/trust and cross-node broadcast still Draft/unbuilt |
| Persistence | **Seam, not a product** | `Store` concept + conformance harness; no bundled production database adapter guarantee |
| Security | **Accepted core** | mTLS + rotation, principal propagation, DoS governance; relay trust boundary open |
| Observability | **In-process** | Counters/metrics registry (009); deployment-facing export/scrape not covered here |
| Release engineering | **Missing** | Source-only CMake consumption; no packages, no release process |
| Documentation accuracy | **Drifting** | README/VERIFICATION counts were stale; `wiki/` mirror stops at ADR-019 and a 153-test suite |

## Blocking items

### B1 — Durable type identity is not unique for described message types
`fingerprint_v<T>` deliberately excludes the name, so structurally identical types collide:
`Point<int,int>`, `Other<int,int>`, and `Account<uint64,int64>` all fold to the same key. Registered
*actor* types are safe (name-seeded + Strict collision scan); described *message* types are not.
This is the one issue that can silently corrupt durable state. See `OpenQuestions.md` item 2.
**Exit:** an ADR splitting the roles — shape fingerprint for wire/schema negotiation, a separate
normalized-or-explicit key for durable identity — plus a collision scan for message types.

### B2 — `type_key` is not portable across toolchains for builtin spellings
Tier 1 (described types) is byte-identical across GCC/Clang/MSVC. Tier 2 (user names) diverges
pairwise and is asserted per compiler. Tier 3 (builtin spellings, e.g. `size_t`) diverges across all
three and is recorded-not-asserted. Tier 2 goldens are also toolchain-*version*-fragile.
**Exit:** normalize spellings (strip the MSVC elaborator, canonicalize builtins) or mandate explicit
per-type keys; promote Tier 3 to Tier 1. See `OpenQuestions.md` item 1 and `VERIFICATION.md`.

### B3 — `IoContext::add_fd`/`del_fd` semantics differ by backend
On Linux/epoll, `add_fd` on a live fd fails with `EEXIST` (silently if ignored); the Windows backend
overwrites. The in-tree callers now deregister before closing/re-adding (`retire_fd`,
`cutover_to_direct`), but the backend contract itself is not hardened.
**Exit:** idempotent/replace-semantics `add_fd` on both backends, plus a churn test that recycles fd
numbers during register/close.

### B4 — AArch64 weak-memory proof is missing
All mailbox handoff evidence is x86-TSO. Two edges (exec-state release/acquire carrying `head_`, and
the lost-wakeup/publication edges) are not formally proven on a weakly ordered target. ARM64 CI
correctness is empirical, not the herd7/GenMC proof `TODO(arm64)` calls for.
**Exit:** formal/abstract-model litmus proof, or an explicit decision to accept ARM64 on CI evidence
only. This blocks ARM64 promotion, not x86-64.

### B5 — Relay trust boundary and relay lifecycle are unbuilt
The `DirectThenRelay<TimeoutMs>` mechanism is proven, but: the relay `Endpoint` is static config
(no discovery/advertisement), relay-down is indistinguishable from peer-down in diagnostics,
multi-relay failover does not exist, and the relay sees routing metadata even under AEAD sealing.
**Exit:** an ADR + implementation for discovery, health/failover, and relay authentication; until
then, no NAT/firewall deployment.

### B6 — Cross-node broadcast and gateway topologies are Draft
`Topic` is Accepted for local fan-out only; cross-node broadcast is Draft on GATE 7. `026`'s gateway
topology variant is Draft on the same proofs.
**Exit:** execute the gates named in 006/026 and promote, or scope them out explicitly.

### B7 — Oversubscription tail latency is unresolved
ADR-038's drain-owner eviction is implemented but default-off; a quiet, pinned host is needed to
separate the effect from the shared box's noise floor (round 4 flipped sign between sessions).
**Exit:** re-baseline on a dedicated quiet host; then either land a default or document the limit
permanently. All knobs stay at default-off meanwhile.

### B8 — One hot shared mailbox does not scale with producers
Single-mailbox producer scaling collapses at P=2 (~0.55×) via the shared `tail_` — the known ADR-031
signature, not a regression. Engine scaling was 1.87× at 4 shards on 4 pinned cores.
**Exit:** this is a documented design trade-off; close it either with workload guidance (per-actor
mailboxes) or an opt-in throughput-prioritizing allocator (OpenQuestions item 5).

### B9 — Release engineering and API stability are absent
No `VERSION`, no `CHANGELOG`, no semver/deprecation policy, no packages or installable targets.
**Exit:** a versioning policy, a changelog, install/export rules, and a stated compatibility window.

### B10 — Operational surface is not in the repo
No deployment guide, no verified metric export/scrape path, no backup/restore runbook for the
persistence adapters, no health/readiness endpoint, no soak/long-running leak run.
**Exit:** ship an ops guide plus one supported persistence adapter with a tested restore path, and a
soak test in CI (or a documented periodic job).

### B11 — Documentation drift undermines auditability
At the time of this snapshot: README claimed 194 tests (actually 211), VERIFICATION's "latest run"
was from July at 153, CONVENTIONS said 153, and `wiki/` is a legacy mirror stuck at ADR-019.
**Exit:** counts are now refreshed in README/CONVENTIONS/VERIFICATION; the `wiki/` mirror still needs
a full regeneration or removal in favour of the generated `docs/` site.

## Verification snapshot (`a602f29`, 28-core devserver)

- Release (g++ 15.2): **211/211**.
- TSan (g++, full config): **194/194**, including `tcp_transport_control_priority_test` this run.
- ASan+UBSan: **13/13** on the changed transport/SWIM/security slice; full-suite ASan last recorded
  209/210, the miss being the known `topic_no_quiesce_control` weakness.
- SWIM chaos (5 processes, netns, iptables/netem): **5/5** scenarios end all-alive.
- `bench-gate`: PASS, 0 hard violations; performance unchanged by the merge.

## Suggested order to close

1. B1/B2 (type identity) — the only items that can silently corrupt data.
2. B3 (fd semantics) + B10's soak test — cheap hardening with a clear test.
3. B5 (relay trust/lifecycle) — required before any NAT/firewall story.
4. B9/B10/B11 (release + ops + docs) — required before external adoption.
5. B4/B6/B7/B8 — promotion work that does not block Linux/x86-64 production use.
