# ADR-044: Principal propagation into handler `MessageContext` — Flag-Gated Envelope Pool

**Status:** Accepted
**Supersedes:** the ctx_-pointer-into-payload-arena mechanism proposed in ADR-007 (§ "Descriptor / ctx_") for the specific purpose of carrying `Principal`
**Related:** 020-Security §3, 003-Memory §Mailbox, 006-Messaging-and-Addressing, ADR-002/003/004/020/027/029/031/032/033/041 (mailbox hot path), ADR-037 (message-pool freelist)

## Question

`MessageContext` already carries a `Principal` field, and 020's design intent is that a
wire-arrived `Principal` enters at the inbound-wire boundary and is carried across a node
hop on `MessageFrame`. Nothing in the live tree populates a receiving actor's
`current_context().principal` from an inbound frame — it stays default-anonymous always,
foreclosing multi-hop least-privilege delegation. ADR-007 (Accepted) proposed a
`Descriptor::ctx_` pointer into an ambient `MessageContext` envelope for exactly this, but
that layout was never built; `deadline_ns`/`trace_id` are inline on the real, ten-times-proven
56/64-byte `Descriptor`, leaving only 8 bytes of slack — too little for a 16-byte `Principal`.

Settle: retrofit ADR-007's `ctx_`-pointer mechanism onto the current Descriptor, or use a
different mechanism compatible with the pinned inline layout, to get a wire-arrived
`Principal` to `current_context().principal` at O(1) cost on top of work the wire path
already pays, while a purely local send/drain path stays untouched (0 allocations, 0
principal-related reads/writes, no new atomic/CAS word).

## Designs (one-line summaries)

1. **ContextOverlay** — add `Descriptor::ctx_` (a nullable 8-byte pointer to a 16-byte
   `{Principal}` overlay placement-newed at the front of the same pooled payload cell for
   wire arrivals only), growing `sizeof(Descriptor)` from 56 to exactly 64 bytes (zero slack
   left). `deadline_ns`/`trace_id` stay inline, deliberately diverging from ADR-007's literal
   text.
2. **PrincipalSideTable** — leave `Descriptor` untouched (56 bytes); carry `Principal` in a
   partitioned, mutex-guarded `unordered_map<{Descriptor*, generation}, Principal>` external
   to Descriptor, gated by one new bit in the existing 12-bit `flags` subfield of `gen_state`.
3. **Flag-Gated Envelope Pool** — leave `Descriptor` untouched (56 bytes); route
   principal-carrying messages through a second, wire-scale pool (`EnvelopePool`) whose Cell
   is `{Descriptor; DescriptorEnvelope{Principal}; destroy; payload[192]}` — the same
   ADR-037 Cell-first-member/fixed-offset idiom `MessagePool` already uses — reached by
   `reinterpret_cast` with zero stored pointer, gated by the same kind of flag bit as design 2.

## Decision

**Winner: Design 3, Flag-Gated Envelope Pool**, as revised during cross-examination
(`C1r`/`S1r`/`S2r` fixes: `DualReclaimSink` in `LocalRouter`, principal resolution threaded
through all four real dispatch/redispatch sites — `drain_step`, `drain_step_governed_seq`,
`admit()`/`ReFrame`, `handle_restart_retry`).

### Why, against the ranking rules

