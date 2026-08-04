// Implements ADR-040 (Phase 2) — proves the F2-revised claim on THIS codebase's own PeerSession/
// SecureTransport (not just the ADR record's standalone prototype): AEAD seal never runs while
// holding a lock shared with another peer's session, so (a) per-op send() latency does not grow with
// the number of live sessions, and (b) throughput to N DISJOINT peers scales with thread count instead
// of flattening at one peer's worth of work.
//
// No 023 Hard/Goal budget exists yet for this new subsystem (023-Performance-Targets-and-Budgets.md
// has not been updated for ADR-040) — this bench prints raw percentiles/throughput informationally
// (no [goal]/[hard]/[MISS]/[floor] tokens), so it is NOT graded by bench/ci_bench_gate.sh. Establishing
// a formal budget is a documented follow-up (see decisions/ADR-040 spec recommendations), not invented
// here. MACHINE SAFETY: single-thread latency runs pinned `taskset -c 0`; the multi-peer-parallelism
// section uses at most 4 threads (the repo cap) — `taskset -c 0-3`.
#include <cstdio>
#include <thread>
#include <vector>

#include "bench_harness.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/core/transport.hpp"

using namespace quark;
using namespace quark::bench;

namespace {
constexpr int kWarmup = 1'000;

// --- (a) Single-thread send() latency, steady state, ONE warm session. --------------------------
void bench_send_latency() {
    LoopbackFabric fabric;
    MockCipher cipher(1);
    const NodeId self{1}, peer{2};
    // `peer` has no attached receiver — frames drop silently at the fabric (fire-and-forget), which
    // is fine: this measures the SENDER'S send() cost, not delivery.
    LoopbackTransport n1_inner(fabric, self);
    SecureTransport secure(n1_inner, cipher, self);

    MessageFrame f{};
    f.from = self;
    f.to = peer;
    f.target = ActorId{TypeKey{1}, 1};
    f.msg_type = TypeKey{2};
    f.payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    for (int i = 0; i < kWarmup; ++i) secure.send(peer, f);

    constexpr int kIters = 50'000;
    std::vector<double> samples;
    samples.reserve(kIters);
    for (int i = 0; i < kIters; ++i) {
        const auto t0 = pal::clock::now();
        secure.send(peer, f);
        const auto t1 = pal::clock::now();
        samples.push_back(ns_between(t0, t1));
    }
    const Stats s = summarize(samples);
    std::printf("secure_transport_send (1 warm session, n=%zu)\n", s.n);
    std::printf("  p50=%.1fns p99=%.1fns p999=%.1fns mean=%.1fns stddev=%.1fns CoV=%.3f\n", s.p50, s.p99,
               s.p999, s.mean, s.stddev, s.cov);
}

// --- (b) Single-thread throughput, round-robin across K peers: does session COUNT matter? --------
double throughput_over_k_peers(int k) {
    LoopbackFabric fabric;
    MockCipher cipher(2);
    const NodeId self{100};
    LoopbackTransport n1_inner(fabric, self);
    SecureTransport secure(n1_inner, cipher, self);

    MessageFrame f{};
    f.from = self;
    f.target = ActorId{TypeKey{1}, 1};
    f.msg_type = TypeKey{2};
    f.payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    for (int i = 0; i < kWarmup; ++i) secure.send(NodeId{static_cast<std::uint64_t>(200 + (i % k))}, f);

    constexpr int kIters = 200'000;
    const auto t0 = pal::clock::now();
    for (int i = 0; i < kIters; ++i) secure.send(NodeId{static_cast<std::uint64_t>(200 + (i % k))}, f);
    const auto t1 = pal::clock::now();
    const double secs = ns_between(t0, t1) / 1e9;
    return static_cast<double>(kIters) / secs / 1e6;  // M ops/s
}

void bench_multipeer_throughput() {
    const double mps8 = throughput_over_k_peers(8);
    const double mps32 = throughput_over_k_peers(32);
    const double ratio = mps32 / mps8;
    std::printf("secure_transport_send throughput vs session count\n");
    std::printf("  8 peers:  %8.2f M ops/s\n", mps8);
    std::printf("  32 peers: %8.2f M ops/s\n", mps32);
    std::printf("  ratio(32/8) = %.3f  %s (session-count-independent lookup: ratio near 1.0)\n", ratio,
               ratio >= 0.85 ? "[flat]" : "[degraded]");
}

// --- (c) Cross-peer parallelism: N threads, EACH its own dedicated peer, sending concurrently. ----
// If AEAD seal/open ever ran under a lock shared across peers, this would flatten near 1x a single
// thread's throughput regardless of thread count; per-session locking (the required fix + F2-revised)
// predicts near-linear scaling instead.
void bench_cross_peer_parallel_throughput() {
    LoopbackFabric fabric;
    MockCipher cipher(3);
    const NodeId self{1000};
    LoopbackTransport n1_inner(fabric, self);
    SecureTransport secure(n1_inner, cipher, self);

    constexpr int kThreads = 4;  // machine-safety cap
    constexpr int kItersPerThread = 200'000;

    MessageFrame f{};
    f.from = self;
    f.target = ActorId{TypeKey{1}, 1};
    f.msg_type = TypeKey{2};
    f.payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

    // Warm up one dedicated session per thread before timing.
    for (int t = 0; t < kThreads; ++t) secure.send(NodeId{static_cast<std::uint64_t>(2000 + t)}, f);

    std::vector<std::thread> threads;
    const auto t0 = pal::clock::now();
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const NodeId peer{static_cast<std::uint64_t>(2000 + t)};
            for (int i = 0; i < kItersPerThread; ++i) secure.send(peer, f);
        });
    }
    for (auto& th : threads) th.join();
    const auto t1 = pal::clock::now();
    const double secs = ns_between(t0, t1) / 1e9;
    const double total_mps = (static_cast<double>(kThreads) * kItersPerThread) / secs / 1e6;

    std::printf("secure_transport_send cross-peer parallel throughput (%d threads, disjoint peers)\n",
               kThreads);
    std::printf("  aggregate = %8.2f M ops/s over %.3fs (no cross-peer lock contention expected)\n",
               total_mps, secs);
}
}  // namespace

int main() {
    bench_send_latency();
    bench_multipeer_throughput();
    bench_cross_peer_parallel_throughput();
    return 0;
}
