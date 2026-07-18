// Implements 009-Observability §Tracing — spans-as-events, sampled, written to a per-shard ring
// buffer, with W3C `traceparent` interop. Reflection-free, std-only. No external telemetry
// dependency; the OpenTelemetry SDK is an optional adapter over the `TraceSink` seam (009 §Tracing
// Alternatives considered), never a core include.
//
// THE ENGINE CORRELATION ID. `MessageContext::trace_id` (004/message_context.hpp) is a single
// uint64 that already rides every message and is COPIED by the Activation into the running
// handler's context (activation.hpp `current_ctx_.trace_id = d->trace_id`). 009 rides that word:
//   * bit 63 is the SAMPLED flag — the sampling decision is made ONCE at trace origin and then
//     propagated for free in the id, so downstream actors do no per-message sampling work
//     (009 §Tracing: "Sampling decision is made once at trace origin and propagated in the context").
//   * bits 0..62 are the correlation id.
// This is the whole of the in-engine tracing state; full span-tree reconstruction is a consumer
// concern (009 §Tracing: "reconstructed offline"). The 128-bit W3C trace-id used at process/node
// boundaries is carried by TraceContext and folded to/from this word at the edge (see below).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/message_context.hpp"

namespace quark {

// --- Sampled bit + correlation id helpers over the engine trace_id word -----------------------
inline constexpr std::uint64_t kTraceSampledBit = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kTraceCorrelationMask = ~kTraceSampledBit;

[[nodiscard]] constexpr bool trace_is_sampled(std::uint64_t engine_trace_id) noexcept {
    return (engine_trace_id & kTraceSampledBit) != 0;
}
[[nodiscard]] constexpr std::uint64_t trace_correlation_id(std::uint64_t engine_trace_id) noexcept {
    return engine_trace_id & kTraceCorrelationMask;
}
// Mint an engine trace_id from a seed correlation id + the origin sampling decision. The decision
// is baked in here and carried unchanged through every hop (tell/ask/async) — a child message
// copies the parent's trace_id verbatim, so `trace_propagate` is identity on the id.
[[nodiscard]] constexpr std::uint64_t make_engine_trace_id(std::uint64_t seed, bool sampled) noexcept {
    return (seed & kTraceCorrelationMask) | (sampled ? kTraceSampledBit : 0);
}
// Propagate the trace across a send: the child carries the SAME correlation id + sampled bit as the
// parent's running context. (The router stamps this onto the outbound descriptor — see the reported
// hook; today the standalone helper is what a handler uses to stamp a child descriptor's trace_id.)
[[nodiscard]] constexpr std::uint64_t trace_propagate(const MessageContext& parent) noexcept {
    return parent.trace_id;
}

// --- Sampler: the origin decision (009 §Tracing). Deterministic ratio over the correlation id, so
// two nodes that see the same id agree, and no per-message RNG runs on the hot path. --------------
class Sampler {
public:
    // ratio in [0,1]; 1 == sample all, 0 == sample none.
    explicit constexpr Sampler(double ratio = 1.0) noexcept
        : threshold_(ratio <= 0.0    ? std::uint64_t{0}
                     : ratio >= 1.0  ? ~std::uint64_t{0}
                                     : static_cast<std::uint64_t>(ratio * 1.8446744073709552e19)) {}

    [[nodiscard]] constexpr bool should_sample(std::uint64_t seed) const noexcept {
        // splitmix64 finalizer so a low-entropy seed still spreads across the threshold.
        std::uint64_t z = seed + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return z <= threshold_;
    }

private:
    std::uint64_t threshold_;
};

// --- Span outcome + event (009 §Tracing: `{trace_id, span_id, parent, actor, msg, t_start, t_end,
// outcome}`). Flat, trivially-copyable — no pointer chasing, no in-engine span tree. ---------------
enum class SpanOutcome : std::uint8_t {
    Ok = 0,
    Failed = 1,        // handler faulted (007)
    DeadLetter = 2,    // undeliverable
    DeadlineMiss = 3,  // deadline overrun (011/018 accounted by 009)
    Cancelled = 4,     // stop_token fired (001)
};

struct SpanEvent {
    std::uint64_t trace_id = 0;         // engine trace_id (correlation id + sampled bit)
    std::uint64_t span_id = 0;
    std::uint64_t parent_span_id = 0;
    std::uint64_t actor = 0;            // actor key / ActorId::hash() (0 if unknown)
    std::uint32_t msg_slot = 0;         // dense dispatch slot (ADR-007) / message type discriminator
    SpanOutcome outcome = SpanOutcome::Ok;
    std::int64_t t_start_ns = 0;        // steady-clock ns
    std::int64_t t_end_ns = 0;
};

// --- TraceSink seam (009 §Tracing). The default keeps the ring; adapters (file, OTLP) export it. --
struct TraceSink {
    void (*fn)(const SpanEvent&, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
    void operator()(const SpanEvent& e) const noexcept {
        if (fn) fn(e, ctx);
    }
};

// --- SpanRing: per-shard, fixed-capacity, single-writer append (009 §Tracing). No allocation on
// the record path (a fixed array); overwrites the oldest event when full (retain-recent). The
// head counter is a relaxed single-writer store (no atomic RMW), matching the metrics model.
// snapshot()/drain() are OFF the lane (scrape time) — call them from the owning lane or while the
// actor is quiescent; a concurrent scrape of the payload during append is a documented refinement
// (seqlock/double-buffer), not exercised on the hot path. --------------------------------------
template <std::size_t Cap = 256>
class SpanRing {
public:
    static_assert(Cap > 0, "span ring capacity must be positive");

