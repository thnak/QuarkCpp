// ADR-054 / 023 gate: overhead of per-actor-type busy-time + bytes accounting (009 metrics).
// Built TWICE (see bench/CMakeLists.txt): `type_accounting_bench` (accounting ON, the default) and
// `type_accounting_bench_off` (-DQUARK_NO_TYPE_ACCOUNTING). Run each pinned to ONE core and compare the
// reported ns/message. Method: N tells are pre-enqueued onto ONE actor before the engine starts, then a
// single worker drains them end to end (start -> last handled) — the full drain path, incl. one
// clock-read pair per run_activation call (budget msgs). Repeated, median reported.
//   taskset -c 0 type_accounting_bench ; taskset -c 0 type_accounting_bench_off
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {
struct Tick {
    std::uint64_t v;
};
std::atomic<std::uint64_t> g_handled{0};
struct Sink : Actor<Sink, Sequential> {
    [[maybe_unused]] static constexpr std::string_view quark_type_name = "quark.bench.type_accounting_bench.Sink";
    using protocol = Protocol<Tick>;
    void handle(const Tick& t) noexcept {
        sum += t.v;
        g_handled.fetch_add(1, std::memory_order_relaxed);  // same in both builds; test-harness only
    }
    std::uint64_t sum = 0;
};

double run_once(std::uint64_t n, std::uint32_t budget) {
    g_handled.store(0);
    detail::MessagePool pool(static_cast<std::size_t>(n) + 64);
    Engine<> eng(EngineConfig{1, 1, budget, 64});
    (void)eng.spawn<Sink>(1, pool.sink());
    LocalRouter router(eng.post_courier(), pool);
    auto ref = router.get<Sink>(1);
    for (std::uint64_t i = 0; i < n; ++i) ref.tell(Tick{i});
    const auto t0 = std::chrono::steady_clock::now();
    eng.start();
    while (g_handled.load(std::memory_order_relaxed) < n) std::this_thread::yield();
    const auto t1 = std::chrono::steady_clock::now();
    eng.stop();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}
}  // namespace

int main() {
#ifdef QUARK_NO_TYPE_ACCOUNTING
    const char* mode = "OFF (QUARK_NO_TYPE_ACCOUNTING)";
#else
    const char* mode = "ON";
#endif
    constexpr std::uint64_t kN = 200'000;
    constexpr int kReps = 15;
    for (std::uint32_t budget : {64u, 8u}) {
        std::vector<double> s;
        (void)run_once(kN, budget);  // warmup
        for (int r = 0; r < kReps; ++r) s.push_back(run_once(kN, budget));
        std::sort(s.begin(), s.end());
        std::printf("type_accounting[%s] budget=%-3u  ns/msg  min=%.1f p50=%.1f max=%.1f  (n=%d x %llu msgs)\n",
                    mode, budget, s.front(), s[s.size() / 2], s.back(), kReps,
                    static_cast<unsigned long long>(kN));
    }
    return 0;
}
