# 013 — Configuration

Defines the engine's configuration surface and, more importantly, the **boundary
between policy and configuration**. Behavioral intent stays in compile-time CRTP
policies (005); configuration is for **operational** knobs only. "Convention over
configuration" means: valid defaults for everything, and configuration cannot
change safety semantics.

## Policy vs. configuration

| Belongs in **policy** (compile-time, 005) | Belongs in **configuration** (startup, here) |
|---|---|
| Execution mode (`Sequential`/`Reentrant`) | Worker/thread count, CPU pinning, NUMA layout |
| Placement strategy | Shard count |
| Failure/supervision strategy | Mailbox bound defaults, overflow policy |
| Resource lifetimes | Persistence store path/backend, `fsync` policy |
| Message protocol | Transport endpoints, cluster seeds, stabilization window, **large-scale topology/connections/placement-cache** (010, 021, 026) |
| — | Default drain budget, idle timeout, validation mode |

Rule: **configuration may not override a safety invariant.** It can set the
*default* drain budget but cannot make a `Sequential` actor reentrant; it can set
a mailbox bound but cannot remove FIFO ordering.

## Source of truth: a programmatic struct

The canonical configuration is a plain struct built with a builder — **no parser,
no file-format dependency in the core**:

```cpp
quark::EngineConfig cfg = quark::ConfigBuilder{}
    .workers(std::thread::hardware_concurrency())
    .shards(64)
    .numa(quark::Numa::Auto)
    .default_drain_budget(64)
    .default_mailbox_bound(4096, quark::Overflow::Block)   // 006
    .validation(quark::Validation::Strict)                 // 008
    .persistence({.store = quark::WalStore, .path = "/var/lib/quark"}) // 012
    .cluster({.transport = quark::TcpTransport, .seeds = {...}})       // 010
    .build();
```

### Large-scale topology knobs (026)

The [026](026-Large-Scale-Cluster-Topology.md) topology is **configuration, not
policy** — the knobs change *speed and path*, never *which node owns an actor*. They
are **zero-cost when unused**: the small default (`Flat/FullMesh/Direct`) instantiates
none of the bins/partial-view/relay machinery.

```cpp
.cluster({
    .transport       = quark::TcpTransport,
    .seeds           = {...},
    .topology        = quark::Topology::PartialView,     // Flat | Partitioned | PartialView
    .connections     = quark::Conn::BoundedPartialView,  // FullMesh | Gateway | BoundedPartialView
    .placement_cache = quark::Cache::VirtualBins,        // Direct | VirtualBins | Resolved
    .max_nodes       = 4096,                              // sizes the bin table
})
```

Validation (008) enforces `B ≥ 16·max_nodes`, `active_view ≈ c·log(max_nodes)`, seeds
present iff distribution is enabled, and — critically — **config may not override an
invariant**: no non-deterministic placement for stateful actors, no FIFO removal.

### Lazy-activation broker concurrency (ADR-028)

[ADR-028](decisions/ADR-028-lazy-activation-idle-timeout-eviction.md)'s per-shard
`ActivationBroker` (the lazy first-touch construction seam) is **permanently
Sequential — not a `MaxConcurrency<K>`/`broker_concurrency` config knob.** The ADR's
own Spec Recommendations named `broker_concurrency` as a documented BuildOnly
tunable, but that assumed a Reentrant broker; the actual implementation
(ADR-028 Phase 4) deliberately kept it Sequential because `construct_and_wire` is
fully synchronous end to end (Phase 5's Snapshot-model persistence recovery is a
direct, non-`co_await` `Store` call, not a suspend point) — a Reentrant broker would
buy no throughput while re-importing the exact `activating_`-set erase-ordering
concurrency bug the ADR's proving run found and fixed. **There is deliberately no
config knob here**: exposing `broker_concurrency` would control nothing real, which
this project treats as worse than no knob at all. If a future phase makes
construction genuinely asynchronous (e.g. EventSourced lazy-recovery), a real
`MaxConcurrency<K>` broker — and the knob to bound it — is the seam to revisit then.

The ADR's residual risk #2 ("broker convoy risk... needs a 009 counter") is resolved
via observability instead of a knob: `broker_wakes_enqueued`/`broker_wakes_handled`
(`ShardCounters`, 009) let a consumer derive live per-shard broker queue depth as
`enqueued − handled` (the same idiom as `GovernanceCore::depth()`), and
`broker_stall_ns` (a histogram) measures Wake-enqueue-to-dispatch latency — the
observable symptom of a convoy, without needing to change the broker's concurrency
model to detect one.

### Alternatives considered

- **A built-in YAML/JSON/TOML file format in the core**: pulls a parser
  dependency into every build and invites putting *behavior* in files, which the
  policy/config boundary forbids.
- **Environment-variable-only**: awkward for nested/structured config.
- **Decision:** the programmatic `EngineConfig` struct is the single source of
  truth. File and env loaders are **optional** layers that *produce* an
  `EngineConfig`; the core never parses anything.

## Optional loaders (layered, outside the core)

Loaders merge into an `EngineConfig` with a defined precedence (later wins):

```
compiled defaults  <  file (optional adapter)  <  environment  <  explicit builder calls
```

- **Environment** — std-only parsing of `QUARK_*` variables (e.g.
  `QUARK_WORKERS`, `QUARK_SHARDS`). No dependency.
- **File** — an *optional* TOML/JSON adapter behind a `ConfigSource` seam, linked
  only if the host wants file config. Not part of the default build.