    SpanRing() noexcept = default;
    SpanRing(const SpanRing&) = delete;
    SpanRing& operator=(const SpanRing&) = delete;

    // Append a span (only if sampled — callers should gate on trace_is_sampled). Single-writer.
    QUARK_ALWAYS_INLINE void append(const SpanEvent& e) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        buf_[static_cast<std::size_t>(h % Cap)] = e;
        head_.store(h + 1, std::memory_order_relaxed);
        sink_(e);  // optional export seam (default: none)
    }

    void set_sink(TraceSink s) noexcept { sink_ = s; }

    [[nodiscard]] std::uint64_t total_recorded() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t t = total_recorded();
        return t < Cap ? static_cast<std::size_t>(t) : Cap;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Cap; }

    // Copy the retained events (oldest->newest) into `out` (off-lane scrape).
    void snapshot(std::vector<SpanEvent>& out) const {
        const std::uint64_t t = total_recorded();
        const std::size_t n = t < Cap ? static_cast<std::size_t>(t) : Cap;
        out.clear();
        out.reserve(n);
        const std::uint64_t start = t < Cap ? 0 : t - Cap;
        for (std::uint64_t i = start; i < t; ++i)
            out.push_back(buf_[static_cast<std::size_t>(i % Cap)]);
    }

private:
    std::array<SpanEvent, Cap> buf_{};
    std::atomic<std::uint64_t> head_{0};
    TraceSink sink_{};
};

// --- W3C traceparent interop (009 §Tracing: "Interop via W3C traceparent ... so ids cross
// process/node boundaries (010) and into external tools"). String handling only, no dependency. --
// Format: `version(2hex)-trace_id(32hex)-parent_id(16hex)-flags(2hex)`, e.g.
//   00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
struct ParsedTraceparent {
    std::uint64_t trace_hi = 0;  // W3C 128-bit trace-id, high 64 bits
    std::uint64_t trace_lo = 0;  // low 64 bits
    std::uint64_t span_id = 0;
    bool sampled = false;

    // Fold the 128-bit W3C id to the 63-bit engine correlation id (xor-fold, top bit cleared) and
    // re-attach the sampled flag. Lossy for FOREIGN ids (correlation only), exact for our own
    // (we emit trace_hi = 0, so the fold is the identity on trace_lo's low 63 bits).
    [[nodiscard]] std::uint64_t engine_trace_id() const noexcept {
        return make_engine_trace_id(trace_hi ^ trace_lo, sampled);
    }
};

namespace detail {
inline void hex_encode(std::uint64_t v, char* out, int nybbles) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = nybbles - 1; i >= 0; --i) {
        out[i] = kHex[v & 0xF];
        v >>= 4;
    }
}
[[nodiscard]] inline bool hex_decode(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty() || s.size() > 16u) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}
}  // namespace detail

// Format an engine trace_id (+ span id) as a W3C traceparent. The 128-bit trace-id is the 63-bit
// correlation id zero-extended (trace_hi = 0); the sampled flag becomes W3C flags bit 0.
[[nodiscard]] inline std::string format_traceparent(std::uint64_t engine_trace_id,
                                                    std::uint64_t span_id) {
    char buf[55];  // 2 + 1 + 32 + 1 + 16 + 1 + 2 = 55
    buf[0] = '0';
    buf[1] = '0';
    buf[2] = '-';
    detail::hex_encode(0, buf + 3, 16);                                  // trace_hi
    detail::hex_encode(trace_correlation_id(engine_trace_id), buf + 19, 16);  // trace_lo
    buf[35] = '-';
    detail::hex_encode(span_id, buf + 36, 16);
    buf[52] = '-';
    const bool sampled = trace_is_sampled(engine_trace_id);
    buf[53] = '0';
    buf[54] = sampled ? '1' : '0';
    return std::string(buf, sizeof(buf));
}

[[nodiscard]] inline std::optional<ParsedTraceparent> parse_traceparent(std::string_view s) noexcept {
    // version "-" trace(32) "-" span(16) "-" flags(2)
    if (s.size() < 55) return std::nullopt;
    if (s[2] != '-' || s[35] != '-' || s[52] != '-') return std::nullopt;
    std::uint64_t ver = 0;
    if (!detail::hex_decode(s.substr(0, 2), ver)) return std::nullopt;
    if (ver == 0xFF) return std::nullopt;  // invalid per W3C
    ParsedTraceparent p;
    if (!detail::hex_decode(s.substr(3, 16), p.trace_hi)) return std::nullopt;
    if (!detail::hex_decode(s.substr(19, 16), p.trace_lo)) return std::nullopt;
    if (!detail::hex_decode(s.substr(36, 16), p.span_id)) return std::nullopt;
    std::uint64_t flags = 0;
    if (!detail::hex_decode(s.substr(53, 2), flags)) return std::nullopt;
    if (p.trace_hi == 0 && p.trace_lo == 0) return std::nullopt;  // all-zero trace-id is invalid
    if (p.span_id == 0) return std::nullopt;                      // all-zero parent-id is invalid
    p.sampled = (flags & 0x01) != 0;
    return p;
}

}  // namespace quark
