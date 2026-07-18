// Zero-cost proof for the 007/ADR-009 handler-boundary guard (F1/F2). Measures the Sequential drain
// success path — post → drain_step(1) → sync handler → complete/reclaim — per message, p50/p99/p999.
//
// Build TWICE from this one source:
//   * default            → the guard is compiled IN (try/catch around the thunk dispatch);
//   * -DQUARK_SUPERVISION_NO_GUARD → the CONTROL, guard compiled OUT.
// The claim (ADR-009 F1): guarded p99 is within noise of the control (ratio ≤ ~1.02), well under the
// 023 100 ns local-tell goal. The handler NEVER throws here — this isolates the guard's success cost.
//
// Pin it: `taskset -c 0 build/bench/supervision_bench` — single-thread latency, one core.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

struct Tick {
    std::uint32_t x;
};

struct Counter : Actor<Counter, Sequential> {
    using protocol = Protocol<Tick>;
    std::uint64_t sum = 0;
    void handle(const Tick& t) noexcept { sum += t.x; }  // trivial, never throws
};

}  // namespace

int main() {
    constexpr std::uint64_t kWarmup = 200'000;
    constexpr std::uint64_t kSamples = 2'000'000;

    Counter actor;
    Activation act{&actor, Counter::dispatch_table()};  // Sequential, default supervision
    (void)act.try_acquire();  // own the lane for the whole run (single-executor)

    Tick msg{1};
    Descriptor d;
    d.payload = &msg;
    stamp<Counter, Tick>(d);

    std::vector<double> ns;
    ns.reserve(kSamples);

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        act.post(&d);  // enqueue + producer fence + exec notify (self-owned ⇒ no wake)
        const auto t0 = std::chrono::steady_clock::now();
        const Activation::DrainOutcome o = act.drain_step(1);  // dispatch ONE message through the guard
        const auto t1 = std::chrono::steady_clock::now();
        if (o != Activation::DrainOutcome::DrainedEmpty && o != Activation::DrainOutcome::BudgetExhausted) {
            std::fprintf(stderr, "bench: unexpected drain outcome\n");
            return 1;
        }
        if (i >= kWarmup)
            ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    const double p50 = percentile(ns, 0.50);
    const double p99 = percentile(ns, 0.99);
    const double p999 = percentile(ns, 0.999);
#ifdef QUARK_SUPERVISION_NO_GUARD
    const char* mode = "control (no guard)";
#else
    const char* mode = "guarded";
#endif
    std::printf("supervision_bench [%s]  p50=%.2f ns  p99=%.2f ns  p999=%.2f ns  (checksum=%llu)\n",
                mode, p50, p99, p999, static_cast<unsigned long long>(actor.sum));
    return 0;
}
