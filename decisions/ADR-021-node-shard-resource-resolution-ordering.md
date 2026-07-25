# ADR-021 — Node/Shard resource resolution ordering vs. actor activation on a cold shard

## Status

Accepted

## Question

When a cold shard (one that has performed no activation yet) receives its first
actor activation that needs a Node-scoped or Shard-scoped resource (004), what is
the resolution ordering and mechanism?

- **Eager-Everywhere** — resolve every Node/Shard resource, for every shard,
  synchronously inside `Engine`'s constructor (008's metadata-compilation cold
  phase), strictly before `Engine::start()` spawns a single worker thread.
- **Lazy-on-first-touch** — resolve a resource on whichever shard's first
  activation actually needs it, with a per-resource-type CAS slot (Node scope)
  and a plain flag piggybacked on the pre-existing `drain_owner` Dekker (Shard
  scope).
- **Background-Prewarm** — resolve Node resources eagerly/synchronously at
  `build()` (few of them, deduplicated), but kick off one background-prewarm job
  per shard for Shard resources asynchronously; `build()` returns without
  waiting; each shard's worker gates on a per-shard ready-flag at activation
  construction.

The design must specify how a Node-scoped resource (shared by potentially many
shards on one node) is resolved exactly once when multiple shards race to be
first, without violating "no dynamic resolution while draining" or introducing
a lock/allocation on the steady-state message path.

## One-line design summaries

| Design | Mechanism |
|---|---|
| Eager-Everywhere | Race dissolved by ordering — every Node/Shard resource for every shard is constructed on one thread, entirely before any worker exists; steady-state reads are a pointer, and the cold `wire()` path has zero atomics either. |
| Lazy-on-first-touch | Race resolved by synchronization — a per-resource-type CAS slot (Empty→Resolving→Ready/Failed) arbitrates concurrent first-touch across shards for Node scope; Shard scope rides the pre-existing `drain_owner` Dekker with a plain, non-atomic flag. |
| Background-Prewarm | Hybrid — Node resources resolved eagerly/synchronously at `build()` (same as Eager, for the small Node set); Shard resources backgrounded to a startup-resolver pool so `build()` returns fast, gated by a per-shard ready-check at activation construction. |

## Decision

**Winner: Eager-Everywhere.**

All three designs, after cross-examination and fixes, had every claim proven
`CORRECT` by executed C++ (GCC 14 / Clang 20, TSan + ASan/UBSan, both
compilers). The deciding factors are not the hot path — which is byte-identical
across all three designs, because none of them touch `Cached<T>::get()` — but
(a) the severity/class of defects red-teaming actually found before the fix,
and (b) how much *new* runtime machinery each design requires to exist at all.

