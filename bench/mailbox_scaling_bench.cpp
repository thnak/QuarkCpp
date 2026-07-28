// Mailbox producer-scaling / contention-isolation / NUMA / fairness benchmark — dimensions 8, 9,
// 15, 16, 17, 25 of the mailbox benchmark suite commissioned to evaluate the CURRENT shipped
// mailbox (include/quark/core/mailbox.hpp) and, unchanged, whatever a future design-debate round
// produces against it.
//
// Every producer thread hammers its OWN pre-allocated Descriptor array (no MessagePool, no shared
// allocator in the loop) so the SHARED contended mailbox test isolates exactly one thing: the
// `tail_.exchange` cache line (mailbox.hpp's documented hot path). This mirrors the "isolated-
// descriptor-array" methodology this session used to diagnose the known, not-yet-fixed bottleneck
// recorded in decisions/ADR-031-mailbox-mpsc-hot-path-r8-judgment.md (F4) and 002-Scheduler.md's
// "Mailbox hot-path baseline": aggregate throughput is flat/sub-linear in P and the per-producer
// average collapses, because every producer's enqueue() contends the SAME `tail_` cache line.
//
//   8) Throughput           — bench_producer_scaling's P=1 point (sustained aggregate msg/s).
//   9) Producer scaling     — bench_producer_scaling's full P-sweep, reproducing the flat/collapsing
//                             shape.
//  15) Contention            — bench_contention_isolation: the SAME P against a shared mailbox
//                             (contended) vs P independent mailboxes (uncontended control), which
//                             isolates the tail_ cache-line mechanism the way ADR-031 F4 did
//                             (P3/P1 ratio contended vs uncontended).
//  16) Same socket           — bench_numa pinned entirely within one NUMA node.
//  17) Cross socket          — bench_numa split across two NUMA nodes; delta vs #16.
//  25) Fairness              — bench_fairness: per-producer enqueue->dequeue latency distribution
//                             at fixed P; a fair design shows similar p50/p99 across producers, not
//                             one thread's messages systematically delayed behind the others.
//
// MACHINE SAFETY (hard rule, this repo/box): never saturate all cores. The default producer sweep
// is capped at P<=4 (<=5 threads total incl. the single consumer). Passing an explicit --wide flag
// (or a max-producers argv) extends the sweep to P=8 — run that ONLY pinned to a bounded core set,
// e.g.:
//   taskset -c 0-8 build/bench/mailbox_scaling_bench --wide
// NEVER `taskset -c 0-31` / unpinned-all-cores; NEVER `-j$(nproc)` anywhere in this repo's tooling.
//
// Informational only (023 §"Regression gating" applies to bench/ci_bench_gate.sh's [MISS]/[goal]
// tokens, which this file never prints) — always exits 0 regardless of the numbers measured, per
// the existing bench/mailbox_pool_partition_bench.cpp convention.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "pal/pal.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#endif

using namespace quark;

namespace {

// ---- Optional core pinning (Linux only; a no-op elsewhere) --------------------------------------
#if defined(__linux__)
void pin_thread(std::thread& th, int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(th.native_handle(), sizeof(set), &set);
}

// Parse a Linux cpulist string like "0-15" or "0-7,16-23" into individual cpu ids.
std::vector<int> parse_cpulist(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        auto dash = tok.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::atoi(tok.c_str()));
        } else {
            int lo = std::atoi(tok.substr(0, dash).c_str());
            int hi = std::atoi(tok.substr(dash + 1).c_str());
            for (int c = lo; c <= hi; ++c) out.push_back(c);
        }
    }
    return out;
}

// Read /sys/devices/system/node/nodeN/cpulist. Returns an empty vector if the node/file is absent
// (a non-NUMA box, a container without /sys, etc) — callers must handle that gracefully.
std::vector<int> numa_node_cpus(int node) {
    std::ifstream f("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist");
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    return parse_cpulist(line);
}
#else
void pin_thread(std::thread&, int) {}
std::vector<int> numa_node_cpus(int) { return {}; }
#endif

