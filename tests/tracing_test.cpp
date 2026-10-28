// Tests 009-Observability §Tracing — the trace_id correlation id (with its sampled bit) propagates
// through a tell chain: the origin stamps a descriptor's trace_id, the Activation copies it into the
// running handler's MessageContext, the handler records a span and PROPAGATES the same id onto a
// downstream (child) message, and the child handler observes the identical trace_id. Also checks the
// W3C traceparent round-trip and the sampler.
#include <cassert>
#include <cstdio>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/tracing.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

// A hop in the trace chain: reads the ambient MessageContext.trace_id, records a span into its ring,
// and remembers what it saw. `Hop::observed` lets the test assert propagation across hops.
struct Msg {
    int n;
};

struct Hop : Actor<Hop, Sequential> {
    using protocol = Protocol<Msg>;
    SpanRing<64>* ring = nullptr;
    std::uint64_t observed = 0;
    std::uint64_t observed_span = 0;

    void handle(const Msg&, const MessageContext& ctx) noexcept {
        observed = ctx.trace_id;
        if (trace_is_sampled(ctx.trace_id)) {
            SpanEvent e;
            e.trace_id = ctx.trace_id;
            e.span_id = ++span_seq_;
            observed_span = e.span_id;
            e.actor = actor_key_;
            e.outcome = SpanOutcome::Ok;
            e.t_start_ns = 1;
            e.t_end_ns = 2;
            if (ring) ring->append(e);
        }
    }
    std::uint64_t actor_key_ = 0;
    std::uint64_t span_seq_ = 0;
};

// Drive one message through an activation to DrainedEmpty (single-executor, like exec_dispatch_test).
// NOTE: side-effecting calls stay OUT of assert() (stripped under NDEBUG/Release) — use check().
void drive(Activation& act, Descriptor& d) {
    act.post(&d);
    const bool acquired = act.try_acquire();
    check(acquired, "worker acquires Scheduled->Running");
    const auto out = act.drain_step(16);
    check(out == Activation::DrainOutcome::DrainedEmpty, "drained empty");
    (void)act.close_out();
}
}  // namespace

int main() {
    // ---- Propagation across a two-hop tell chain, sampled ------------------------------------
    {
        SpanRing<64> ringA, ringB;
        Hop a;
        a.ring = &ringA;
        a.actor_key_ = 0xAAAA;
        Hop b;
        b.ring = &ringB;
        b.actor_key_ = 0xBBBB;
        Activation actA{&a, Hop::dispatch_table()};
        Activation actB{&b, Hop::dispatch_table()};

        // Origin decides sampling ONCE and bakes it into the trace_id (bit 63).
        const std::uint64_t origin = make_engine_trace_id(0xC0FFEE, /*sampled=*/true);
        check(trace_is_sampled(origin), "origin trace is sampled");
        check(trace_correlation_id(origin) == 0xC0FFEE, "correlation id preserved");

        // Hop 1: stamp the origin id onto the message and drive actor A.
        Msg m1{1};
        Descriptor d1;
        d1.payload = &m1;
        d1.trace_id = origin;  // the router stamps this (see reported post_message hook)
        stamp<Hop, Msg>(d1);
        drive(actA, d1);
        check(a.observed == origin, "hop A observes the origin trace_id (Activation copied it)");

        // Hop 2: A propagates onto a child message; drive actor B.
        Msg m2{2};
        Descriptor d2;
        d2.payload = &m2;
        d2.trace_id = trace_propagate(actA.current_context());  // == a.observed
        stamp<Hop, Msg>(d2);
        drive(actB, d2);
        check(b.observed == origin, "hop B observes the SAME trace_id after propagation");
        check(trace_is_sampled(b.observed), "sampled bit survived both hops");

        // Both hops recorded a sampled span carrying the correlation id.
        std::vector<SpanEvent> spansA, spansB;
        ringA.snapshot(spansA);
        ringB.snapshot(spansB);
        check(spansA.size() == 1 && spansA[0].trace_id == origin, "hop A recorded a span");
        check(spansB.size() == 1 && spansB[0].trace_id == origin, "hop B recorded a span");
    }

    // ---- Unsampled origin: propagates, but no spans recorded (per-message sampling avoided) ---
    {
        SpanRing<64> ring;
        Hop a;
        a.ring = &ring;
        Activation act{&a, Hop::dispatch_table()};
        const std::uint64_t origin = make_engine_trace_id(0x1234, /*sampled=*/false);
        Msg m{1};
        Descriptor d;
        d.payload = &m;
        d.trace_id = origin;
        stamp<Hop, Msg>(d);
        drive(act, d);
        check(a.observed == origin, "unsampled trace_id still propagates");
        check(ring.total_recorded() == 0, "no span recorded for an unsampled trace");
    }

    // ---- W3C traceparent round-trip ----------------------------------------------------------
    {
        const std::uint64_t tid = make_engine_trace_id(0xDEADBEEFCAFEull, /*sampled=*/true);
        const std::uint64_t span = 0x00F067AA0BA902B7ull;
        const std::string tp = format_traceparent(tid, span);
        check(tp.size() == 55, "traceparent length");
        check(tp.substr(0, 3) == "00-", "traceparent version");
        const auto parsed = parse_traceparent(tp);
        check(parsed.has_value(), "traceparent parses");
        check(parsed->span_id == span, "span id round-trips");
        check(parsed->sampled, "sampled flag round-trips");
        check(parsed->engine_trace_id() == tid, "engine trace_id round-trips (hi=0 fold is exact)");

        // A well-known W3C example parses.
        const auto ex = parse_traceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
        check(ex.has_value() && ex->sampled, "W3C example parses, sampled");
        // Malformed inputs are rejected.
        check(!parse_traceparent("garbage").has_value(), "reject short/garbage");
        check(!parse_traceparent("00-00000000000000000000000000000000-00f067aa0ba902b7-01").has_value(),
              "reject all-zero trace-id");
    }

    // ---- Sampler: ratio 1 samples all, ratio 0 samples none ----------------------------------
    {
        Sampler all{1.0};
        Sampler none{0.0};
        bool all_ok = true, none_ok = true;
        for (std::uint64_t s = 1; s <= 1000; ++s) {
            if (!all.should_sample(s)) all_ok = false;
            if (none.should_sample(s)) none_ok = false;
        }
        check(all_ok, "ratio 1.0 samples all");
        check(none_ok, "ratio 0.0 samples none");
        // A mid ratio samples SOME but not all (statistical sanity, not exact).
        Sampler half{0.5};
        int sampled = 0;
        for (std::uint64_t s = 1; s <= 1000; ++s) sampled += half.should_sample(s) ? 1 : 0;
        check(sampled > 100 && sampled < 900, "ratio 0.5 samples a fraction");
    }

    std::printf("tracing_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