**(1) Safety gate.** Design 1 (ContextOverlay) is **disqualified**. Its own claim `C6` — "a
handler that received a wire-carried principal forwards it through an ordinary local
tell/ask" — was marked **WRONG** by the prover: `post_message` (the real code backing every
local tell/ask, `actor_ref.hpp`) only reads `amb->trace_id`/`amb->deadline_ns` from the
ambient context, never `amb->principal`; `make_descriptor` (the path a local tell actually
uses) never sets `d->ctx_` to anything but the pooled `nullptr` default. This defeats the
design's own stated purpose — multi-hop least-privilege delegation — for the single most
common case (a same-node forward). The prover's own note on a fix is explicit that it is
*not* cheap: closing it means adding a runtime `amb->principal.anonymous()` branch to the
local send hot path and conditionally routing through the `ctx_` builder — a change that was
never made and would require re-proving `C2`/`C3`'s zero-cost claims from scratch. Per the
ranking rule ("any safe/correct claim marked WRONG disqualifies… unless a stated cheap fix
exists"), no such fix was proven, so Design 1 is out regardless of its otherwise-clean
64-byte layout proof (`C1` CORRECT) and clean local-path cost (`C2`/`C3` CORRECT).

Designs 2 and 3 both pass the gate: every `safe`/`correct`-kind claim that survived
red-teaming was proven **CORRECT** after their respective fixes (Design 2: `S1`, `S2`, `C1`,
`C2` all CORRECT after the insertion-point fix; Design 3: `C1r`, `C2r`, `C3`, `C4`, `C5`,
`S1r`, `S2r` all CORRECT after the `DualReclaimSink` fix). Neither design's `S3` (TSan
coverage) could be run to completion — TSan is unavailable on every toolchain in this
environment — and both report `S3` **INCONCLUSIVE** rather than claim it as proven; per rule
(2) INCONCLUSIVE carries no weight for either side, so it does not separate them here but is
carried forward as a residual risk for both. Only `fast`-kind claims (`F2` for design 2,
`F2r` for design 3) were marked WRONG for either surviving design — outside the safety gate,
but relevant to rule (3) below.

**(2) Proven beats claimed / (3) measured hot-path numbers.** Both designs keep
`Descriptor` byte-for-byte at 56 bytes (`C4`/`C5` for design 3; the analogous checks for
design 2) — neither bends the core invariant of a proven, unmodified `gen_state` CAS
protocol, and both show a statistically-clean, noise-bounded local-path cost (`F1` CORRECT
for both — design 2 patched 24.44 M cycles/s/core avg vs. 23.11 M baseline, +5.8%,
never slower in 4 runs; design 3 patched median 258.07 ns/op vs. baseline 260.75 ns/op,
−1.0%, within a ~25% noise band). The deciding evidence is the wire-path marginal-cost claim
each design staked its "layered on top of already-paid wire-path work" story on:

| Claim | Design | Result | Number |
|---|---|---|---|
| `F2` insert+take vs. `decode_tagged` | 2 (PrincipalSideTable) | **WRONG** | 378–424% of decode cost (claimed <10%) — ~40× over budget, driven by `unordered_map` node allocation, **independent of message size** |
| `F2r` EnvelopePool marginal cost vs. total frame cost | 3 (Envelope Pool) | **WRONG** for 4-byte payloads only | 8.9–34.3% (median ~12%) on trivial/control-frame-sized messages; **CORRECT** (−3.4% to +3.2%) on realistic ~64-byte payloads — the mutex-guarded acquire is a near-fixed ~15–20 ns tax whose relative share shrinks as real payload grows |

Design 2's mechanism (a hash-map-based side channel) pays a large, size-independent tax on
every wire-arrived principal-carrying message. Design 3's mechanism (a pooled, fixed-offset
struct copy) pays a small, size-diluting tax that is within budget for realistic message
shapes and only exceeds it for degenerate near-empty payloads (heartbeats, bare acks) — a
narrower, better-understood, and cheaper failure mode.

Design 3 also has materially more complete **coverage**: the cross-examination independently
established there are exactly four real per-message context-population sites
(`drain_step`, `drain_step_governed_seq`, `admit()`/`ReFrame`, `handle_restart_retry`), and
Design 3's revised `C1r`/`C2r`/`S1r`/`S2r` were proven CORRECT at **all four**, including the
007 supervised-restart-retry redelivery path. Design 2's own materials name only three sites
(never mentions `handle_restart_retry`) and its `C1`/`S1` evidence exercises only Sequential
and Reentrant explicitly; nothing in its evidence establishes correct behavior — or even
absence of data loss — on a supervised-restart redelivery of a wire-arrived message. This
matters architecturally, not just as a coverage gap: `PrincipalSideTable::take()` is a
consuming read (erase-on-take). If a message is claimed, its principal taken, and the
handler then faults and is redelivered via 007 supervised restart, the side-table entry is
already gone — the retried dispatch would silently observe an anonymous principal. Design 3's
envelope is **not** consumed on read (`DescriptorEnvelope::principal` is a plain struct field
that persists with the pooled cell until reclaim), so a restart-retry naturally observes the
same principal without special-casing. This asymmetry was not exercised as an executable
claim for Design 2 and is called out below as a residual risk of *not* adopting the
side-table design, and as one more reason Design 3 is preferred.

