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
    // + user-defined counters, indexed by a registered slot
};
```

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
outcomes — see [020-Security.md](020-Security.md)) flow to a sibling **`AuditSink`**
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
> [023-Performance-Targets-and-Budgets.md](023-Performance-Targets-and-Budgets.md) —
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

- Histogram bucket layout: fixed exponential vs. configurable per metric.
- Whether user counters are declared as a resource/policy (compile-time slot) or
  registered dynamically at startup.
- Cardinality control for per-actor-type vs. per-actor-instance metrics.
