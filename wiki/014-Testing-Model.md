# 014 — Testing Model

An engine this concurrent is untestable by luck. Quark ships a **deterministic
simulation** runtime and a unit-level harness so that the same public API can run
either on real threads (production) or on a single thread with a virtual clock and
a seeded scheduler (tests) — reproducibly, byte-for-byte, from a seed.

## Deterministic simulation (`SimEngine`)

A drop-in engine that implements the **same public API** as the production engine
but:

- runs **all shards on one thread**, so there is no real concurrency to be
  flaky about;
- interleaves message delivery and activation ordering via a **seeded PRNG**, so a
  failing run is reproduced exactly from its seed;
- drives all time from a **virtual clock** (011) — timers and deadlines advance
  when the test advances time, never by wall-clock sleeping;
- exposes the schedule so a test can explore many interleavings of the same
  scenario.

```cpp
quark::SimEngine sim{seed};
auto order = sim.get<Order>(42);
order.tell(Ship{...});
sim.run_until_idle();              // deterministic drain
sim.advance(1s);                   // fire timers/deadlines deterministically
```

### Alternatives considered

- **Real threads + logging + retries**: cannot reliably reproduce a rare
  interleaving; "flaky test" becomes the norm.
- **Mocking the scheduler per test**: divergent from production behavior, so tests
  pass against a fiction.
- **Decision:** a real single-threaded deterministic executor sharing the
  production actor/dispatch code (008), differing only in the scheduler and clock.
  Same code paths, controllable nondeterminism. (Prior art: FoundationDB,
  `madsim`.)

### Chooser policy (ADR-025, proven)

`SimEngine` picks its next runnable actor through a compile-time `Chooser`
policy: `template <ChoicePolicy Chooser = RandomChoicePolicy> class SimEngine`, a
concept-constrained, zero-cost compile-time parameter — no vtable, consistent
with the RFC's "no virtual dispatch for policy" ground rule and the locked
CRTP-policy-types decision. `RandomChoicePolicy` is the unchanged default, byte-
identical to today's `rng_() % n`; `ScriptedChoicePolicy` drives bounded
exploration (below).

## Bounded exploration (DPOR) (ADR-025, proven)

`explore_bounded(N, branch_cap)` performs iterative-deepening DFS over pick
scripts up to `N` context switches — defined precisely as "branch points where
`|runnable_| > 1`," a conservative proxy for (not equivalent to) the stricter
"executing-actor-changes" definition used in CHESS-lineage literature.

Pruning uses **DPOR**: source-set/minimal-sibling backtracking over
`post()`-observed touched-receiver sets. Its **soundness restriction is a hard
rule, not a footnote**: handlers explored under bounded model checking must
communicate only via `tell`/`ask`; any shared-mutable-state side channel is an
explicit, documented soundness gap. Bugs requiring more than `N` switches are
**not guaranteed to be found** — this is the design's stated limit, not an
oversight.

## Fault injection

The simulator can deterministically inject the failure modes the RFC defines, so
supervision (007) and distribution (010) are actually exercised:

- **handler faults** — force a `handle` to throw on the Nth message;
- **message loss / reordering / delay** — within transport bounds (010);
- **node failures / partitions** — drop a simulated node, split membership;
- **store faults** — `StateStore` (012) errors and slow writes.

Each is seeded, so a discovered bug replays exactly.

## Unit harness (`TestKit`)

For testing one actor without standing up an engine:

```cpp
quark::TestKit<Order> kit;                  // isolated activation
auto reply = kit.ask<Confirmation>(Query{...});
kit.expect_reply(reply, Confirmation{...});
kit.expect_tells_to<Inventory>(Reserve{...});   // outbound messages captured
kit.expect_no_message();
kit.advance(500ms);                             // virtual time for timers
kit.assert_state([](const Order& o){ return o.shipped(); });
```

`TestKit` captures outbound `tell`/`ask`, exposes actor state for assertions,
drives virtual time, and injects a `MessageContext` (cancellation, deadline,
trace) so context-dependent handlers are testable.

