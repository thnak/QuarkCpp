// Sustained-duration stress/survivability test for the WHOLE engine under continuous production-
// like load — paired producer/actor lanes (M producers -> M actors, one dedicated shard/worker
// each), not a single hammered mailbox.
//
// An earlier version of this file used quark_mpsc_bench.cpp's N-producers-to-1-actor shared
// mailbox topology, sustained instead of bursty. That's an artificial worst case — no real
// production workload routes every message to the same single actor identity — and it made the
// dev box unresponsive at 8 producers (backlog ballooned to ~18GB RSS against one overwhelmed
// consumer). This version instead mirrors quark_mpsc_sharded_bench.cpp's topology: producer[i]
// only ever talks to actor[i], each actor on its own worker/shard, so total consumption capacity
// scales with the pair count instead of being capped by a single mailbox — the shape an actual
// engine sees when load is spread across many actors, and the shape that lets this bench actually
// measure sustained ENGINE throughput rather than one contention pathology.
//
// SAFETY (kept from the incident above, defense in depth even though this topology shouldn't hit
// it the same way): the producer-stop decision is driven PURELY by a cheap wall-clock check on
// this bench's own atomics — it never calls ask() while producers might still be flooding. Three
// independent layers guard against a runaway run:
//   1. The stop-timer never blocks on an ask() — pure steady_clock reads + local atomic sums.
//   2. A hard global send cap (kMaxTotalSent) is a circuit breaker independent of the timer.
//   3. A process watchdog thread force-exits (std::quick_exit) past a generous absolute ceiling.
// The post-run correctness check uses a bounded, non-blocking poll on AskFuture::await_ready()
// with a timeout, never plain block_on() — abandoning an unawaited future is documented-safe
// (gen-fenced ReplyCell), so a timed-out poll reports INCONCLUSIVE instead of hanging.
//
// Ramp up from small pair counts manually (1 -> 2 -> 4 -> ...) — each pair adds TWO OS threads
// (one producer, one dedicated worker), so this grows the thread count twice as fast as the old
// shared-mailbox version did for the same "producer count" argument.
//
// Build: see CMakeLists.txt
// Run:   quark_stress_bench.exe [pairs] [duration_seconds] [warmup_seconds] [probe_limit] [ack_spin_limit]
// Default: pairs = 2 (4 total OS threads, at this repo's standing multi-thread-stress cap),
//          duration = 5s, warmup = 1s, probe_limit = 0 (ADR-038 cooperative eviction disabled,
//          shipped default), ack_spin_limit = 128 (only consulted when probe_limit != 0).

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace quark;
using namespace std::chrono;

namespace {

struct Stats {
    double p50, p99, p999, mean, min, max;
    size_t n;
};

Stats summarize(std::vector<double>& ns) {
    if (ns.empty()) return {};
    std::sort(ns.begin(), ns.end());
    Stats s;
    s.n = ns.size();
    s.min = ns.front();
    s.max = ns.back();
    auto p = [&](double q) { return ns[static_cast<size_t>(q * (ns.size() - 1))]; };
    s.p50 = p(0.50);
    s.p99 = p(0.99);
    s.p999 = p(0.999);
    double sum = 0;
    for (auto v : ns) sum += v;
    s.mean = sum / s.n;
    return s;
}

void print_stats(const char* label, const Stats& s) {
    std::printf("%s  (n=%zu)\n", label, s.n);
    std::printf("  p50  = %8.1f ns   p99  = %8.1f ns   p999 = %8.1f ns\n",
                s.p50, s.p99, s.p999);
    std::printf("  mean = %8.1f ns   min  = %8.1f ns   max  = %8.1f ns\n",
                s.mean, s.min, s.max);
}

// Sequential serializes handle() for both message types, so a plain (non-atomic) counter is
// safe even though tell() and ask() arrive concurrently from different threads — the same
// pattern as tests/authoring_e2e_test.cpp's Counter actor (Add mutates, Ask<GetTotal,int> reads).
struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<int, Ask<int, std::uint64_t>>;
    std::uint64_t count = 0;

