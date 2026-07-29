// Tests 009-Observability §"Histogram bucket layout and cardinality" (ADR-022) — the CLOSED-FORM
// `bucket_index_of` (the `{shift, step_shift}` technique) against a brute-force LINEAR scan over
// `Spec::boundaries()` (the exact `while (v > b[i]) ++i;` shape the ADR names as the thing being
// replaced), for TWO independently-parameterized HistogramSpecs, across a wide value sweep:
//   * exhaustive over small values,
//   * every physical bucket boundary +/- a few,
//   * a geometric sweep out past 2^62 with several offsets per step.
// Both code paths are computed independently (one an O(bucket_count) scan, one O(1) shift/mask
// math) from the SAME {shift, step_shift} parameters — agreement is a real, non-circular proof that
// the closed form matches the declared bucket semantics, not just internal self-consistency.
#include <array>
#include <cstdint>
#include <cstdio>

#include "quark/core/histogram_spec.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

// The brute-force oracle: EXACTLY the `while (v > b[i]) ++i;` shape ADR-022 names.
template <std::size_t N>
std::size_t linear_scan(std::uint64_t v, const std::array<std::uint64_t, N>& b) {
    std::size_t i = 0;
    while (i + 1 < N && v > b[i]) ++i;
    return i;
}

template <class Spec>
void run_spec(const char* name) {
    const auto b = Spec::boundaries();
    // Boundaries are non-decreasing (a well-formed bucket layout).
    for (std::size_t i = 1; i < Spec::bucket_count; ++i)
        check(b[i] >= b[i - 1], "boundaries() is non-decreasing");
    check(b[Spec::bucket_count - 1] == ~std::uint64_t{0}, "last boundary catches everything above");

    std::uint64_t total = 0, mismatches = 0;

    // Exhaustive over a wide low range.
    for (std::uint64_t v = 0; v < 500'000ULL; ++v) {
        const std::size_t closed = bucket_index_of<Spec::shift, Spec::step_shift, Spec::bucket_count>(v);
        const std::size_t lin = linear_scan<Spec::bucket_count>(v, b);
        ++total;
        if (closed != lin) ++mismatches;
    }

    // Every boundary +/- a few (the discontinuities are where a bug would show up).
    for (std::size_t i = 0; i < Spec::bucket_count; ++i) {
        for (std::int64_t d = -3; d <= 3; ++d) {
            const std::int64_t vv = static_cast<std::int64_t>(b[i]) + d;
            if (vv < 0) continue;
            const std::uint64_t v = static_cast<std::uint64_t>(vv);
            const std::size_t closed =
                bucket_index_of<Spec::shift, Spec::step_shift, Spec::bucket_count>(v);
            const std::size_t lin = linear_scan<Spec::bucket_count>(v, b);
            ++total;
            if (closed != lin) ++mismatches;
        }
    }

    // Geometric sweep out to the top of the range (values well beyond any realistic latency/depth).
    for (std::uint64_t v = 1; v != 0; v = (v << 1) | 1) {
        for (std::uint64_t off = 0; off < 16; ++off) {
            const std::uint64_t vv = v + off * 1'000'003ULL;
            const std::size_t closed =
                bucket_index_of<Spec::shift, Spec::step_shift, Spec::bucket_count>(vv);
            const std::size_t lin = linear_scan<Spec::bucket_count>(vv, b);
            ++total;
            if (closed != lin) ++mismatches;
        }
        if (v & (std::uint64_t{1} << 62)) break;
    }

    std::printf("  %s: %llu checks, %llu mismatches\n", name, static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(mismatches));
    check(mismatches == 0, "closed-form bucket_index_of matches brute-force linear scan");
}
}  // namespace

int main() {
    run_spec<LatencyNsSpec>("LatencyNsSpec(shift=4,step_shift=4,n=48)");
    run_spec<MailboxDepthSpec>("MailboxDepthSpec(shift=0,step_shift=3,n=32)");

    // Validation-time cap: a well-formed Spec's bucket_count must be within [1, kHistogramSpecBucketCap].
    static_assert(LatencyNsSpec::bucket_count <= kHistogramSpecBucketCap);
    static_assert(MailboxDepthSpec::bucket_count <= kHistogramSpecBucketCap);
    check(HistogramBlock<LatencyNsSpec>::kBuckets == LatencyNsSpec::bucket_count, "block bucket count");

    // record() lands values in the same bucket the closed form predicts, end to end.
    {
        HistogramBlock<LatencyNsSpec> h;
        h.record(0);
        h.record(1'000'000);
        h.record(5);
        const auto snap = h.snapshot();
        check(snap.count == 3, "record/snapshot count");
        check(snap.sum == 1'000'005, "record/snapshot sum");
        check(snap.min == 0, "record/snapshot min");
        check(snap.max == 1'000'000, "record/snapshot max");
        const std::size_t b0 = bucket_index_of<LatencyNsSpec::shift, LatencyNsSpec::step_shift,
                                                 LatencyNsSpec::bucket_count>(0);
        check(snap.buckets[b0] >= 1, "value 0 landed in the predicted bucket");
    }

    std::printf("metrics_histogram_spec_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
