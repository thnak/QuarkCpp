// Tests 009-Observability §Deadline accounting — 009 accounts/reports deadline overruns (the
// deadline MODEL is 018's; 009 only compares MessageContext.deadline_ns to now and records). A miss
// bumps the shard `deadline_misses` counter and emits a sampled DeadlineMiss span; a message with no
// deadline (deadline_ns == 0) or one still in the future is never a miss.
#include <cassert>
#include <cstdio>
#include <vector>

#include "quark/core/observability.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
}  // namespace

int main() {
    // deadline_expired: only fires for a set deadline that has passed.
    {
        MessageContext none;  // deadline_ns == 0
        check(!deadline_expired(none, 1'000'000), "no deadline => never expired");
        MessageContext future;
        future.deadline_ns = 1'000'000;
        check(!deadline_expired(future, 999'999), "future deadline not yet expired");
        check(deadline_expired(future, 1'000'000), "deadline expired at the instant");
        check(deadline_expired(future, 2'000'000), "deadline expired past it");
    }

    // account_deadline_miss: bumps the counter + emits a sampled span.
    {
        ShardCounters sc;
        SpanRing<32> ring;
        MessageContext ctx;
        ctx.deadline_ns = 500;
        ctx.trace_id = make_engine_trace_id(0x77, /*sampled=*/true);

        const std::int64_t now = 900;  // past the deadline
        check(deadline_expired(ctx, now), "expired precondition");
        account_deadline_miss(sc, ring, ctx, /*actor=*/0xA1, /*msg_slot=*/2, now);

        check(sc.deadline_misses.load() == 1, "deadline_misses counter bumped");
        std::vector<SpanEvent> spans;
        ring.snapshot(spans);
        check(spans.size() == 1, "one deadline-miss span emitted");
        if (!spans.empty()) {
            check(spans[0].outcome == SpanOutcome::DeadlineMiss, "span outcome is DeadlineMiss");
            check(spans[0].trace_id == ctx.trace_id, "span carries the trace_id");
            check(spans[0].t_start_ns == 500 && spans[0].t_end_ns == 900, "span records deadline vs now");
        }
    }

    // An UNSAMPLED miss still counts, but records no span (per-message sampling avoided).
    {
        ShardCounters sc;
        SpanRing<32> ring;
        MessageContext ctx;
        ctx.deadline_ns = 500;
        ctx.trace_id = make_engine_trace_id(0x77, /*sampled=*/false);
        account_deadline_miss(sc, ring, ctx, 0, 0, 900);
        check(sc.deadline_misses.load() == 1, "unsampled miss still counted");
        check(ring.total_recorded() == 0, "no span for an unsampled miss");
    }

    // steady_now_ns is monotonic and on the same basis as deadline_ns (018 seam).
    {
        const std::int64_t t0 = steady_now_ns();
        const std::int64_t t1 = steady_now_ns();
        check(t1 >= t0, "steady_now_ns monotonic");
    }

    std::printf("deadline_accounting_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
