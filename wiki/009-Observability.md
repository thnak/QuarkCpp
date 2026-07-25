# 009 — Observability

Metrics, tracing, deadline accounting, and dead-letter inspection — with **zero
hot-path synchronization** and **no mandatory external telemetry dependency**.
Everything is a seam with a std-only default; adapters (OpenTelemetry, Prometheus
client) are optional and live outside the core.

## Metrics

### Storage: per-shard, contention-free

Counters live **inside each shard** as plain (non-atomic) integers. A shard is
drained by one worker at a time (002), so its own counters need no synchronization
on the hot path. A scraper reads each shard's counters with a relaxed atomic
snapshot and **aggregates on read** — the only cross-thread interaction, and it is
off the hot path.

```cpp
struct ShardCounters {
    uint64_t messages_processed;
    uint64_t mailbox_enqueued;
    uint64_t activations;
    uint64_t restarts;         // 007
    uint64_t steals;           // 002
    uint64_t wakeups;          // 002
    uint64_t dead_letters;     // 007
    uint64_t deadline_misses;  // 011
    uint64_t broker_wakes_enqueued;  // ADR-028: lazy-activation Wake control messages posted
    uint64_t broker_wakes_handled;   // ADR-028: Wake messages the shard's broker has processed
    // + user-defined counters, indexed by a registered slot
};
```

### Broker convoy observability (ADR-028)

