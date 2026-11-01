# ADR-067 — Direction: Quark becomes AeroCoWorker's network center, not a general actor engine

Status: **Accepted as direction — owner decision, 2026-10-31.** This is a product and scope decision, not a technical proof, so it has no executed
evidence and claims none. Every feature it prioritises still needs its own spec section and, where hot-path or safety-critical, its own executed
ADR. Mirror record on the consumer side: AeroCoWorker
[ADR-045](https://github.com/thnak/AeroCoWorker/blob/master/decisions/ADR-045-quarkcpp-network-center.md).

## 1. The decision (owner)

> "Change QuarkCpp's product direction to avoid competing directly with CAF, which is already open source and more complete. QuarkCpp's code
> becomes AeroCoWorker's network-processing center (the network stacks AgentEngine manages stay as they are for now)."

1. **Quark no longer aims to be a general-purpose distributed actor engine.** It does not compete with CAF (the C++ Actor Framework): no general
   distributed-actor product roadmap, no "10³–10⁴ nodes for any application" promise, no general-engine comparison as a goal.
2. **Quark's purpose is AeroCoWorker's network center:** every machine-to-machine path AeroCoWorker has: its node-to-node plane, the hosted
   Team and its hub, the serverless LAN group, phones and devices attaching to their home PC, relays, and headless Linux nodes. AeroCoWorker links
   Quark only inside its HAL (its network module), so Quark's platform layer is part of that product's OS boundary.
3. **Kept and maintained (the network center's machinery):** the scheduler and actor runtime as internal machinery, transport and
   `SecureTransport`, SWIM membership and its events, relay (including the sealed shape), the replay guard, the wire-version window, backup with
   fencing where AeroCoWorker's hub needs it, and the PALs.
4. **Frozen:** features no network use needs today — general actor placement as a product, persistence adapters beyond what the hub needs,
   supervision as a product surface, the CAF comparison as a goal. **Frozen means:** no new work and no new promises; existing tests keep passing;
   removal only by its own ADR when the code gets in the way.
5. **Quark may be shaped by AeroCoWorker's needs.** Specs and APIs may name AeroCoWorker's use cases. **This repo's process is unchanged:** the spec
   wins, contested or safety-critical changes get an executed ADR, and the ASan/UBSan/TSan and benchmark gates stay.

## 2. Consequences

- **The roadmap is AeroCoWorker's network work**, already filed: #35 (issuer-authenticated revocation), #36 (certificate issuance and
  enrolment), #37 (non-member authenticated client sessions), #40 (pinned-key handshake), #41 (application-driven admission and eviction),
  #42 (LAN discovery adapter), #43 (an `android_arm64` PAL; phones are now first-class users, so this is required, not optional), #44 (several
  isolated clusters in one process).
- **Windows** (x64 and ARM64) and **Android** become first-class targets beside Linux, because AeroCoWorker's Host is Windows-first and its client
  is Android (ADR-059 is the Windows/ARM64 start; #43 the Android one).
- **README and CLAUDE.md** describe Quark this way. The CAF comparison stays as a historical record of the hot-path claims, not as a positioning.
- **Out of scope:** AgentEngine's own network stacks (model providers, its MCP transports) stay AgentEngine's; AeroCoWorker's local shell-to-Host
  IPC and its third-party HTTP calls are not Quark's.

## 3. Risks

- **Narrower audience, deeper coupling:** a single consumer sets the direction; a change made for it must still keep this repo's own invariants and
  gates, or Quark's quality becomes AeroCoWorker's risk.
- **Frozen code still compiles and ships** until removed; its tests keep running so it does not rot silently.