// ---- Shared building blocks --------------------------------------------------------------------

// Every producer thread's messages live in ITS OWN pre-allocated array (never the shared
// MessagePool) — the shared Mailbox's `tail_` is the ONLY thing multiple producers ever contend.
struct ProducerDescs {
    std::vector<Descriptor> descs;
    explicit ProducerDescs(std::uint64_t n) : descs(n) {}
};

struct RunResult {
    double aggregate_mps = 0.0;
    std::vector<double> per_producer_mps;   // per-producer own-message-count / wall time
};

// One shared Mailbox, P producer threads (each racing tail_.exchange), one consumer thread draining
// as fast as possible. `pin_producer_cpus`/`pin_consumer_cpu` are optional (-1 == don't pin).
RunResult run_shared_mailbox(unsigned P, std::uint64_t per_producer,
                             const std::vector<int>* pin_producer_cpus = nullptr,
                             int pin_consumer_cpu = -1) {
    std::vector<ProducerDescs> arrays;
    arrays.reserve(P);
    for (unsigned p = 0; p < P; ++p) arrays.emplace_back(per_producer);
    for (unsigned p = 0; p < P; ++p)
        for (std::uint64_t i = 0; i < per_producer; ++i)
            arrays[p].descs[i].message_id = MessageId{(static_cast<std::uint64_t>(p) << 40) | i};

    Mailbox mb;
    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0};
    std::atomic<std::uint64_t> total_drained{0};
    const std::uint64_t total_expected = static_cast<std::uint64_t>(P) * per_producer;

    std::vector<double> producer_wall_secs(P, 0.0);
    std::vector<std::thread> producers;
    producers.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin to line producers up */ }
            const auto t0 = pal::clock::now();
            for (std::uint64_t i = 0; i < per_producer; ++i) mb.enqueue(&arrays[p].descs[i]);
            const auto t1 = pal::clock::now();
            producer_wall_secs[p] = std::chrono::duration<double>(t1 - t0).count();
        });
        if (pin_producer_cpus != nullptr && p < pin_producer_cpus->size())
            pin_thread(producers.back(), (*pin_producer_cpus)[p]);
    }

    std::thread consumer([&] {
        while (total_drained.load(std::memory_order_relaxed) < total_expected) {
            DrainResult r = mb.try_dequeue();
            if (r.status == DrainStatus::Message) total_drained.fetch_add(1, std::memory_order_relaxed);
        }
    });
    if (pin_consumer_cpu >= 0) pin_thread(consumer, pin_consumer_cpu);

    while (ready.load(std::memory_order_acquire) < P) std::this_thread::yield();
    const auto wall0 = pal::clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();
    consumer.join();
    const auto wall1 = pal::clock::now();

    RunResult r;
    const double wall_secs = std::chrono::duration<double>(wall1 - wall0).count();
    r.aggregate_mps = wall_secs > 0.0 ? static_cast<double>(total_expected) / wall_secs / 1e6 : 0.0;
    r.per_producer_mps.resize(P);
    for (unsigned p = 0; p < P; ++p)
        r.per_producer_mps[p] = producer_wall_secs[p] > 0.0
                                    ? static_cast<double>(per_producer) / producer_wall_secs[p] / 1e6
                                    : 0.0;
    return r;
}