## Invariant checking

The simulator can assert **core invariants** hold across every explored
interleaving, turning the RFC's invariants into runtime property checks in test
builds:

- single-executor: no actor is ever entered concurrently;
- FIFO: per-(sender,receiver) order is preserved;
- no lost message: every message reaches `Completed`, `Cancelled`, or dead-letter;
- placement stability: `ActorId → shard` is constant for a fixed membership.

Violations abort the run with the seed and the offending schedule for replay.

**Reproducer generalization (ADR-025, proven).** A violation's reproducer is
`(fault_seed, forced_picks)` — a superset of a bare seed — which replays exactly
through `ScriptedChoicePolicy` and is strictly stronger than "abort with the
seed" alone.

**LostMessage gap (closed, ADR-025).** `run_until_idle()` must, at **true
quiescence** (`runnable_` empty **and** all timer/armed-event queues empty — not
merely `runnable_` empty), scan every activation and throw
`SimInvariantViolation{LostMessage,...}` for any non-terminal
(`Completed`/`Cancelled`/dead-lettered) activation, so a permanently parked
coroutine can never be silently certified as delivered.

## Relationship to production code

There is **one** actor/handler/dispatch implementation (008). `SimEngine` and the
production engine differ only in the *scheduler* and *clock* implementations behind
a small internal interface, so tests exercise production dispatch, resource wiring,
and supervision — not a parallel mock universe.

**Scope warning (ADR-025, proven — S2).** Scheduler-owned oracles (shadow-FIFO
ledger, single-executor guard) verify the *scheduler's own bookkeeping* under
`SimEngine`'s single-threaded execution model; they are **not** a substitute for,
and must never be read as proof of, the production concurrent mailbox's (002)
correctness — that remains the threaded-engine + TSan tier's job.

## Regression corpus (ADR-025, proven)

An append-only `tests/corpus/<scenario>.seeds` file, each entry storing `(seed,
topology fingerprint)`, replayed before any campaign/DFS budget on every CI run —
a complementary tier regardless of exploration strategy (SeedFarm's proven
mechanism). A fingerprint mismatch (scenario evolved) must emit an explicit
"stale corpus entry, re-mine required" CI signal rather than a silent pass — a
naive (seed-only) corpus was proven to hide exactly this gap.

## Dependencies

Std-only: PRNG from `<random>` (explicitly seeded — the hot-path ban on
`Math.random`-style nondeterminism does not apply to the seeded test scheduler),
virtual clock over the same `steady_clock` abstraction (011). No external test
framework required; `TestKit`/`SimEngine` integrate with any (GoogleTest, Catch2,
doctest) as plain objects.

**Throughput (ADR-025, proven).** A single reference core sustains **≥50,000**
independent seeded `SimEngine` runs/sec for a representative small scenario, once
pool/timer capacities are right-sized and lazily grown — the shipped default's
eager pool-capacity pre-construction was the actual bottleneck, not the picker.
This is orthogonal to, and fixed independent of, the exploration-strategy
decision.

Any future perf note for scheduler-side oracles (shadow-FIFO ledger etc.) should
cite **~200–250ns/msg**, not an aspirational ≤50ns — that is a test-only,
non-hot-path cost by this RFC's own definition of the hot path.

## Open questions

- *(Interleaving exploration strategy: resolved — a compile-time `Chooser`
  policy for random search, plus `explore_bounded(N, branch_cap)` DPOR-pruned
  bounded model checking for systematic exploration. See "Chooser policy" and
  "Bounded exploration (DPOR)" above, ADR-025.)*
- How much of distribution (010) is simulated in-process vs. requiring a
  multi-process integration tier.
- Performance-regression testing: deterministic sim gives correctness, not
  throughput. *(Resolved: the benchmark harness + quantified budgets are
  [023-Performance-Targets-and-Budgets](023-Performance-Targets-and-Budgets),
  which runs on the **native** PAL backend — the perf counterpart to this spec's
  **sim** backend. A design must pass both: right here, fast there.)*
