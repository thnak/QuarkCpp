// Tests ADR-054 / 009 §Cardinality — per-actor-type handler busy-time + message bytes.
//   Part 1: registry-level aggregation across shards (flat per-shard cells summed per slot at snapshot,
//           slot cap, Prometheus rows) — no Engine.
//   Part 2: the REAL Engine — busy_ns > 0 for a busy type and ~0-by-comparison for an idle one, bytes
//           attributed EXACTLY to the receiving type (distinct payload sizes), multi-shard aggregation.
// Compiled out (and skipped) under -DQUARK_NO_TYPE_ACCOUNTING.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metrics.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

struct BusyMsg {
    std::uint64_t pad[4];  // 32 B payload
};
struct LightMsg {
    std::uint32_t v;  // 4 B payload
};

struct BusyActor : Actor<BusyActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.test.metrics_type_accounting_test.BusyActor";
    using protocol = Protocol<BusyMsg>;
    void handle(const BusyMsg&) noexcept {
        const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
        while (std::chrono::steady_clock::now() < end) {}  // genuine handler CPU time
        ++n;
    }
    std::uint64_t n = 0;
};
struct LightActor : Actor<LightActor, Sequential> {
    static constexpr std::string_view quark_type_name = "quark.test.metrics_type_accounting_test.LightActor";
    using protocol = Protocol<LightMsg>;
    void handle(const LightMsg&) noexcept { ++n; }
    std::uint64_t n = 0;
};

const TypeMetric* find(const MetricsSnapshot& s, TypeKey k) {
    for (const TypeMetric& t : s.types)
        if (t.key == k) return &t;
    return nullptr;
}
}  // namespace

int main() {
#ifdef QUARK_NO_TYPE_ACCOUNTING
    std::printf("metrics_type_accounting_test: SKIP (QUARK_NO_TYPE_ACCOUNTING)\n");
    return 0;
#else
    // ---- Part 1: aggregation across shards, slot cap, Prometheus ------------------------------
    {
        MetricsRegistry reg;
        ShardCounters& a = reg.add_shard();
        ShardCounters& b = reg.add_shard();
        a.init_type_cells(2);
        b.init_type_cells(2);
        const TypeKey k0{0xAAAA}, k1{0xBBBB}, k2{0xCCCC};
        const int s0 = reg.assign_type_slot(k0, "ns::Zero", 2);
        const int s1 = reg.assign_type_slot(k1, "", 2);
        check(s0 == 0 && s1 == 1, "dense slots assigned in order");
        check(reg.assign_type_slot(k0, "", 2) == 0, "find-or-assign is idempotent");
        check(reg.assign_type_slot(k2, "x", 2) == -1, "table full => -1 (no accounting, no error)");
        a.type_cell(0)->busy_ns.inc(100);
        b.type_cell(0)->busy_ns.inc(250);
        a.type_cell(0)->bytes.inc(10);
        b.type_cell(1)->bytes.inc(7);
        b.type_cell(1)->bytes.inc(8);
        check(a.type_cell(2) == nullptr, "out-of-range cell is null");
        const MetricsSnapshot s = reg.snapshot();
        check(s.types.size() == 2, "one row per assigned slot");
        check(s.types[0].busy_ns == 350 && s.types[0].bytes == 10, "slot 0 summed across shards");
        check(s.types[1].busy_ns == 0 && s.types[1].bytes == 15, "slot 1 summed across shards");
        check(s.types[0].name == "ns::Zero", "name carried");
        const std::string p = reg.to_prometheus();
        check(p.find("quark_actor_type_handler_busy_ns_total{type_key=\"0x000000000000aaaa\",type=\"ns::Zero\"} 350") !=
                  std::string::npos,
              "prometheus busy row");
        check(p.find("quark_actor_type_message_bytes_total{type_key=\"0x000000000000bbbb\"} 15") != std::string::npos,
              "prometheus bytes row (no name label when unknown)");
    }

    // ---- Part 2: real Engine -----------------------------------------------------------------
    {
        constexpr std::uint32_t kActors = 4;       // spread over 2 shards
        constexpr std::uint32_t kBusyPer = 25;     // 25 * 200us = 5 ms busy per actor
        constexpr std::uint32_t kLightPer = 100;
        detail::MessagePool pool(4096);
        Engine<> eng(EngineConfig{2, 2, 64, 64});
        for (std::uint32_t k = 0; k < kActors; ++k) {
            check(eng.spawn<BusyActor>(k, pool.sink()).has_value(), "spawn busy");
            check(eng.spawn<LightActor>(k, pool.sink()).has_value(), "spawn light");
        }
        LocalRouter router(eng.post_courier(), pool);
        eng.start();
        for (std::uint32_t k = 0; k < kActors; ++k) {
            auto br = router.get<BusyActor>(k);
            auto lr = router.get<LightActor>(k);
            for (std::uint32_t i = 0; i < kBusyPer; ++i) br.tell(BusyMsg{});
            for (std::uint32_t i = 0; i < kLightPer; ++i) lr.tell(LightMsg{1});
        }
        eng.stop();  // drains everything

        const MetricsSnapshot s = eng.metrics_snapshot();
        const TypeMetric* bt = find(s, type_key_of<BusyActor>());
        const TypeMetric* lt = find(s, type_key_of<LightActor>());
        check(bt != nullptr && lt != nullptr, "both types present in snapshot");
        if (bt && lt) {
            const std::uint64_t kBusyN = std::uint64_t{kActors} * kBusyPer;
            const std::uint64_t kLightN = std::uint64_t{kActors} * kLightPer;
            check(bt->bytes == kBusyN * (sizeof(BusyMsg) + sizeof(Descriptor)),
                  "busy-type bytes attributed exactly to BusyActor");
            check(lt->bytes == kLightN * (sizeof(LightMsg) + sizeof(Descriptor)),
                  "light-type bytes attributed exactly to LightActor");
            // 100 handler runs * 200us = 20 ms of real handler time summed across shards/actors.
            check(bt->busy_ns >= 15'000'000ull, "busy type busy_ns reflects real handler time (>=15ms of 20ms)");
            check(lt->busy_ns < bt->busy_ns, "light type busy_ns < busy type busy_ns");
            std::printf("  busy: %llu ns, %llu B | light: %llu ns, %llu B\n",
                        static_cast<unsigned long long>(bt->busy_ns), static_cast<unsigned long long>(bt->bytes),
                        static_cast<unsigned long long>(lt->busy_ns), static_cast<unsigned long long>(lt->bytes));
            check(!bt->name.empty() && bt->name.find("BusyActor") != std::string::npos, "spawn<A> supplies the type name");
        }
        check(eng.metrics_prometheus().find("quark_actor_type_handler_busy_ns_total{") != std::string::npos,
              "engine prometheus exports per-type busy series");
    }

    std::printf("metrics_type_accounting_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
#endif
}