    void handle(const int&) noexcept { ++count; }
    void handle(const Ask<int, std::uint64_t>& m) noexcept { m.respond(count); }
};

// Bounded, non-blocking poll for one actor's live count — never plain block_on(). Returns
// {value, true} if the ask resolved within timeout_s, {0, false} (safely abandoning the future)
// otherwise.
std::pair<std::uint64_t, bool> try_poll_count(ActorRef<PingActor>& ref, double timeout_s) {
    AskFuture<std::uint64_t> fut = ref.ask<std::uint64_t>(0);
    auto deadline = steady_clock::now() + duration<double>(timeout_s);
    while (!fut.await_ready()) {
        if (steady_clock::now() >= deadline) return {0, false};
        std::this_thread::sleep_for(milliseconds(5));
    }
    auto res = block_on(std::move(fut));  // already ready: resolves immediately, does not park
    if (!res.has_value()) return {0, false};
    return {res.value(), true};
}

constexpr unsigned kStopCheckMask = 1023;   // check the stop flag every 1024 sends
constexpr unsigned kSampleMask = 8191;      // sample enqueue latency every 8192nd send

}  // namespace

int main(int argc, char** argv) {
    // Safe-by-default: CLAUDE.md caps multi-thread stress at 4 threads on this box. Each pair
    // costs TWO threads (producer + dedicated worker), so pairs=2 already reaches that cap.
    // Going past it is an explicit, deliberate CLI override — ramp there manually.
    unsigned pairs = 2;
    if (argc > 1) {
        int p = std::atoi(argv[1]);
        if (p > 0) pairs = static_cast<unsigned>(p);
    }
    if (pairs > 32) pairs = 32;

    double duration_s = 5.0;
    if (argc > 2) {
        double d = std::atof(argv[2]);
        if (d > 0) duration_s = d;
    }
    double warmup_s = 1.0;
    if (argc > 3) {
        double w = std::atof(argv[3]);
        if (w >= 0) warmup_s = w;
    }

    // ADR-038 tie-breaking experiment (proving-only CLI toggle): two trailing, optional args to
    // exercise Bounded Cooperative Drain-Owner Eviction (shipped default-off, probe_limit=0) through
    // this bench's real worker_loop/park()/wake path, instead of the F2 harness's busy-poll
    // simplification. Defaults (0 / 128) match EngineConfig's own shipped defaults, so existing
    // 3-arg invocations are byte-identical to before this change.
    std::uint32_t drain_owner_steal_probe_limit = 0;
    if (argc > 4) {
        int v = std::atoi(argv[4]);
        if (v >= 0) drain_owner_steal_probe_limit = static_cast<std::uint32_t>(v);
    }
    std::uint32_t drain_owner_steal_ack_spin_limit = 128;
    if (argc > 5) {
        int v = std::atoi(argv[5]);
        if (v >= 0) drain_owner_steal_ack_spin_limit = static_cast<std::uint32_t>(v);
    }
    // ADR-038 Round 3 (the cheaper heuristic): a sixth optional trailing arg. Default (8) matches
    // EngineConfig's own shipped default so existing 5-arg invocations are unchanged.
    std::uint32_t drain_owner_steal_miss_threshold = 8;
    if (argc > 6) {
        int v = std::atoi(argv[6]);
        if (v >= 0) drain_owner_steal_miss_threshold = static_cast<std::uint32_t>(v);
    }

    // Circuit breaker: hard cap on total messages enqueued across all pairs in the stress window,
    // independent of the wall-clock timer. In the paired topology a healthy run's cumulative sent
    // count isn't itself a memory risk (each lane drains what it sends — this isn't a proxy for
    // live backlog), so this is a generous backstop against a mistyped/runaway duration_s, not a
    // tight per-second budget. 40M tripped on a perfectly healthy 4-pair/~17M-msg/s run.
    constexpr std::uint64_t kMaxTotalSent = 300'000'000;
    // Drain-wait budget also scales with pairs (see its use below) — a fixed budget under-times
    // at higher pair counts since a single sequential pass over N lanes can itself take N seconds
    // (a real 12-pair CAF run demonstrated this exact bug; fixed here symmetrically even though
    // Quark's runs happened not to trigger it).
    const double kDrainTimeoutS = 15.0 + pairs * 2.0;
    // Absolute process-runtime ceiling — force-exits if anything above ever fails to converge.
    const double kWatchdogCeilingS = warmup_s + duration_s + kDrainTimeoutS + 15.0;

    unsigned max_workers = std::thread::hardware_concurrency();
    unsigned total_threads = pairs * 2;
    std::printf("=== Quark Sustained Stress Test (M:S paired lanes, whole-engine throughput) ===\n");
    std::printf("Pairs: %u producer<->actor lanes (machine max: %u logical cores) | total OS threads: ~%u\n",
                pairs, max_workers, total_threads);
    std::printf("Duration:  %.1fs sustained (+%.1fs untimed warmup) | send cap: %llu | watchdog: %.0fs\n",
                duration_s, warmup_s, static_cast<unsigned long long>(kMaxTotalSent), kWatchdogCeilingS);
    std::printf("ADR-038 cooperative eviction: probe_limit=%u ack_spin_limit=%u miss_threshold=%u"
                " (probe_limit=0 = shipped default, byte-identical to pre-ADR-038 behavior)\n",
                drain_owner_steal_probe_limit, drain_owner_steal_ack_spin_limit,
                drain_owner_steal_miss_threshold);
    if (total_threads > 4) {
        std::printf("WARNING: %u threads exceeds this repo's standing 4-thread multi-thread-stress "
                    "cap (CLAUDE.md). Only run this deliberately, ramped up from a smaller pair "
                    "count first.\n", total_threads);
    }
    std::printf("\n");

    // Last-resort backstop: independent of every other stop mechanism.
    std::jthread watchdog([kWatchdogCeilingS](std::stop_token st) {
        auto deadline = steady_clock::now() + duration<double>(kWatchdogCeilingS);
        while (steady_clock::now() < deadline) {
            if (st.stop_requested()) return;
            std::this_thread::sleep_for(milliseconds(100));
        }
        if (!st.stop_requested()) {
            std::fprintf(stderr, "\nWATCHDOG: exceeded %.0fs absolute ceiling — force-exiting.\n",
                        kWatchdogCeilingS);
            std::fflush(stderr);
            std::quick_exit(3);
        }
    });

    // Paired topology: one shard/worker per pair, producer[i] only ever talks to actor[i] —
    // mirrors quark_mpsc_sharded_bench.cpp, sustained instead of a fixed op count.
    Engine eng(EngineConfig{
        .worker_count = pairs,
        .shard_count = pairs,
        .drain_budget = 64,
        .busy_spin_limit = 1024,
        .drain_owner_steal_probe_limit = drain_owner_steal_probe_limit,
        .drain_owner_steal_ack_spin_limit = drain_owner_steal_ack_spin_limit,
        .drain_owner_steal_miss_threshold = drain_owner_steal_miss_threshold,
    });
    detail::MessagePool pool{4096, pairs};
    std::vector<ActorId> aids;
    aids.reserve(pairs);
    for (unsigned t = 0; t < pairs; ++t)
        aids.push_back(eng.spawn<PingActor>(t, pool.sink()).value());
    LocalRouter router(eng.post_courier(), pool);
    std::vector<ActorRef<PingActor>> refs;
    refs.reserve(pairs);
    for (unsigned t = 0; t < pairs; ++t)
        refs.push_back(router.get<PingActor>(t));
    eng.start();

    // --- Warmup (untimed, primes pool partitions + caches). Stop is a plain wall-clock sleep on
    // the main thread, no ask() involved. ---
    {
        std::atomic<bool> warm_stop{false};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < pairs; ++t) {
            threads.emplace_back([&refs, &warm_stop, t]() {
                std::uint64_t i = 0;
                for (;;) {
                    refs[t].tell(static_cast<int>(i & 0x7FFFFFFF));
                    ++i;
                    if ((i & kStopCheckMask) == 0 && warm_stop.load(std::memory_order_relaxed))
                        return;
                }
            });
        }
        std::this_thread::sleep_for(duration<double>(warmup_s));
        warm_stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();
    }

    // Baseline: each actor's count already includes everything warmup sent, so the timed
    // window's correctness check must subtract it back out per lane.
    std::vector<std::uint64_t> baseline(pairs, 0);
    for (unsigned t = 0; t < pairs; ++t) {
        auto [b, ok] = try_poll_count(refs[t], 10.0);
        baseline[t] = ok ? b : 0;
    }

    // --- Sustained stress run ---
    std::vector<std::atomic<std::uint64_t>> sent_per_thread(pairs);
    for (auto& c : sent_per_thread) c.store(0, std::memory_order_relaxed);
    std::vector<std::vector<double>> thread_samples(pairs);

    std::atomic<bool> stop{false};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < pairs; ++t) {
        threads.emplace_back([&, t]() {
            thread_samples[t].reserve(4096);
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

            std::uint64_t i = 0;
            for (;;) {
                auto s = pal::clock::now();
                refs[t].tell(static_cast<int>(i & 0x7FFFFFFF));
                auto e = pal::clock::now();
                if ((i & kSampleMask) == 0)
                    thread_samples[t].push_back(duration<double, std::nano>(e - s).count());
                ++i;
                if ((i & kStopCheckMask) == 0) {
                    sent_per_thread[t].store(i, std::memory_order_relaxed);
                    if (stop.load(std::memory_order_relaxed)) break;
                }
            }
            sent_per_thread[t].store(i, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_acquire) < pairs) std::this_thread::yield();
    auto t_start = steady_clock::now();
    go.store(true, std::memory_order_release);

    // --- Periodic snapshots while the stress window runs. NO ask() here — sent-only, from local
    // atomics, so this loop can never stall behind a growing mailbox. Short 200ms tick so the
    // send-cap circuit breaker responds quickly if some lane falls behind. ---
    std::printf("%7s  %14s  %12s  %10s\n", "t (s)", "sent", "instant M/s", "cap?");
    std::uint64_t last_sent = 0;
    double last_elapsed = 0.0;
    bool cap_hit = false;
    for (;;) {
        double elapsed = duration<double>(steady_clock::now() - t_start).count();
        std::uint64_t cur_sent = 0;
        for (auto& c : sent_per_thread) cur_sent += c.load(std::memory_order_relaxed);

        if (elapsed >= duration_s || cur_sent >= kMaxTotalSent) {
            if (cur_sent >= kMaxTotalSent) cap_hit = true;
            break;
        }
        std::this_thread::sleep_for(milliseconds(200));
        elapsed = duration<double>(steady_clock::now() - t_start).count();
        cur_sent = 0;
        for (auto& c : sent_per_thread) cur_sent += c.load(std::memory_order_relaxed);

        double instant_mps = (elapsed > last_elapsed)
            ? static_cast<double>(cur_sent - last_sent) / (elapsed - last_elapsed) / 1e6
            : 0.0;
        std::printf("%7.1f  %14llu  %12.2f  %10s\n",
                    elapsed,
                    static_cast<unsigned long long>(cur_sent),
                    instant_mps,
                    cur_sent >= kMaxTotalSent ? "HIT" : "-");
        last_sent = cur_sent;
        last_elapsed = elapsed;
    }

    stop.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t_end = steady_clock::now();

    std::uint64_t total_sent = 0;
    for (auto& c : sent_per_thread) total_sent += c.load(std::memory_order_relaxed);
    double actual_secs = duration<double>(t_end - t_start).count();

    if (cap_hit) {
        std::printf("\nSEND CAP HIT: %llu messages enqueued across %u lanes before hitting the "
                    "%llu cap — some lane's consumer likely can't keep up. Stopped early by design "
                    "(see file header) rather than growing memory further.\n",
                    static_cast<unsigned long long>(total_sent), pairs,
                    static_cast<unsigned long long>(kMaxTotalSent));
    }

    // --- Bounded drain-wait: poll each lane (never plain block_on) until every lane's received
    // count matches what it was sent, or a timeout. ---
    std::vector<std::uint64_t> received(pairs, 0);
    std::vector<bool> resolved(pairs, false);
    auto drain_start = steady_clock::now();
    bool all_resolved_and_drained = false;
    while (!all_resolved_and_drained &&
           duration<double>(steady_clock::now() - drain_start).count() < kDrainTimeoutS) {
        all_resolved_and_drained = true;
        for (unsigned t = 0; t < pairs; ++t) {
            auto [r, ok] = try_poll_count(refs[t], 1.0);
            received[t] = ok && r >= baseline[t] ? r - baseline[t] : 0;
            resolved[t] = ok;
            std::uint64_t want = sent_per_thread[t].load(std::memory_order_relaxed);
            if (!ok || received[t] < want) all_resolved_and_drained = false;
        }
        if (!all_resolved_and_drained) std::this_thread::sleep_for(milliseconds(50));
    }

    eng.stop();
    watchdog.request_stop();

    double total_mps = static_cast<double>(total_sent) / actual_secs / 1e6;
    std::uint64_t total_received = 0;
    bool any_unresolved = false;
    double min_pt_mps = 1e18, max_pt_mps = 0;
    for (unsigned t = 0; t < pairs; ++t) {
        if (!resolved[t]) { any_unresolved = true; continue; }
        total_received += received[t];
        double pt_mps = static_cast<double>(sent_per_thread[t].load(std::memory_order_relaxed))
                       / actual_secs / 1e6;
        min_pt_mps = std::min(min_pt_mps, pt_mps);
        max_pt_mps = std::max(max_pt_mps, pt_mps);
    }

    std::printf("\n=== Sustained window summary ===\n");
    std::printf("Duration:        %.3fs actual (target %.1fs)\n", actual_secs, duration_s);
    std::printf("Total sent:      %llu\n", static_cast<unsigned long long>(total_sent));
    std::printf("Total received:  %s%llu\n", any_unresolved ? "(one or more lanes unconfirmed) " : "",
                static_cast<unsigned long long>(total_received));
    std::printf("Aggregate throughput: %.2f M msg/s   (per-lane range: [%.2f, %.2f] M msg/s)\n\n",
                total_mps, pairs ? min_pt_mps : 0.0, pairs ? max_pt_mps : 0.0);

    std::vector<double> all;
    for (auto& s : thread_samples) all.insert(all.end(), s.begin(), s.end());
    auto stats = summarize(all);
    print_stats("enqueue latency (sampled, all lanes, full window):", stats);

    std::printf("\n");
    int exit_code;
    if (any_unresolved) {
        std::printf("VERDICT: INCONCLUSIVE — could not confirm final delivered count on one or more "
                    "lanes within %.1fs. Total sent: %llu.\n",
                    kDrainTimeoutS, static_cast<unsigned long long>(total_sent));
        exit_code = 2;
    } else if (total_received == total_sent) {
        std::printf("VERDICT: PASS — %llu/%llu messages delivered across %u lanes, zero "
                    "loss/duplication after %.1fs of sustained production-like load.%s\n",
                    static_cast<unsigned long long>(total_received),
                    static_cast<unsigned long long>(total_sent), pairs, actual_secs,
                    cap_hit ? " (window ended early via send cap, not the timer)" : "");
        exit_code = 0;
    } else {
        std::printf("VERDICT: FAIL — drained %llu/%llu messages within the timeout; %lld message(s) "
                    "unaccounted for.\n",
                    static_cast<unsigned long long>(total_received),
                    static_cast<unsigned long long>(total_sent),
                    static_cast<long long>(total_sent) - static_cast<long long>(total_received));
        exit_code = 1;
    }

    return exit_code;
}