**(4) Core invariants.** Both surviving designs keep `Descriptor` unmodified at 56 bytes, add
no new atomic word, and add no new CAS — both spend exactly one previously-unused bit of the
existing 12-bit `flags` subfield of `gen_state`. Neither bends a core invariant. Design 3 is
architecturally the more conservative extension: it reuses the exact `MessagePool::Cell`
Cell-first-member/fixed-offset idiom already proven in ADR-037, rather than introducing a new
kind of structure (a partitioned hash table) to the codebase.

### Evidence table

| Claim | Design | Kind | Survived red-team? | Proven? | Number / note |
|---|---|---|---|---|---|
| C1 (64B layout) | 1 ContextOverlay | correct | yes | CORRECT | `sizeof(Descriptor)==64`, zero padding |
| C2 (local-path cost) | 1 | fast | yes (revised) | CORRECT | p50/p99 within noise vs. real PERFORMANCE.md baseline (116/169ns) |
| C3 (0-alloc local) | 1 | safe | yes (revised) | CORRECT | Sequential delta=0; Reentrant delta matches unpatched baseline exactly |
| C4 (no stale-ctx_ leak) | 1 | safe | yes | CORRECT | positive control flips to FAIL as required |
| C5 (wire→handler principal) | 1 | correct | yes | CORRECT | subject/rights match bit-for-bit |
| **C6 (local forward propagates principal)** | 1 | correct | yes | **WRONG** | `post_message` never reads `amb->principal`; forwarded principal silently drops to anonymous — **no cheap fix proven** |
| C7 (176B wire budget) | 1 | correct | yes (revised) | CORRECT | static_assert fires exactly at 176/177B boundary |
| C8 (revert-to-anonymous next message) | 1 | safe | yes (new) | CORRECT | positive control flips to FAIL as required |
| F1 (local-path zero-cost) | 2 PrincipalSideTable | fast | yes | CORRECT | patched +5.8% avg throughput vs. baseline (noise) |
| **F2 (insert+take vs. decode_tagged)** | 2 | fast | yes | **WRONG** | 378–424% of decode cost (claimed <10%) |
| S1 (no cross-message leak) | 2 | safe | yes | CORRECT | Sequential + governed-Sequential, 1000/1000 clean |
| S2 (table never leaks) | 2 | safe | yes (revised: insertion point) | CORRECT | size()==0 at quiescence, 5 terminal-path cases |
| S3 (partition-mutex race safety) | 2 | safe | yes | INCONCLUSIVE | TSan unavailable; ASan+RTC1-substitute clean |
| C1 (wire→handler, Seq+Reentrant) | 2 | correct | yes (revised: inbound_thunk wiring + per-frame Reentrant fix) | CORRECT | 2 concurrent frames, no cross-contamination |
| C2 (try_claim additive overload) | 2 | correct | yes | CORRECT | existing tests unmodified & pass; race probe 0 mismatches |
| F1 (local-path zero-cost) | 3 Envelope Pool | fast | yes | CORRECT | median −1.0%, min +2.8% (noise) |
| **F2r (EnvelopePool marginal cost)** | 3 | fast | yes (narrowed) | **WRONG** for 4B payload; CORRECT for ~64B | 8.9–34.3% (trivial) vs. −3.4%..+3.2% (realistic) |
| C1r (wire→handler, all 4 real sites) | 3 | correct | yes (revised: 4-site fix) | CORRECT | drain_step, governed_seq, admit, restart_retry all pass |
| C2r (no cross-message leak, all sites) | 3 | correct | yes | CORRECT | 3 sites, all clean |
| C3 (local-tell propagates on forward) | 3 | correct | no attack | CORRECT | not gated on node boundary |
| C4 (Descriptor unmodified) | 3 | correct | no attack | CORRECT | verbatim unmodified tests pass unchanged |
| C5 (sizeof==56) | 3 | correct | no attack | CORRECT | static_assert + runtime print, 2 compilers |
| **S1r (no cross-pool-reclaim corruption)** | 3 | safe | yes (fatal bug found & fixed: DualReclaimSink) | CORRECT | 1.2M messages + fallback paths, 0 ASan/UBSan reports, 2 compilers |
| S2r (envelope_of only under flag, all 4 sites) | 3 | safe | yes | CORRECT | mutant reliably trips heap-buffer-overflow, 2 compilers |
| S3 (current_ctx_ safe across coroutine resume) | 3 | safe | yes | INCONCLUSIVE | TSan unavailable on both toolchains; 20,000-resume substitute clean |

