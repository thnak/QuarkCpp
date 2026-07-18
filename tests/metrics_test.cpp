// Tests 009-Observability §Metrics — per-shard single-writer counters aggregate correctly on
// scrape, INCLUDING under concurrent producers (each thread owns a shard, ≤4 threads per the
// machine-safety cap). Also checks the histogram bucketing and the Prometheus text exposition.
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/metrics.hpp"

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
    // ---- Single-shard correctness ------------------------------------------------------------
    {
        ShardCounters sc;
        for (int i = 0; i < 1000; ++i) sc.messages_processed.inc();
        sc.dead_letters.inc(5);
        check(sc.messages_processed.load() == 1000, "single-writer counter counts");
        check(sc.dead_letters.load() == 5, "inc(n) adds n");

        // histogram: value 1 -> bucket 1, value 8 (0b1000) -> bucket 4, value 0 -> bucket 0.
        check(Histogram::bucket_of(0) == 0, "hist bucket of 0");
        check(Histogram::bucket_of(1) == 1, "hist bucket of 1");
        check(Histogram::bucket_of(8) == 4, "hist bucket of 8");
        sc.message_latency_ns.record(100);
        sc.message_latency_ns.record(200);
        sc.message_latency_ns.record(300);
        const HistogramSnapshot h = sc.message_latency_ns.snapshot();
        check(h.count == 3, "hist count");
        check(h.sum == 600, "hist sum");
        check(h.min == 100, "hist min");
        check(h.max == 300, "hist max");
    }

    // ---- Concurrent producers: 4 shards, one thread each, aggregate-on-read ------------------
    {
        constexpr int kThreads = 4;   // machine-safety cap (≤4)
        constexpr int kPerThread = 250'000;
        MetricsRegistry reg;
        std::vector<ShardCounters*> shards;
        shards.reserve(static_cast<std::size_t>(kThreads));
        for (int t = 0; t < kThreads; ++t) shards.push_back(&reg.add_shard());

        std::vector<std::thread> ts;
        ts.reserve(static_cast<std::size_t>(kThreads));
        for (int t = 0; t < kThreads; ++t) {
            ShardCounters* sc = shards[static_cast<std::size_t>(t)];
            ts.emplace_back([sc] {
                for (int i = 0; i < kPerThread; ++i) {
                    sc->messages_processed.inc();       // hot event
                    sc->mailbox_enqueued.inc();
                    sc->message_latency_ns.record(static_cast<std::uint64_t>((i % 512) + 1));
                    sc->user[0].inc();
                }
            });
        }
        for (auto& th : ts) th.join();

        const MetricsSnapshot s = reg.snapshot();
        const std::uint64_t expected = static_cast<std::uint64_t>(kThreads) * kPerThread;
        check(s.messages_processed == expected, "concurrent messages_processed aggregate exact");
        check(s.mailbox_enqueued == expected, "concurrent mailbox_enqueued aggregate exact");
        check(s.user[0] == expected, "concurrent user counter aggregate exact");
        check(s.message_latency_ns.count == expected, "concurrent histogram count aggregate exact");
        check(reg.shard_count() == kThreads, "shard count");

        // Prometheus exposition is pure string formatting over the snapshot (off hot path).
        const std::string prom = reg.to_prometheus();
        check(prom.find("quark_messages_processed_total ") != std::string::npos,
              "prometheus emits messages_processed");
        check(prom.find("quark_message_latency_ns_count ") != std::string::npos,
              "prometheus emits histogram count");
    }

    std::printf("metrics_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
