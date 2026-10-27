# Open-issues plan

Snapshot of open GitHub issues as of 2026-10-27 (#4, #10, #13, #17–#25), grouped into five phases.
Issues #18–#25 are unverified reviewer readings: every item starts with a reproduction test and may
be closed as invalid. Spec-first: when code and spec disagree, fix the spec (with an ADR) before the code.
Build with `cmake --build build` (never unbounded `-j`; TSan `-j1`).

## Phase 1 — Correctness first

- **#18** Message-type key collisions (wrong remote handler, `recover()` misrouting user events).
  1. Failing tests: two same-layout message types rejected at startup or given distinct keys;
     a user event shaped like `WatermarkRecord` round-trips through `recover()`; check local dispatch.
  2. Fix (type-name/tag salt in the key + startup collision scan for message types). ADR (008/016).
- **#24 (part a)** Failed snapshot on passivation must keep the actor active or surface an error.

Exit: new tests green under ASan/UBSan/TSan; ADR written.

## Phase 2 — Spec says done, code isn't

- **#19** `Delivery<AtLeastOnce>`: retry + ack for remote `tell`; durable delivered marker in the
  `EffectivelyOnceLane` outbox. Test with 30% frame drop.
- **#24 (part b)** Implement `CompactEvery` (ADR-024); trim dedup watermark and reply-cache maps;
  free `lazy_owned`/`id_table` on idle eviction. RSS test with 10^6 lazy keys; add a bench.

Exit: every Accepted-spec promise in these areas has a test; bench-gate passes.

## Phase 3 — Cluster operability

- **#21** Membership events (Joined/Suspect/Dead/Left + epoch) and per-node health metadata.
- **#20** Wire versioning for SWIM + TCP hello, N/N-1 window, distinct refusal reason for N-2
  (builds on #21 for surfacing the refusal). Gate: 5-process mixed cluster passes `run_swim_chaos.sh`.
- **#17** Mixed Windows/Linux (and Linux/arm64) conformance job; document bulk-transfer path/limits and
  which transport behaviours are identical across PAL backends (docs part can start immediately).

Exit: mixed-version and mixed-OS CI legs green; docs published.

## Phase 4 — Persistence and observability

- **#25** Per-actor-type handler busy-time and allocated bytes in 009 metrics, <2% overhead in bench gate.
- **#23** Online snapshot/restore of a `Store` with a fencing epoch; old owner's writes rejected.
  Share the fencing-token concept with #22.

Exit: bench-gate overhead within budget; restore-into-fresh-cluster test passes.

## Phase 5 — Design-pipeline and platform items

Hot-path/contested work goes through `design-debate-prove` and produces ADRs, not ad-hoc changes.

- **#10** Ordered, reliable multi-subscriber fan-out (open question: block producer vs evict slow
  subscriber with gap notice).
- **#4** Mailbox contention under many producers (Vyukov MPSC collapse vs CAF).
- **#22** Leases and leader election with fencing tokens (new RFC; 10^4 seeded partition simulations).
- **#13** Windows/ARM64 PAL backend, CI leg on `windows-11-arm`, and IOCP for Windows/x86-64 (needs runners).

Exit: ADR/RFC per item; each proven by executed C++ under sanitizers + benches.