1. **Safety gate.** All three designs had a real defect found and fixed:
   - Eager: two mechanical lifetime-ordering bugs (member-declaration order
     causing a destruction-order use-after-free; a missing `teardown_` list on
     `ShardResourceScope` causing silently-skipped destructors and a leak).
     Both are cold-path, non-concurrency bugs, fixed with a field reorder and a
     `teardown_` vector, and both fixes are proven by dedicated
     ASan/LeakSanitizer regression tests (C7, C8) that also confirm the
     *un-fixed* version fails the same test (sensitivity check).
   - Lazy: a genuine reentrancy deadlock (a Node-resource factory that
     transitively re-enters its own in-flight slot hangs forever) — a real
     concurrency defect, fixed with a thread-local "currently resolving" guard
     stack, TSan-proven.
   - Background-Prewarm: a **fatal, observed safety violation** — the
     `thread_local` warm-flag downgrade let a worker that had warmed on one
     shard (via work-stealing, 002) skip the readiness gate entirely on a
     different, still-cold shard, constructing an activation that reports
     success while its `Cached<T>` slots are still `nullptr` — directly
     violating "no dynamic resolution while draining" and "handler never runs
     with a null/degraded resource." This was reproduced deterministically
     before the fix. The fix (delete the optimization, always re-check the
     shard's own gate) is cheap and was proven correct (C3r), so per the
     ranking rule this does not outright disqualify the design — but it is the
     only one of the three where the *original* mechanism, as specified, was
     unsafe in a way that would have shipped a real bug had it not been
     red-teamed.
2. **Proven-claims count.** Eager: 8/8 claims proven CORRECT on both compilers,
   full TSan+ASan/UBSan coverage, no INCONCLUSIVE legs. Lazy: 8/8 proven
   CORRECT, also full coverage, no INCONCLUSIVE legs — a very close second.
   Background-Prewarm: 8/8 proven CORRECT but with one claim (C7, the
   release/acquire-necessity check) carrying an INCONCLUSIVE leg on
   Clang+TSan for the deliberately-broken `relaxed` falsifier (toolchain hang,
   not a defect in the actual design) and no ARM64 hardware available — per the
   ranking rule, INCONCLUSIVE carries no weight, so Background-Prewarm's
   evidentiary base is thinnest of the three on that one claim.
3. **Hot-path numbers.** Tied. Lazy's design produced the single most decisive
   piece of hot-path evidence in the whole debate — `objdump` shows
   `Cached<T>::get()`/`operator->()` for Activation-, Node-, and Shard-scoped
   instantiations are **byte-identical machine code** (`mov (%rdi),%rax; ret`)
   on both GCC and Clang, because the type carries no lifetime tag at all.
   Eager relies on the same unmodified `resource.hpp` machinery, so the same
   conclusion holds by construction even though it wasn't re-disassembled
   per-lifetime in that design's own test. Background-Prewarm is the only
   design that adds a real, always-on cost — a per-activation-construction gate
   load (0.897 ns/call vs. 0.409 ns baseline, Δ≈0.49 ns) — which is bounded and
   amortized once per actor's whole lifetime (004), not per message, so it does
   not violate the hot-path rule, but it is strictly non-zero where the other
   two are exactly zero.
4. **Core-invariant fidelity.** Eager is the only design that makes the "many
   shards race for a Node resource" scenario **structurally impossible** rather
   than merely fast and safe: at the moment any worker thread first exists,
   `node_scope_`/`shard_scopes_` are already fully populated and immutable, so
   there is no CAS, no `atomic::wait`, no gate, and no reentrancy hazard class
   to reason about on the cold path at all — for the resource-type set known at
   first `build()`. Both competitors introduce genuine concurrent state
   machines (CAS slots, park/wake, ready-gates) to *arbitrate* a race that
   Eager avoids having to arbitrate. Occam's razor plus the fact that the two
   competitors' actual defects were concurrency-class bugs (reentrancy hang,
   null-resource-under-success) while Eager's were mechanical lifetime bugs,
   favors the design with the smaller concurrency surface.
