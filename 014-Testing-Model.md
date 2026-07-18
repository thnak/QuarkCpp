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

## Relationship to production code

There is **one** actor/handler/dispatch implementation (008). `SimEngine` and the
production engine differ only in the *scheduler* and *clock* implementations behind
a small internal interface, so tests exercise production dispatch, resource wiring,
and supervision — not a parallel mock universe.

## Dependencies

Std-only: PRNG from `<random>` (explicitly seeded — the hot-path ban on
`Math.random`-style nondeterminism does not apply to the seeded test scheduler),
virtual clock over the same `steady_clock` abstraction (011). No external test
framework required; `TestKit`/`SimEngine` integrate with any (GoogleTest, Catch2,
doctest) as plain objects.

## Open questions

- Interleaving exploration strategy: random search vs. bounded model checking
  (systematic exploration up to N context switches).
- How much of distribution (010) is simulated in-process vs. requiring a
  multi-process integration tier.
- Performance-regression testing: deterministic sim gives correctness, not
  throughput. *(Resolved: the benchmark harness + quantified budgets are
  [023-Performance-Targets-and-Budgets.md](023-Performance-Targets-and-Budgets.md),
  which runs on the **native** PAL backend — the perf counterpart to this spec's
  **sim** backend. A design must pass both: right here, fast there.)*