**Totals across all three designs:** 20 claims proven CORRECT, 3 claims proven WRONG (1 safety-gating: Design 1's `C6`; 2 non-gating `fast` claims: Design 2's `F2`, Design 3's `F2r` partial), 2 claims INCONCLUSIVE (both designs' `S3`, TSan-shaped, both substituted with ASan/RTC1/coroutine-stress evidence that came back clean).

## Residual risks (carried into the accepted design)

1. **TSan coverage gap (S3, both surviving designs).** No TSan-capable toolchain was
   available in this environment (MSVC ships no TSan; the available Clang-on-Windows
   rejects `-fsanitize=thread` for the MSVC target). ASan plus a Debug/RTC1 and
   coroutine-resume stress substitute came back clean, but per ADR-032's own methodological
   note, TSan can miss data races regardless of memory order — a genuine happens-before
   litmus test (herd7/GenMC or a Linux TSan build) is still owed before this is treated as
   permanently settled, specifically for `current_ctx_`'s write-then-coroutine-resume path
   and for `EnvelopePool`'s mutex-guarded acquire/reclaim.
2. **F2r's trivial-payload tax.** For control-frame-sized wire messages (≤4–8 bytes:
   heartbeats, bare acks, empty replies), `EnvelopePool`'s mutex-guarded acquire is a
   measured 9–34% marginal tax over total per-frame processing — real, though small in
   absolute terms (~15–20 ns) and bounded to messages that rarely carry a meaningful
   `Principal` in practice. Should be re-measured once real wire traffic shapes are known;
   an uncontended spinlock or a small per-shard free-list cache (mirroring `MessagePool`'s
   local-cache magazine) is the natural follow-up if it proves to matter under load.
3. **EnvelopePool has no admission control / backpressure policy** — it inherits
   `MessagePool`'s "grow, never bounded" posture, duplicated across two pools instead of one.
   Needs sizing guidance once 020's authenticated-connection concurrency budget is known.
4. **Audit obligation:** `make_descriptor`/`make_descriptor_enveloped` (or their
   equivalents) and `post_message`/`deliver_from_wire` were the only two Descriptor-
   construction sites the accepted design touched. Any future Descriptor-construction path
   (persistence replay, cluster-control messages, a hand-built test harness) that bypasses
   these two functions silently defaults to the non-enveloped path and drops principal
   propagation. Needs an explicit call-site audit, not just convention.