5. **Dependency on unbuilt machinery.** Lazy's own risk list and
   Background-Prewarm's construction both depend on a lazy-activation-on-
   first-message dispatch hook that **does not exist yet in the codebase**
   (`engine.hpp`'s `resolve()` comment flags it as an open seam to 010/002).
   Eager needs no such hook: its mechanism is fully expressed by the existing
   `Engine` constructor and the existing `std::thread`-construction
   synchronizes-with edge already used by `Engine::start()`. This means Eager
   is buildable and provable against the actual, current codebase end-to-end,
   while the other two designs' race-arbitration code is — by their own
   authors' admission — presently dead code with no caller until that seam is
   built, which is exactly the kind of code path that is easy to under-test in
   practice (a risk Background-Prewarm's own risk list calls out explicitly).

The price Eager pays — and the reason this is a real trade-off, not a free
lunch — is `build()` wall-clock scaling linearly in `shard_count × resource_count`
(proven, R² > 0.9998 on both compilers) including for shards that never host an
actor. This is a named, cold-path-only cost, not a hot-path or steady-state
cost, and 004 already accepts amortized cold-path cost as the price of a
zero-cost hot path elsewhere (`PerMessage<T>` allocation).

## Evidence table

| Claim | Design | Survived red-team? | Proven? | Number / result |
|---|---|---|---|---|
| Node factory invoked exactly once, single-threaded ctor, zero atomics | Eager C2 | Yes | CORRECT | 0 races, 100 TSan runs (50×2 compilers), plain counter == 1 |
| No extra fence needed beyond `std::thread`'s synchronizes-with edge | Eager C3 | Yes | CORRECT | 0 TSan reports, 100 runs, worst-case zero-sleep race window |
| First activation never sees unresolved `Cached<T>` | Eager C1 | Yes (narrowed to spawn()-before-start() scope) | CORRECT | 0 asserts fired, 64 shards × 3 Node + 3 Shard resources, both compilers |
| `wire()` has zero atomics; measurable but immaterial cost vs. a lazy-CAS comparator | Eager C4 | Yes (framing walked back) | CORRECT | objdump: 0 lock/cmpxchg/xadd both compilers; Δ 30.9–36.4 ns = 0.31–0.36% of the 10 µs cold-activation budget |
| `build()` ctor time scales linearly in shard_count | Eager C5 | Yes | CORRECT | R² = 0.99987 (gcc), 0.999992 (clang) |
| Post-build `add_actor_type<T>()` rejects unregistered resource types, no live resolution | Eager C6 | Yes | CORRECT | unexpected returned, table pointer + arena high-water-mark unchanged |
| Scope teardown must run after actor teardown (destruction order) | Eager C7 (new) | Fixed defect | CORRECT | ASan: bad-order build reproduces UAF; fixed-order build clean, both compilers |
| Shard-scope resources need an explicit teardown list (pmr arena skips dtors) | Eager C8 (new) | Fixed defect | CORRECT | ASan+LSan: before-fix 0/32 dtors run + 512B leak; after-fix 32/32 dtors run, 0 leaks |
| Node CAS-resolve-once: exactly-once, identical pointer for all racers | Lazy C1 | Yes | CORRECT | 0 TSan reports, 64 racers, factory_calls == 1 |
| No hang; reentrant resolve fails fast instead of deadlocking | Lazy C2 | Fixed defect (reentrancy hang found) | CORRECT | Guard stack: cyclic resolve fails in 0 ms with `errc::validation`, 0 hangs |
| Release/acquire publishes a fully-constructed object (1000-field canary) | Lazy C3 | Yes | CORRECT | 0 torn reads across ~236,000 racer-observations |
| Cached<T>::get() identical for Activation/Node/Shard scope | Lazy C4 | Yes | CORRECT | Byte-identical disassembly, both compilers |
| Shard-scope needs no atomic — rides the existing `drain_owner` Dekker | Lazy C5 | Yes | CORRECT | 0 TSan reports, ~90–94k genuine cross-thread handoffs, 100k rounds |
| Node factory failure identical + terminal across racers | Lazy C6 | Yes | CORRECT | 32/32 identical errc, factory invoked once only |
| Lazy resolves only touched shards, not O(shard_count) | Lazy C7 | Yes | CORRECT | lazy 5.4 ms vs eager 71.0 ms at shard_count=64 (~13× measured, ~16× predicted) |
| Slow Node factory delays only concurrent racers of that resource, not the whole node | Lazy C8 | Yes (framing corrected) | CORRECT | ~205–207 ms spike confined to actual racers; untouched shards ~11–13 ms baseline |
| Per-shard gate check costs ~1 predicted load, 0 allocations | Prewarm C3r | Fixed **fatal** defect (thread_local skipped gate on stolen shard) | CORRECT | Repro: bad design returns success w/ nullptr in 0 µs; fixed design blocks correctly ~150 ms; Δ 0.49 ns/call after fix |
| Stalled shard never delays other shards (workers-are-lanes) | Prewarm C4 | Yes | CORRECT | p99 ratio 0.78×–1.69× (noise) vs. a 2000 ms stall on one shard |
| Backgrounded prewarm failure surfaces via the same 007 boundary | Prewarm C5r | Yes (narrowed to terminating failures) | CORRECT | ask bounded unexpected in 150 ms; 1 dead-letter for tell |
| Build-time Node factory failure fails `build()` itself (not silent poisoning) | Prewarm C8 (new) | Fixed gap (found by red-team) | CORRECT | build() returns unexpected(ValidationReport); 0 shard-prewarm jobs run |
| `build()` return time decoupled from per-shard prewarm cost | Prewarm C6 | Yes (wording corrected from "O(1)" to "negligible coefficient") | CORRECT | flat 0.11–0.81 ms across 0/1/100 ms per-shard work; submission slope 1158.6 ns/shard |
| release/acquire is necessary, not just sufficient | Prewarm C7 | Yes | CORRECT (1 leg INCONCLUSIVE) | relaxed variant: 6 TSan races on gcc; clang-TSan hung on the falsifier (toolchain quirk, not the design); ARM64 untested (no hardware) |

## Spec recommendations

**`004-Resources.md`**
1. Replace the open question at the end of the file (currently: *"Node/Shard
   resource resolution ordering vs. actor activation on a cold shard."*) with a
   resolved note in the same style as the factory-error-handling entry above
   it:
   > *(Node/Shard resolution ordering: resolved — Node- and Shard-scoped
   > resources are resolved eagerly, for every configured shard, synchronously
   > inside the Engine's construction/`build()` cold phase (008), strictly
   > before any worker thread is created. A Node-scoped resource shared by many
   > shards is therefore constructed exactly once by a single thread with no
   > CAS, lock, or `std::call_once` — the multi-shard race is dissolved by
   > ordering, not resolved by synchronization. See ADR-021.)*
2. Add a short subsection under "Lifetimes" (after the "Longer-lived scopes are
   strictly cheaper" paragraph) making the eager-resolution contract explicit:
   *"Node and Shard resources are resolved once, for every shard, during
   Engine construction — before any worker thread exists and before any
   activation occurs. There is no 'cold shard' case to reason about: by the
   time a worker thread is spawned, every shard's Node/Shard resource table is
   already complete and immutable. `Cached<T>::wire()` for these lifetimes is a
   plain pointer-copy loop with zero atomics, identical in cost/shape to the
   Activation-scope wiring already documented."*
3. Add an explicit lifetime-ordering / teardown invariant (new bullet under
   "Rules"): *"The Engine's Node/Shard resource storage must be declared (and
   therefore destroyed) after every container of owned actors/activations —
   actor destructors that touch a `Cached<>` resource (e.g. a pool-checkout
   RAII guard) must never run after the resource they reference has already
   been torn down. Any resource storage backed by a bump/arena allocator
   (`std::pmr::monotonic_buffer_resource` or equivalent) must carry an explicit
   destructor-thunk list — bulk-reclaiming arena memory does not, by itself,
   invoke placement-constructed destructors."*
4. Name the accepted operational trade-off explicitly: *"Engine construction
   time scales linearly with `shard_count × resource_count`, including for
   shards that will host no actor. For very large shard counts with expensive
   Node/Shard factories (013/026), this cost may be parallelized across
   build-time-only helper threads joined before `start()`; such parallel-build
   CAS/state-machine helpers must be a distinct, clearly-marked build-only type
   never reachable from a worker thread."*

**`008-Metadata-and-Startup.md`**
1. In "Metadata compilation," add a line stating that Node/Shard resource
   resolution is now part of this cold phase, not deferred to Runtime: *"Node-
   and Shard-scoped resource resolution (004) happens here, for every shard
   configured, before Runtime begins — see ADR-021. `ActorMetadata.ResourcePlan`
   references the resolved Node/Shard resource slots by dense index; no lookup
   occurs at activation time for these two lifetimes."*
2. In "Dynamic registration (guarded) — ADR-008," add the closed-world
   constraint proven by Eager's C6: *"A guarded `add_actor_type<T>()` whose
   `ResourcePlan` references a Node- or Shard-scoped resource type that was
   never registered at the original `build()` MUST fail incremental Validation
   (return `std::unexpected`, publish nothing, leave the resource tables
   pointer-identical). It must never attempt live resolution against running
   workers — the zero-atomics guarantee for Node/Shard resolution (ADR-021)
   is a closed-world guarantee over the resource-type set known at first
   `build()`."*
3. Update the "Open questions" line (`Node/Shard resource resolution ordering
   vs. actor activation on a cold shard.`) — remove it or mark resolved,
   pointing at ADR-021, mirroring the existing resolved bullet for hot-reload
   above it.

## Residual risks

- **Startup latency at extreme shard counts.** `build()` time is proven linear
  in `shard_count × resource_count`; for very large clusters (026) with
  heavyweight Node/Shard factories (real DB dials, TLS handshakes), this could
  become operationally slow. Mitigation exists only as an optional,
  not-yet-built parallel-build accelerant — it should be prototyped and
  measured, not merely specified, before a team hits this in practice.
- **Wasted resources for shards with no actor.** Explicitly conceded and
  proportional to configured `shard_count`, independent of actual actor
  population — relevant for elastic/test deployments provisioning peak
  `shard_count` while mostly idle.
- **Lifetime-ordering fragility.** The two Eager defects found (destruction
  order, missing teardown list) were purely mechanical and are now covered by
  regression tests (C7, C8), but they demonstrate that this class of bug is
  easy to reintroduce silently on future refactors of `Engine`'s member list.
  A comment plus the regression tests are the only guard; there is no
  compile-time enforcement (pointer-to-member order isn't a portable constant
  expression).
- **`C1`'s scope is narrower than the debate's original wording.** Eager's
  proof covers the currently-implemented `spawn<A>()` path (pre-`start()`,
  single-threaded, unsynchronized `owned_actors_`/`owned_activations_`
  push_back), not a true "lazy activation triggered by the first message
  mid-drain" path — that runtime mechanism does not exist in the codebase yet
  (`engine.hpp`'s `resolve()` comment flags it as an open seam to 010/002).
  The architectural argument that Eager's guarantee extends unconditionally to
  that future path (nothing needs to happen at activation time for Node/Shard
  scope, regardless of when activation occurs) is sound but **argued, not
  executed** — re-run Eager's C1 experiment (N external threads simultaneously
  sending the first-ever message to N never-touched ActorIds on the same node,
  under TSan) the moment that lazy-activation seam lands, before relying on
  this conclusion for that code path.
- **Both competing designs' race-arbitration code is presently dead code.**
  Lazy's CAS/park machinery and Prewarm's ready-gate are only exercised via
  ADR-008's guarded `add_actor_type<T>()` or a not-yet-built lazy-activation
  hook — a path with no ordinary integration-test coverage today. If either
  mechanism is ever adopted later (e.g. for a future feature that needs true
  lazy per-shard resolution), it must ship with dedicated stress tests from
  day one, since "no caller exercises it yet" was exactly how
  Background-Prewarm's fatal `thread_local` bug went unnoticed until red-teaming.
- **Tie-breaking experiment, if this decision is ever revisited:** implement
  the lazy-activation-on-first-message dispatch hook (the 002/010 seam both
  competing designs depend on), then re-run Eager's C1 test in that mode
  (N external producers racing N never-touched ActorIds on the same node,
  first message ever, under TSan) — if and only if that generalization ever
  fails would Lazy-on-first-touch (the strongest runner-up, with the cleanest
  proven claims and the best skewed-placement cold-start numbers, ~13× faster
  than Eager at shard_count=64 with 4/64 shards touched) become the
  fallback candidate.