The per-shard `ActivationBroker` (lazy first-touch activation, ADR-028) is a single
Sequential lane per shard, so a burst of first-touches/reactivations exceeding what it
can drain immediately — or one pathologically slow reload — queues later requests
behind it (ADR-028 residual risk #2). This is observable, not just bounded in theory:
`broker_wakes_enqueued`/`broker_wakes_handled` give live per-shard broker queue depth as
`enqueued − handled` (the producer/drain-owner split mirrors `mailbox_enqueued`/
`messages_processed`), and a `broker_stall_ns` histogram (alongside `message_latency_ns`/
`mailbox_depth`) records Wake-enqueue-to-dispatch-start latency, so a convoy shows up as
a widening stall distribution before it becomes an outage.

### Alternatives considered

- **Global atomic counters**: every `++` is a cross-core RMW — false sharing and
  contention on the hottest events. Rejected.
- **Per-thread TLS counters**: works, but a shard is the natural ownership unit
  here and already single-writer, so per-shard is simpler and maps to placement.
- **Decision:** per-shard plain counters, aggregate-on-scrape.

### Export

A `MetricsSink` seam. Built-in, dependency-free options:

- **Snapshot API** — `engine.metrics_snapshot()` returns a struct (for tests, 014,
  and embedding).
- **Prometheus text exposition** — pure string formatting over the snapshot; no
  Prometheus client library.

Histograms (latency, mailbox depth) use fixed-bucket HDR-style arrays per shard —
no allocation, mergeable on scrape.

### Histogram bucket layout and cardinality (ADR-022, proven)

**Bucket layout** is per-metric, configurable via a compile-time `HistogramSpec`
(`boundaries()` + `bucket_count`) — not one universal scheme. Validation rejects an
over-provisioned spec: `bucket_count` is capped (e.g. ≤ 64, mirroring the per-type
grid's `kHistogramCap`) so a single metric can't blow up per-record or per-cell
cost. Bucket resolution is a **closed-form lookup**, not a linear boundary scan:
`HistogramSpec`-declared metrics use the `{shift, step_shift}` closed-form technique
(`bucket_index_of`) proven within noise of the shipped `bucket_of`, in place of
`while (v > b[i]) ++i;`.

**Cardinality** is two-level and default-safe:

- **Per-actor-type** always exists — one block per registered type, dense
  `type_index`-addressed, sized once at build-time registration, never reallocated.
- **Per-actor-instance** is strictly **opt-in** via a `PerInstanceMetrics<N>` CRTP
  policy, backed by one engine-wide, build-time-sized `critical_arena` (prefix-sum
  offsets per opted-in type + a per-(shard, type) fixed-capacity freelist). The
  instance slot is resolved once at activation creation and cached on the
  activation — never re-resolved per message. Per-instance cardinality multiplies
  the *same* per-metric bucket layout into more physical blocks; it never changes
  bucket semantics.

**Memory-order contract (normative):** histogram cells are plain (non-atomic)
integers on the single-writer side; the scraper reads via `std::atomic_ref` at
relaxed order and aggregates on read. This depends on 002's drain-owner handoff
establishing happens-before across a work-steal migration — the TSan
migration-stress test proven clean in ADR-022 is a permanent CI regression guard
for that dependency, not an optional check.

**Build-time budget gate:** `Engine::build()` computes the exact worst-case
footprint of any metrics grid/arena via `sizeof()` (hand arithmetic previously
undercounted this by 3.5× in a draft) and rejects with
`std::unexpected(MetricsBudgetExceeded)` if it exceeds a configured
`metrics_memory_budget_bytes` knob (013-style), before attempting the allocation.

**Per-instance failure modes are observable, not silent fallback:** freelist
exhaustion degrades to per-type-only recording for overflow instances (never
allocates, never blocks) and increments a dedicated `instance_slot_exhausted`
counter. `PerInstanceMetrics<N>` combined with a `Stateless<N>` pooled actor is a
compile-time `static_assert` failure — pool activations have no stable per-key
identity for slot pinning. The instance-slot → `ActorId` label side-table is
updated atomically with `bind_activation`/`unbind_activation`.

**Inherited caveat (not re-litigated here):** the shipped `Histogram`'s relaxed
load-then-store (non-RMW) single-writer pattern under weak memory order on ARM64
is still only partially verified (see `hot_cell.hpp`'s existing caveat). ADR-022
does not resolve it, and it applies unchanged regardless of which histogram
bucket-layout/cardinality design is chosen.

## Tracing

`trace_id` already rides in `MessageContext` (004). Tracing adds **spans as
events**, sampled, written to a per-shard ring buffer:

- On message start/end, a lightweight span event `{trace_id, span_id, parent,
  actor, msg, t_start, t_end, outcome}` is appended if the trace is sampled.
- Sampling decision is made once at trace origin and propagated in the context,
  so downstream actors do no per-message sampling work.
- Interop via **W3C `traceparent`** parsing/formatting (string handling only, no
  dependency) so ids cross process/node boundaries (010) and into external tools.

A `TraceSink` seam exports the ring buffer; the default drains it to a file or the
snapshot API. Full span-tree reconstruction is a consumer concern, not the
engine's.

### Alternatives considered

- **Hard dependency on OpenTelemetry SDK**: heavy, pulls gRPC/protobuf
  transitively — violates the low-dependency goal. Provided as an *optional
  adapter* over the `TraceSink` seam instead.
- **Full in-engine span trees**: memory + pointer chasing on the hot path.
  Rejected in favor of flat sampled events, reconstructed offline.

## Deadline accounting

Deadlines are registered in the timer wheel (011). A miss (`deadline` passes
before completion) increments `deadline_misses`, emits a trace event, and drives
the message to failure (007). No polling: expiry is timer-driven.

## Dead-letter inspection

Dead-letters (007) are both:

- **counted** (`dead_letters`), and
- **retained** in a bounded per-shard ring of recent dead-letter records
  `{actor, msg type, error, trace_id, t}` for inspection and optional replay.

A `DeadLetterSink` seam lets a host forward them elsewhere; the default keeps the
ring and exposes it via the snapshot API.

## Audit

Security-relevant events (authn failures, authz denials, cluster admission
outcomes — see [020-Security](020-Security)) flow to a sibling **`AuditSink`**
seam, and authorization *denials* land in a **distinct security dead-letter**
stream so they are never conflated with ordinary poison-message dead-letters
(007). The default `AuditSink` writes structured records to stderr/file; SIEM/OTLP
export is an adapter. Secrets (020) never appear in any sink.

## Dependencies

Std-only core. Prometheus/OpenTelemetry/OTLP are optional adapters over the
sinks (`MetricsSink`, `TraceSink`, `DeadLetterSink`, `AuditSink`) and are not
linked unless the host opts in.

> The per-shard counters and latency histograms here double as the **measurement
> surface for the macrobenchmarks** of
> [023-Performance-Targets-and-Budgets](023-Performance-Targets-and-Budgets) —
> the observability layer *is* the perf instrument, so whole-engine benchmarks need
> no separate plumbing.

### Large-scale cluster metrics (026)

The same per-shard/per-node plain-counter model (no hot-path RMW) carries the 026
control-plane signals: placement-cache **hit/miss + cold-recompute** count, a
**roster-digest / membership-epoch** gauge, **gossip convergence rounds**, a
**relay-hop histogram** + ttl-exhaustion dead-letter count, and an **open-socket gauge
vs. the configured bound**. The `VirtualBins` refill runs on the membership thread, so
its cost is a **control-plane** metric, never a drain-path one.

## Open questions

- *(Histogram bucket layout: resolved — per-metric configurable via a compile-time
  `HistogramSpec`, Validation-capped `bucket_count`, closed-form bucket lookup. See
  "Histogram bucket layout and cardinality" above, ADR-022.)*
- Whether user counters are declared as a resource/policy (compile-time slot) or
  registered dynamically at startup.
- *(Cardinality control for per-actor-type vs. per-actor-instance metrics:
  resolved — per-type always on, per-instance opt-in via `PerInstanceMetrics<N>`
  over a build-time-sized `critical_arena`. See "Histogram bucket layout and
  cardinality" above, ADR-022.)*