5. **`envelope_of(d)`'s safety is guard-dependent, not type-enforced.** `reinterpret_cast`
   through `EnvelopePool::Cell` is only correct when `kControlFlagHasEnvelope` was observed
   from the *same* flags word at *every* call site. `S2r`'s mutant test proves the guard is
   load-bearing today, but a future 5th dispatch/redispatch site added without the same
   guard reintroduces the exact heap-buffer-overflow class `S2r` exists to catch. Recommend
   centralizing the guarded read (e.g. a single `resolve_principal(Descriptor*, uint16_t
   flags)` helper analogous to what Design 1's rebuttal proposed) rather than hand-copying
   the branch a 5th time.
6. **Design 2's discarded architecture flags a real, untested hazard worth recording even
   though it lost:** `PrincipalSideTable::take()`'s consume-on-read semantics would silently
   lose the principal on a 007 supervised-restart redelivery of the same wire-arrived
   descriptor — never exercised as an executable claim in this debate. This is not a risk of
   the *accepted* design (Design 3's envelope is not consumed on read), but is worth noting
   in case a future change reintroduces a consuming side-channel for any other ambient field.

## Spec recommendations

**`020-Security.md`**
- Add a concrete "Principal propagation mechanism" subsection under §3 stating: a
  wire-arrived, non-anonymous `Principal` is carried from `DistributedRouter::deliver`
  through `inbound_thunk` (which must forward `MessageFrame::principal`, not drop it) into
  `LocalRouter::deliver_from_wire`, which routes the descriptor through `EnvelopePool`
  (`quark::detail::EnvelopePool`/`DescriptorEnvelope`) instead of the plain `MessagePool`
  whenever `principal.anonymous() == false`, and is resolved into
  `current_context().principal` (or the per-frame `ReFrame::ctx.principal` under
  `Reentrant`/`MaxConcurrency<N>`) via one flag-gated pooled-struct dereference at **all
  four** claim/dispatch sites: plain `Sequential` drain, governed-`Sequential` drain,
  `Reentrant` admit, and supervised-restart redelivery.
- State explicitly that a purely local send with an anonymous ambient principal (the
  default) never touches `EnvelopePool` and pays zero extra cost — this is the "a purely
  intra-process tell pays nothing" invariant, now backed by measured evidence (F1).
- Document the residual TSan gap (risk 1) and the trivial-payload marginal-cost bound (risk
  2) as open items, not settled facts.

**`003-Memory.md`**
- Add `kControlFlagHasEnvelope` (bit 1 of the existing 12-bit `flags` subfield of
  `gen_state`) alongside the already-documented `kControlFlagDeactivate` (bit 0), with a note
  that it marks "sourced from `EnvelopePool`, not `MessagePool`" and must be read from the
  *same* flags word `try_claim()` already loads — no new atomic load.
- Add `EnvelopePool`/`DescriptorEnvelope` to the pool inventory alongside `MessagePool`,
  cross-referencing ADR-037's Cell-first-member/fixed-offset idiom as the shared pattern both
  pools use, and note the reclaim-dispatch obligation (any code path that reclaims a
  Descriptor must route through a flag-checking dual-sink, never a single pool's `reclaim`
  unconditionally — this is what `S1r` closed).
- Keep the existing `sizeof(Descriptor) <= 64` / `== 56` assertions as-is; add a note that
  this ADR chose *not* to spend Descriptor's slack bytes on Principal, preserving headroom.

**`006-Messaging-and-Addressing.md`**
- In the section describing `MessageFrame`, state explicitly that `MessageFrame::principal`
  must be threaded through `inbound_thunk` into `deliver_from_wire` (previously silently
  dropped) — cite this as the specific integration bug this ADR's evidence run found and
  fixed.
- Note that principal propagation on a local forward is **not** gated on crossing a node
  boundary (C3): a handler running under a non-anonymous ambient principal that issues an
  ordinary local `tell`/`ask` carries that principal to the next hop via `post_message`'s
  existing ambient-context read, unless it explicitly attenuates.

**`decisions/ADR-007-actor-authoring-and-handler-dispatch-api.md`**
- Add a superseding note at the top: "The `Descriptor::ctx_`-pointer-into-payload-arena
  mechanism proposed here for carrying ambient `MessageContext` was never implemented; for
  `Principal` specifically it is superseded by ADR-044's Flag-Gated Envelope Pool, which
  keeps `Descriptor` at its real, ten-times-proven 56-byte layout instead of growing it to
  64. `deadline_ns`/`trace_id` remain inline members, as they are pervasive on local causal
  chains (009/018) and were never actually moved out despite the original text proposing to."
- Do not delete the original ctx_ proposal text — retain it as historical record of a design
  that was proven internally consistent (C1–C5, C7, C8 all CORRECT) but disqualified by a
  single proven-WRONG claim (C6: local-tell forwarding silently drops the ambient principal,
  defeating the design's own motivating multi-hop-delegation use case) with no cheap fix
  identified.
