# ADR-034: On-Demand Actor Passivation (`ActorRef<A>::passivate()`)

## Status

Accepted

## Question

Quark actors are spawned lazily and, once active, stay active until the automatic idle-timeout
wheel (ADR-028 Phase 1/2) eventually evicts them — there is no caller-facing way to say "tear this
specific actor down now." This bites an actor kept busy by a recurring timer/reminder (027): it may
receive messages often enough that its idle-timeout window never accumulates, even though it is
genuinely idle between any two individual messages (Sequential, one message at a time). Its state
effectively "lives as long as the actor" with no way to force a checkpoint or run cleanup.

Design a caller-facing on-demand teardown API that (1) reuses the exact same deactivation mechanism
as the automatic path — no divergent code, "lifecycle called as it was" — and (2) stays **soft**:
advisory, never a hard mid-handler interrupt, never breaking the single-executor/handler-runs-to-
completion invariant (001).

This was **not** run through the mailbox lineage's full architect→red-team→prover→judge cycle (that
process is reserved for the mailbox MPSC hot-path rounds, ADR-001 etc.) — it is a scoped, additive
capability built against a reviewed implementation plan and verified with the project's own real
Engine/Activation test suite (011/012 conventions), which is the appropriate weight for a capability
that assembles existing, already-proven mechanisms rather than inventing new hot-path concurrency.

## Design adopted

Three pieces, funneled through **one** call site (`Activation::close_out_retire()`) so automatic and
on-demand retirement are byte-for-byte identical from the actor's point of view:

1. **`ActorRef<A>::passivate()`** — fire-and-forget (matches `tell`'s async nature), returns `bool`
   (`false` iff the id is unresolved; `true` = accepted/already-pending, not "has retired yet").
   Resolves through the SAME `resolve()` lookup `tell` uses (never the lazy-construction `activate()`
   hand-off), then posts the SAME `Deactivate` control descriptor (ADR-028 Phase 1) the automatic
   wheel posts. **Sequential-only, enforced by `static_assert`** — the reused Dekker close-out is
   proven only for exactly one in-flight drain, the same restriction `IdleTimeout<Ms>` already has.
2. **The interlock.** `deactivate_descriptor_` is a single, never-pool-allocated `Descriptor` per
   `Activation`; the Vyukov intrusive mailbox requires it never be enqueued twice while still linked
   from a prior enqueue. Introducing a second, independent trigger (on-demand, callable from any
   producer thread, vs. the wheel, shard-drain-owner-only) creates a genuine double-post race. Fixed
   with a small atomic `DeactivateToken{Idle,Posted}` CAS'd by both triggers before touching the
   descriptor, cleared the instant `drain_step()` claims it off the mailbox — the loser of a race is
   a harmless no-op (a passivation is already pending). `Engine::on_deactivate_fire`'s inline
   build+post sequence was refactored into a shared `Engine::post_deactivate()` both triggers call.
3. **`on_deactivate()` + the persistence flush**, both new, both fired from the top of
   `close_out_retire()` — strictly *before* `retire_to_dormant()`'s release-store, while the worker
   still unambiguously holds `Running` (after that store + the Dekker fence, a concurrent producer
   can hand a *different* worker ownership of this activation, so touching actor state after that
   point would be a genuine second-executor race). `on_deactivate()` is member-detected exactly like
   the existing `has_resource_wire<A>`/`has_persist_state<A>` idiom — opt-in, no virtual. The
   persistence flush reuses the existing, proven `save_snapshot`/`Store` seam (012), wired only for
   actors registered through `Engine::declare_lazy<A>(store,...)` (the only existing mechanism
   establishing a `{Store, FenceToken}` relationship at all) — best-effort, counted
   (`deactivate_flush_failures`, 009), never fatal, since there's nothing left to retry against once
   the mailbox is genuinely empty.

**Necessary prerequisite fix, in scope**: `declare_lazy<A>(store,...)`'s broker construction never
actually called `store.acquire_fence(id)` — recovery only ever read, never established ownership.
Closed as part of this change (`ActivationBroker::handle_wake` now acquires a real fence once, at
construction, reused by the flush) since a deactivation-time write needs a non-stale token to be
meaningfully checked against `Store::save_snapshot`'s own fencing gate.

## Evidence

Verified with the real Engine/Activation machinery (no hand-rolled harness), following this repo's
own test-naming conventions:

| Test | Proves |
|---|---|
| `activation_passivate_interlock_test` | The CAS interlock claims exactly once while pending; `clear_deactivate_token()` re-opens it |
| `engine_passivate_test` | `.passivate()` drains queued/in-flight work then reaches `Dormant`; a racing message still dispatches (abort-eviction preserved); an unresolved id returns `false`; two racing `.passivate()` calls from different threads converge on exactly one retirement; an already-`Dormant` actor round-trips cleanly |
| `activation_on_deactivate_hook_test` | `on_deactivate()` fires once via the automatic wheel, once again via an explicit `.passivate()` after reactivation — the same call site serves both triggers identically |
| `engine_passivate_persistence_test` | A `declare_lazy<A>(store,...)` actor's latest state is persisted under a real (non-default) fence; a failing store's flush error is counted but retirement still commits; a `spawn<A>()`'d (no-store) actor passivates cleanly with the flush inertly skipped |

All new tests pass; the full existing suite (182 tests) passes unmodified, including
`engine_idle_timeout_eviction_test` (the interlock is a no-op in the pre-existing single-trigger
scenario, confirming zero behavioral change to the automatic path).

## Decision

Adopted as designed. No competing design was evaluated — the automatic path's existing
`Deactivate`-control-descriptor mechanism was already structurally the right shape for an on-demand
trigger (a mailbox-posted control message degrades gracefully regardless of the target's current
busy/Parked/Dormant state by construction); the only genuinely new engineering was the interlock
guarding the now-shared descriptor against a second trigger.

## Residual risks

1. **ADR-028 Phase 4 (freeing the actor instance on `Dormant`) remains unimplemented** — this feature
   matches *today's* Dormant semantics (state flag only, actor object stays alive) for both the
   automatic and on-demand path alike; it does not attempt to close this pre-existing gap.
2. **No persistence support for eagerly-`spawn<A>()`'d actors** — a `Persistent<Snapshot>` actor
   spawned via `spawn<A>()` rather than `declare_lazy<A>(store,...)` gets no store regardless of
   policy, unchanged from before this feature; the flush is simply inactive for such an actor.
3. **`ThroughputFirst`-style Reentrant/`MaxConcurrency<N>` passivation is out of scope** — forbidden
   at compile time via `static_assert`, not silently degraded or unproven.
4. **The EventSourced/ADR-024 compaction-cadence machinery is untouched** — the persistence flush
   here is the simpler Snapshot-model write path only; `through_seq = store.last_seq(id)` is the only
   EventSourced-adjacent value touched, purely as a pass-through to the already-proven `Store`
   concept.

## Spec-update recommendations (applied)

- `011-Timers-and-Scheduled-Work.md` §"Idle-timeout deactivation" — new bullet documenting
  `passivate()`'s reuse of the same interlock/Dekker sequence.
- `012-Persistence.md` — new §"Deactivation-time flush (Snapshot model, ADR-028 Phase 8)",
  distinguished explicitly from the EventSourced compaction-cadence section above it.
- `006-Messaging-and-Addressing.md` §"Send verbs" — new `passivate` subsection next to `tell`/`ask`.
- `005-Developer-Model.md` — new §"Lifecycle hooks" documenting `on_deactivate()` (and, since they
  were previously undocumented anywhere, `wire()`/`snapshot_state()`/`restore_state()` alongside it).
