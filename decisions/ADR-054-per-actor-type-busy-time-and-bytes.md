# ADR-054 — Per-actor-type handler busy-time and message bytes in the 009 metrics (#25)

Status: Accepted (opt-out via `-DQUARK_NO_TYPE_ACCOUNTING`; the <2% overhead goal is **not** demonstrated
on the measured configuration, see section 4). Evidence: `tests/metrics_type_accounting_test.cpp`,
`bench/type_accounting_bench.cpp` (+ `type_accounting_bench_off` twin).

## 1. Issue claims, verified against the code
- "per-type handler busy-time and allocated bytes appear in 009 metrics": **true that they were
  absent.** `MetricsSnapshot` held only engine-wide counters + 3 histograms. 009 (section "Cardinality")
  says a per-actor-type block "always exists", but no such block was wired into the engine; the
  `TypeMetricsGrid` in `metrics_cardinality.hpp` is a standalone, un-integrated experiment (ADR-022).
- Further findings while verifying: `ShardCounters::message_latency_ns` and `mailbox_depth` are declared
  but **never recorded anywhere** in include/ or src/ (only `broker_stall_ns` is). So no per-message
  timing existed to piggy-back on; this ADR does not fix that (out of scope).
- `Engine::spawn<A>()` does **not** register `A` in `TypeRegistry` (only `declare_lazy<A>()` does), so
  the TypeRegistry `index` cannot be the per-type slot for spawned actors. Using it would also have made
  a later `declare_lazy<A>` of the same type collide in Strict mode.

## 2. Design
- **Slots.** `MetricsRegistry::assign_type_slot(TypeKey, name, cap)` (cold, mutex-guarded; spawn and
  broker lazy-construct can run concurrently) find-or-assigns a dense slot, capped at
  `EngineConfig::max_types`. The slot space is the metrics registry's own, keyed by `TypeKey`
  (`type_key_of<A>()`); it coincides with the TypeRegistry index only by accident. Cap overflow returns
  -1: that type simply gets no accounting (never an error/allocation).
- **Cells.** `ShardCounters::type_cells` = flat `TypeCell[max_types]` per shard, allocated once in the
  `Engine` ctor, never resized. `TypeCell{ MetricCounter busy_ns, bytes }`. The `Activation` caches a
  direct `TypeCell*` (`set_type_cell`, set cold at spawn / lazy construct), so the hot path does no
  index math, no map, no string. No `TypeCell` pointer (e.g. `register_activation` callers, stateless
  pools, hand-built test Activations) => no per-type accounting for that activation.
- **Single-writer idiom.** Both counters are drain-owner-exclusive and use `MetricCounter::inc()`
  (relaxed load+store, no RMW), summed across shards on snapshot, like `messages_processed`.
  0 heap allocation on the record path; everything `noexcept`.
- **bytes** = per dispatched message `payload_size + sizeof(Descriptor)` (logical message bytes, 56 B
  descriptor on MSVC), counted at dispatch on the receiving type. A drain-local accumulator
  (`detail::TypeBytesAcc`) adds in a register and publishes once per `drain_step` call (destructor,
  every return path) instead of a per-message load+store chain on the shared cell. **It is not the
  pool cell size** (cells are a fixed 192 B payload area; the logical size is more informative for
  quotas) and it **excludes** heap the handler itself allocates, ask reply cells, and the sender-side
  envelope pool. Counted at dequeue/dispatch, not at the sender's allocation, to keep producers free of
  per-type RMWs (many producers per shard).
- **busy_ns** = wall time of `Engine::run_activation` (one activation's scheduling quantum: up to
  `budget` messages + close-out), charged to the activation's type: **one `pal::now()` pair per
  quantum, not per message.** Granularity caveats: includes dequeue/linger/close-out overhead; a
  handler that blocks the lane is counted as busy (wall, not CPU time); async continuations resumed on
  a foreign carrier via `Engine::complete_parked` are **not** counted (counting them would double-count
  when the carrier thread is itself inside another actor's `run_activation`, and that path is not
  drain-owned, so it would need `inc_atomic`). Reentrant (`MaxConcurrency>1`) activations get quantum
  time like the rest, but their bytes are counted in `admit()` (per message, no accumulator).
- **Export.** `MetricsSnapshot::types` (`std::vector<TypeMetric{key,name,busy_ns,bytes}>`, one row per
  slot; the snapshot is therefore no longer trivially copyable, only copyable) and Prometheus series
  `quark_actor_type_handler_busy_ns_total{type_key="0x..",type="<canonical name>"}` and
  `quark_actor_type_message_bytes_total{...}`. The name label is present when the type was seen via
  `spawn<A>`/`declare_lazy<A>`; a type first seen only through the broker keeps the hex key.
- **Opt-out.** `-DQUARK_NO_TYPE_ACCOUNTING` removes the cell allocation, the accumulator, and the clock
  pair at compile time (`type_cell_for` returns null; `types` stays empty).

## 3. Tests (all pass on MSVC Release, build-e)
`metrics_type_accounting_test`: registry-level cross-shard aggregation, find-or-assign idempotence, slot
cap, Prometheus rows; real 2-shard engine with two actor types of different payload sizes: bytes
attributed **exactly** per type, busy_ns >= 15 ms of 20 ms of real spin handler time for the busy type
and far less for the light one, name label from `spawn<A>`. **Not tested:** the lazy (`declare_lazy` +
broker) wiring path and the TSan migration case (no sanitizer run on this Windows box).

## 4. Measured overhead (honest)
Machine: Windows 11, MSVC (VS 18 2026) Release /O2, one worker pinned to one core (affinity mask 0x4,
High priority), 200k pre-enqueued `tell`s to a trivial sync handler, 15 reps x 3 rounds, ns per
message start-to-last-handled, ON vs the `_off` twin:

| budget | ON p50 (3 rounds) | OFF p50 (3 rounds) | delta |
|---|---|---|---|
| 64 | 44.3 / 45.9 / 44.5 | 43.9 / 44.4 / 41.7 | +1% / +3% / +7% (min-based: +3 to +5%) |
| 8  | 50.1 / 50.6 / 50.2 | 48.5 / 46.3 / 44.9 | +3% / +9% / +12% |

Run-to-run noise between identical OFF rounds is ~+/-3% on p50, so budget 64 is **noise-limited at
roughly 1.5 ns/msg (~3%)**: this does **not** demonstrate the issue's <2% on an empty-handler bench, and
the true cost is probably ~3%. The cost model: the clock pair (~40-50 ns on QPC) amortizes over the
batch (0.7 ns/msg at budget 64, ~6 ns/msg at budget 8) plus ~0.5-1 ns/msg for the byte accumulate.
For any handler doing real work (>= ~200 ns) the relative cost is well under 1%. A first draft with a
per-message load+store on the shared cell measured ~+4-7% at budget 64 before the accumulator.
Possible further reduction (not done): a cheaper timestamp (TSC with calibration) per quantum, or
sampling every Nth quantum. `bench-gate` result: see the task report (may not be meaningful on this box;
the gate script targets the Linux/pinned CI environment).

## 5. Not done
Per-type message *count*, per-instance accounting (`PerInstanceMetrics<N>`), TypeRegistry-index
alignment of the slot space, ARM64/weak-memory verification (inherits the `hot_cell.hpp` caveat),
CPU-time (vs wall-time) busy measurement.