// P INDEPENDENT mailboxes (one per producer — zero tail_ contention), one consumer round-robins.
// Isolates the SAME workload shape with the contention mechanism removed (ADR-031 F4's control).
double run_independent_mailboxes(unsigned P, std::uint64_t per_producer) {
    std::vector<ProducerDescs> arrays;
    arrays.reserve(P);
    for (unsigned p = 0; p < P; ++p) arrays.emplace_back(per_producer);
    for (unsigned p = 0; p < P; ++p)
        for (std::uint64_t i = 0; i < per_producer; ++i)
            arrays[p].descs[i].message_id = MessageId{i};

    std::vector<Mailbox> boxes(P);
    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0};
    std::atomic<std::uint64_t> total_drained{0};
    const std::uint64_t total_expected = static_cast<std::uint64_t>(P) * per_producer;

    std::vector<std::thread> producers;
    producers.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (std::uint64_t i = 0; i < per_producer; ++i) boxes[p].enqueue(&arrays[p].descs[i]);
        });
    }

    std::thread consumer([&] {
        std::vector<std::uint64_t> drained_per_box(P, 0);
        while (total_drained.load(std::memory_order_relaxed) < total_expected) {
            for (unsigned p = 0; p < P; ++p) {
                if (drained_per_box[p] >= per_producer) continue;
                DrainResult r = boxes[p].try_dequeue();
                if (r.status == DrainStatus::Message) {
                    ++drained_per_box[p];
                    total_drained.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    while (ready.load(std::memory_order_acquire) < P) std::this_thread::yield();
    const auto t0 = pal::clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();
    consumer.join();
    const auto t1 = pal::clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return secs > 0.0 ? static_cast<double>(total_expected) / secs / 1e6 : 0.0;
}

// ---- Dimension 8/9: throughput + producer scaling sweep -----------------------------------------
void bench_producer_scaling(bool wide) {
    std::printf("=== [dim 8/9] throughput + producer scaling (shared mailbox, isolated descriptors) ===\n");
    constexpr std::uint64_t kTotalTarget = 8'000'000;  // roughly constant total work per P
    std::vector<unsigned> sweep = {1, 2, 4};
    if (wide) sweep.push_back(8);

    double p1_mps = 0.0;
    for (unsigned P : sweep) {
        const std::uint64_t per_producer = kTotalTarget / P;
        RunResult r = run_shared_mailbox(P, per_producer);
        if (P == 1) p1_mps = r.aggregate_mps;
        double sum = 0, mn = r.per_producer_mps.empty() ? 0 : r.per_producer_mps[0], mx = mn;
        for (double v : r.per_producer_mps) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
        const double avg_per_producer = r.per_producer_mps.empty() ? 0 : sum / static_cast<double>(P);
        std::printf("  P=%-2u  aggregate=%7.2f M msg/s   per-producer avg=%6.2f M msg/s "
                    "(min=%.2f max=%.2f)   scaling P/P1=%.2fx\n",
                    P, r.aggregate_mps, avg_per_producer, mn, mx,
                    p1_mps > 0 ? r.aggregate_mps / p1_mps : 0.0);
    }
    std::printf("  [info] flat/sub-linear aggregate + collapsing per-producer average reproduces the\n"
                "  known tail_ cache-line contention documented in ADR-031/002-Scheduler.md \"Mailbox\n"
                "  hot-path baseline\" — this is the regression signature a future producer-side\n"
                "  redesign should visibly beat, not a bug in this bench.\n");
    if (!wide)
        std::printf("  [info] pass --wide (pinned, e.g. `taskset -c 0-8 ... --wide`) to extend the sweep to P=8.\n");
}

// ---- Dimension 15: contention isolation (shared vs independent mailboxes) -----------------------
void bench_contention_isolation() {
    std::printf("\n=== [dim 15] contention isolation: shared tail_ vs independent mailboxes ===\n");
    constexpr std::uint64_t kTotalTarget = 8'000'000;
    const std::vector<unsigned> sweep = {1, 2, 4};

    double contended_p1 = 0.0, uncontended_p1 = 0.0;
    for (unsigned P : sweep) {
        const std::uint64_t per_producer = kTotalTarget / P;
        RunResult contended = run_shared_mailbox(P, per_producer);
        double uncontended = run_independent_mailboxes(P, per_producer);
        if (P == 1) { contended_p1 = contended.aggregate_mps; uncontended_p1 = uncontended; }
        std::printf("  P=%-2u  contended(shared tail_)=%7.2f M msg/s (P/P1=%.2fx)   "
                    "uncontended(independent)=%7.2f M msg/s (P/P1=%.2fx)\n",
                    P, contended.aggregate_mps,
                    contended_p1 > 0 ? contended.aggregate_mps / contended_p1 : 0.0,
                    uncontended, uncontended_p1 > 0 ? uncontended / uncontended_p1 : 0.0);
    }
    std::printf("  [info] mirrors ADR-031 F4's same-shape independent-cache-line control (measured\n"
                "  there: contended P3/P1=0.73-0.88x vs uncontended P3/P1=2.5-2.6x) — isolates the\n"
                "  mechanism (shared tail_) rather than leaving it a profiled-but-unexplained cost.\n");
}

// ---- Dimensions 16/17: same-socket vs cross-socket NUMA placement --------------------------------
void bench_numa() {
    std::printf("\n=== [dim 16/17] same-socket vs cross-socket NUMA placement ===\n");
    std::vector<int> node0 = numa_node_cpus(0);
    std::vector<int> node1 = numa_node_cpus(1);
    if (node0.size() < 5 || node1.empty()) {
        std::printf("  [skip] fewer than 2 NUMA nodes detected (or node0 has <5 cpus) via "
                    "/sys/devices/system/node/ — this box/container is not multi-socket "
                    "(or /sys is unavailable); nothing to compare.\n");
        return;
    }

    constexpr unsigned P = 4;
    constexpr std::uint64_t per_producer = 2'000'000;

    // Same-socket: 4 producers + 1 consumer, all pinned within node0.
    {
        std::vector<int> producer_cpus(node0.begin(), node0.begin() + P);
        int consumer_cpu = node0[P];  // a 5th distinct core on the same node
        RunResult r = run_shared_mailbox(P, per_producer, &producer_cpus, consumer_cpu);
        std::printf("  same-socket  (node0 cpus %d..%d, producers+consumer)  : %7.2f M msg/s\n",
                    node0.front(), node0.back(), r.aggregate_mps);
    }
    // Cross-socket: producers split node0/node1, consumer on node0.
    {
        std::vector<int> producer_cpus;
        for (unsigned p = 0; p < P; ++p)
            producer_cpus.push_back(p % 2 == 0 ? node0[p / 2] : node1[p / 2]);
        int consumer_cpu = node0[P / 2 + 1 < node0.size() ? P / 2 + 1 : 0];
        RunResult r = run_shared_mailbox(P, per_producer, &producer_cpus, consumer_cpu);
        std::printf("  cross-socket (producers split node0/node1, consumer node0) : %7.2f M msg/s\n",
                    r.aggregate_mps);
    }
    std::printf("  [info] this session's earlier same-box measurement: same-socket P=4 12.51 M/s vs\n"
                "  cross-socket P=4 10.37 M/s — expect a same-direction delta here, not identical\n"
                "  numbers (different run, different moment-in-time system load).\n");
}

// ---- Dimension 25: fairness — per-producer latency spread at fixed P -----------------------------
void bench_fairness() {
    std::printf("\n=== [dim 25] fairness: per-producer enqueue->dequeue latency spread ===\n");
    // NOTE: producers are UNTHROTTLED (no backpressure — the mailbox is unbounded by design, 003
    // §Backpressure is a separate, higher layer), so a real, expected queueing backlog forms when 4
    // producers' combined enqueue rate outruns the single consumer's drain rate; the printed
    // absolute p50/p99 therefore reflect real (sustained-flood) queueing delay, not the ~10-100 ns
    // single-message enqueue cost the isolated latency bench (mailbox_bench.cpp /
    // mailbox_enqueue_latency_bench.cpp) measures. That is fine and expected for THIS dimension:
    // fairness asks whether ONE producer is systematically delayed *relative to the others*, i.e.
    // the SPREAD across producers, not the absolute magnitude, which this backlog-under-flood
    // methodology still measures correctly (every producer's messages sit in the same one queue).
    constexpr unsigned P = 4;
    constexpr std::uint64_t per_producer = 20'000;

    std::vector<ProducerDescs> arrays;
    arrays.reserve(P);
    for (unsigned p = 0; p < P; ++p) arrays.emplace_back(per_producer);
    for (unsigned p = 0; p < P; ++p)
        for (std::uint64_t i = 0; i < per_producer; ++i)
            arrays[p].descs[i].message_id = MessageId{(static_cast<std::uint64_t>(p) << 40) | i};

    Mailbox mb;
    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0};
    std::atomic<std::uint64_t> total_drained{0};
    const std::uint64_t total_expected = static_cast<std::uint64_t>(P) * per_producer;

    std::vector<std::thread> producers;
    producers.reserve(P);
    for (unsigned p = 0; p < P; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (std::uint64_t i = 0; i < per_producer; ++i) {
                // Stash the enqueue timestamp in the otherwise-unused deadline_ns scratch field —
                // the consumer reads it back on dequeue to compute pure transit latency.
                arrays[p].descs[i].deadline_ns =
                    pal::clock::now().time_since_epoch().count();
                mb.enqueue(&arrays[p].descs[i]);
            }
        });
    }

    std::vector<std::vector<double>> per_producer_latency_ns(P);
    for (auto& v : per_producer_latency_ns) v.reserve(per_producer);

    std::thread consumer([&] {
        while (total_drained.load(std::memory_order_relaxed) < total_expected) {
            DrainResult r = mb.try_dequeue();
            if (r.status != DrainStatus::Message) continue;
            const auto now_ns = pal::clock::now().time_since_epoch().count();
            const std::uint64_t id = r.desc->message_id.value;
            const unsigned p = static_cast<unsigned>(id >> 40);
            const double lat_ns = static_cast<double>(now_ns - r.desc->deadline_ns);
            if (p < P) per_producer_latency_ns[p].push_back(lat_ns);
            total_drained.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (ready.load(std::memory_order_acquire) < P) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : producers) th.join();
    consumer.join();

    double min_p50 = -1, max_p50 = -1, min_p99 = -1, max_p99 = -1;
    for (unsigned p = 0; p < P; ++p) {
        bench::Stats s = bench::summarize(per_producer_latency_ns[p]);
        std::printf("  producer[%u]  p50=%8.1f ns   p99=%8.1f ns   p999=%8.1f ns   (n=%zu)\n",
                    p, s.p50, s.p99, s.p999, s.n);
        if (min_p50 < 0 || s.p50 < min_p50) min_p50 = s.p50;
        if (s.p50 > max_p50) max_p50 = s.p50;
        if (min_p99 < 0 || s.p99 < min_p99) min_p99 = s.p99;
        if (s.p99 > max_p99) max_p99 = s.p99;
    }
    const double p50_spread_pct = min_p50 > 0 ? 100.0 * (max_p50 - min_p50) / min_p50 : 0.0;
    const double p99_spread_pct = min_p99 > 0 ? 100.0 * (max_p99 - min_p99) / min_p99 : 0.0;
    std::printf("  spread: p50 %.1f%%  p99 %.1f%%  (across %u producers; a fair design keeps this\n"
                "  small — no producer's messages are systematically delayed behind the others)\n",
                p50_spread_pct, p99_spread_pct, P);
}

}  // namespace

int main(int argc, char** argv) {
    bool wide = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--wide") == 0) wide = true;

    std::printf("== Quark mailbox scaling/contention/NUMA/fairness bench (dims 8,9,15,16,17,25) ==\n");
    std::printf("(pin e.g. `taskset -c 0-3 build/bench/mailbox_scaling_bench`; --wide extends P to 8,\n");
    std::printf(" pin wider e.g. `taskset -c 0-8 ... --wide` — never unpinned-all-cores)\n\n");

    bench_producer_scaling(wide);
    bench_contention_isolation();
    bench_numa();
    bench_fairness();
    return 0;
}