## Per-actor-type overrides

Operational knobs can be overridden per actor type at startup, keyed by
`type_key` (008), without recompiling:

```cpp
cfg.for_actor<Order>()
   .drain_budget(128)
   .mailbox_bound(16384, quark::Overflow::DropOldest)
   .idle_timeout(30s);
```

These override the policy-supplied *defaults* for tunables that are safe to tune
(budget, bounds, timeouts). Overriding a non-tunable (execution mode, placement
strategy) is rejected at Validation (008).

## Reconfig class and override scope (ADR-008)

Resolved by [ADR-008](decisions/ADR-008-engine-actor-configuration-and-activation-lifecycle-policy.md)
(D3 Frozen-Core + Hot-Leaf, proven). Every knob declares **two** compile-time facts —
its **override scope** and its **reconfig class** — generated from **one X-macro table**
so the `consteval` trait and its runtime (loader) mirror can never drift (a parity test
gates divergence).

**Override scope** — the fixed five-layer precedence, **last-highest-setter-wins**:

```
built-in defaults  <  engine  <  node  <  actor-type  <  instance
```

Precedence is resolved **once**, at `build()`/reconfig time — never per message (proven
deterministic: 2,000,000/2,000,000 resolve to the highest setter). A per-instance
override materializes a **full resolved private word** (not just the instance layer), and
a type/node/engine-scope live delta fans into those masked instance cells via a sweep
(surfaced as `ReconfigReceipt.masked_count`).

**Reconfig class** — whether a knob may change on a running engine:

| Class | Knobs | Behavior |
|---|---|---|
| **BuildOnly** | worker/thread count, shard count, NUMA layout, placement topology, execution mode, type set, `max_types` cap | A live change is **rejected fail-fast** (`ReconfigError::BuildOnly`), mutating nothing. |
| **Live** | default & per-type drain budget, mailbox bound + overflow, idle timeout | Resolved once through the precedence chain, then **packed into one 8-byte-aligned atomic word** per `(shard × type_index)`. |

The **operational read-set** (drain budget 002, mailbox bound + overflow 006/022, idle
timeout) is the packed Live word. The hot-path read is a **single `mov + mask` relaxed
load** — 0 cross-core RMW, no decode branch, no possible tear (an 8-byte-aligned word is
hardware-indivisible on x86-64; proven 0 torn over 4.6B reads, objdump-confirmed single
load on both compilers at -O2 and -O3+LTO). This satisfies the 023 0-RMW drain gate and
the ≤100 ns local-tell budget directly.

## Live reconfiguration (ADR-008)

`reconfigure(delta)` is a **control-plane** API returning
`std::expected<ReconfigReceipt, ReconfigError>`:

- A **Live** delta re-resolves the affected `(shard × type_index)` cells and stores **one
  packed word each** with a **single relaxed store** — the store *is* the publish. There
  is no pointer to reclaim on the hot word, so a reconfig **cannot stall the drain**
  (proven: publish 67–73 ns/op, 0 alloc; publish→visible p50 ≈ 140 ns; co-resident storm
  < 5% drain drop with `alignas(64)` cell padding — a packed-8B control showed 74% drop,
  proving the padding load-bearing). Publish→visible is a **cold-path** metric with **no
  Hard budget** (config is operational, not hot).
- A **BuildOnly** delta fails fast `ReconfigError::BuildOnly` (mutates nothing); an
  out-of-range Live value fails fast `ReconfigError::OutOfRange` (**never truncates**).
- The rare "oversized warm leaf" and the append-only metadata table (008) are the **only**
  places RCU/QSBR reclamation applies (grace = `in_flight == 0`); the hot word is never
  reclaimed.

> **Packing ceilings** (drain_budget < 2¹⁴, mailbox_bound < 2²⁴, idle_ticks 16-bit coarse
> units) are enforced as `OutOfRange` fail-fast. A future per-message knob that cannot fit
> the word must be BuildOnly or warm-indirected (which would split the single-load read).

## Validation

`EngineConfig` is validated in the startup **Validation** phase (008) with the
same `std::expected<…, ValidationReport>` model: ranges (shards > 0, budget > 0),
consistency (cluster seeds present iff distribution enabled), and resource
reachability (persistence path writable). Strict mode fails `build()`.

## Dependencies

Std-only core (struct + builder + env parsing). File-format parsers are optional
adapters behind `ConfigSource`.

## Resolved (ADR-008)

- **Live reconfiguration** → a per-knob **reconfig class** (Live vs BuildOnly); Live
  knobs pack into one relaxed-load/relaxed-store atomic word per `(shard × type_index)`,
  BuildOnly changes fail fast. See *Reconfig class* and *Live reconfiguration* above.
- **Precedence across layers** → the fixed five-layer scope `built-in < engine < node <
  actor-type < instance`, last-highest-setter-wins, resolved once at build/reconfig. (This
  scope hierarchy is orthogonal to the loader-source precedence `defaults < file < env <
  builder` above, which sources a single layer's value.)
- **Secrets handling** for transport/persistence credentials — the `SecretSource` seam + a
  `SecurityMode` config knob, [020-Security.md](020-Security.md); `EngineConfig` carries
  secret references, never values.

## Open questions

- Control-plane **rate-limiting / coalescing** of the publish path so an adversarial
  reconfig storm cannot ping-pong one shard's cache line (ADR-008 residual risk 3; bounded
  to <5% by cell padding, but worth a governor).
