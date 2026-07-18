// Implements 009-Observability — the umbrella include tying the four surfaces together plus the
// two cross-cutting seams the spec names (deadline accounting; audit). Zero hot-path
// synchronization, no mandatory external telemetry dependency: everything here is a seam with a
// std-only default, and heavy backends (OpenTelemetry, Prometheus client, OTLP) are optional
// adapters over the sinks (MetricsSink, TraceSink, DeadLetterSink, AuditSink), never in the core.
//
//   #include "quark/core/observability.hpp"   // metrics + tracing + dead-letters + deadline + audit
//
// ===========================================================================================
// SEAMS TO OTHER SPECS / HOOKS NEEDED IN CORE HEADERS (reported, NOT edited this session):
//
//  * 018 (Clocks & Deadlines) — 009 does NOT own the deadline model. `MessageContext::deadline_ns`
//    is a steady-clock INSTANT produced by 011/018 (e.g. TimerService::deadline_from_delay). 009
//    only ACCOUNTS overruns: `deadline_expired(ctx, now_ns)` compares it to `steady_now_ns()`, and
//    on a miss `account_deadline_miss(...)` bumps `deadline_misses` + emits a trace event and lets
//    007 drive the message to failure. The MISS DRIVER is timer-driven (011: "No polling: expiry is
//    timer-driven") — the 011 timer expiry / the drain edge calls `account_deadline_miss`. That call
//    site is the hook; it lives in engine/timer code owned elsewhere.
//
//  * activation.hpp `d->trace_id` — the Activation already COPIES the descriptor's trace_id into the
//    running handler's MessageContext (`current_ctx_.trace_id = d->trace_id`). That IS the tracing
//    propagation seam and needs no change.
//
//  * actor_ref.hpp `LocalRouter::post_message` HARDCODES `d->trace_id = 0` (and `deadline_ns = 0`).
//    For automatic trace propagation across `tell`/`ask`, that line must instead stamp the ambient
//    running context's trace_id (and deadline). HOOK NEEDED: post_message should read an ambient
//    "current message context" (a lane-local pointer set by the Activation around a handler) and do
//    `d->trace_id = trace_propagate(current_ctx); d->deadline_ns = current_ctx.deadline_ns;`. Until
//    that hook lands, a handler propagates manually with `trace_propagate(ctx)` (tracing.hpp). This
//    is reported, not edited (actor_ref.hpp is a locked core header this session).
//
//  * per-shard wiring — the engine owns the per-worker `ShardCounters` / `SpanRing` /
//    `DeadLetterRegistry`; `MetricsRegistry::register_shard` and `DeadLetterRegistry::as_sink()`
//    (which plugs into `Activation::set_dead_letter_sink`) are the registration seams. No core edit.
// ===========================================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "quark/core/audit.hpp"  // AuditSink/AuditRecord/AuditKind — moved to a lean sibling header (020)
#include "quark/core/dead_letter.hpp"
#include "quark/core/error.hpp"
#include "quark/core/message_context.hpp"
#include "quark/core/metrics.hpp"
#include "quark/core/tracing.hpp"
#include "pal/pal.hpp"

namespace quark {

// Steady-clock now in ns since the epoch — the SAME basis as MessageContext::deadline_ns and
// TimerService::deadline_from_delay (011/018). The one clock read 009 does, off the record path.
[[nodiscard]] inline std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch())
        .count();
}

// ===========================================================================================
// Deadline accounting (009 §Deadline accounting). 009 accounts/reports overruns; the deadline
// MODEL is 018's and the expiry is timer-driven (011). Coordinate at this seam — don't duplicate.
// ===========================================================================================

// True iff the message carries a deadline (deadline_ns != 0) that has passed at `now_ns`.
[[nodiscard]] constexpr bool deadline_expired(const MessageContext& ctx, std::int64_t now_ns) noexcept {
    return ctx.deadline_ns != 0 && now_ns >= ctx.deadline_ns;
}

// Account one deadline overrun: bump the shard `deadline_misses` counter (009 §Metrics) and, if the
// trace is sampled, append a DeadlineMiss span event (009 §Deadline accounting: "increments
// deadline_misses, emits a trace event, and drives the message to failure (007)"). Driving the
// message to failure is 007's job at the call site; this records the observability side.
template <std::size_t Cap>
inline void account_deadline_miss(ShardCounters& sc, SpanRing<Cap>& ring,
                                  const MessageContext& ctx, std::uint64_t actor,
                                  std::uint32_t msg_slot, std::int64_t now_ns) noexcept {
    sc.deadline_misses.inc();
    if (trace_is_sampled(ctx.trace_id)) {
        SpanEvent e;
        e.trace_id = ctx.trace_id;
        e.actor = actor;
        e.msg_slot = msg_slot;
        e.outcome = SpanOutcome::DeadlineMiss;
        e.t_start_ns = ctx.deadline_ns;
        e.t_end_ns = now_ns;
        ring.append(e);
    }
}

// ===========================================================================================
// Audit (009 §Audit + 020-Security). The AuditSink seam + records + default stderr sink now live in
// the lean `audit.hpp` sibling (included above) so the 020 security seams compose them without the
// full 009 surface. Security-relevant events flow to a SIBLING AuditSink; authz DENIALS land in a
// DISTINCT security dead-letter stream (authorizer.hpp) so they are never conflated with ordinary
// poison-message dead-letters (007). Secrets (020) never appear in any sink.
// ===========================================================================================

}  // namespace quark
