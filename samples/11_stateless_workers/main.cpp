// Quark sample 11 — Stateless worker pool (025 Stateless<N>).
//
// Some work has no per-actor identity — image resizes, hash checks, format conversions. For that,
// Quark offers a POOL of N interchangeable, stateless worker activations behind a local load-balancing
// router. Any request can go to any worker; the router picks one (LeastLoaded = power-of-two-choices
// on mailbox depth, or RoundRobin). The key guarantee is EXACTLY-ONCE: each message is enqueued on
// exactly one worker's mailbox, so it is handled by exactly one worker — never lost, never duplicated.
// Each worker is single-executor, so its scratch state is never corrupted by concurrency.
//
// This sample is deterministic and single-threaded: post M jobs through the router, drive every
// worker's lane to completion, then verify the pool distributed the load and handled each job once.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 11_stateless_workers
// Run  :  taskset -c 0-3 build/samples/11_stateless_workers
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/stateless_pool.hpp"

using namespace quark;

struct Job {
    std::uint64_t token;
};

// A stateless worker: it owns no durable identity, just counts what it processed (scratch state, safe
// because each activation is single-executor). It records each token so we can prove exactly-once.
struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Job>;
    std::uint64_t handled = 0;
    std::vector<std::uint64_t>* seen = nullptr;  // shared oracle (written only on this worker's lane)
    void handle(const Job& j) noexcept {
        ++handled;
        if (seen) (*seen)[j.token]++;  // exactly-once check: every token must end at count 1
    }
};

// The per-lane run loop (same as the 002 scheduler runs); inlined so the sample needs no engine.
static void drive_lane(Activation& act) {
    if (!act.try_acquire()) return;
    std::uint64_t busy = 0;
    for (;;) {
        switch (act.drain_step(1u << 20)) {
            case Activation::DrainOutcome::DrainedEmpty:
                if (act.close_out()) { busy = 0; continue; }
                return;
            case Activation::DrainOutcome::Busy:
                if (++busy > (1u << 24)) return;
                continue;
            case Activation::DrainOutcome::BudgetExhausted:
                act.yield_to_scheduled();
                if (act.try_acquire()) { busy = 0; continue; }
                return;
            case Activation::DrainOutcome::Suspended:
                return;
        }
    }
}

int main() {
    constexpr std::size_t kWorkers = 4;
    constexpr std::uint64_t kJobs = 20'000;

    // A pool of 4 stateless workers with power-of-two-choices least-loaded routing.
    StatelessPool<Worker> pool(kWorkers, PoolRoute::LeastLoaded);

    // Shared exactly-once oracle: seen[token] must be exactly 1 at the end.
    std::vector<std::uint64_t> seen(kJobs, 0);
    for (std::size_t i = 0; i < pool.size(); ++i) pool.actor(i).seen = &seen;

    // Keep the job payloads + descriptors alive for the whole run (0 hot-path alloc on the pool side).
    std::vector<Job> jobs(kJobs);
    std::vector<Descriptor> descs(kJobs);
    for (std::uint64_t t = 0; t < kJobs; ++t) {
        jobs[t].token = t;
        descs[t].payload = &jobs[t];
        stamp<Worker, Job>(descs[t]);
        pool.post(&descs[t]);  // router picks a worker; message lands on exactly one mailbox
    }

    // Drive every worker's lane until all mailboxes are empty.
    for (std::size_t i = 0; i < pool.size(); ++i) drive_lane(pool.activation(i));

    // Verify: total handled == kJobs, load was distributed, and each token handled exactly once.
    std::uint64_t total = 0, lost = 0, dup = 0;
    for (std::uint64_t t = 0; t < kJobs; ++t) {
        if (seen[t] == 0) ++lost;
        else if (seen[t] > 1) ++dup;
    }
    std::printf("pool of %zu workers processed %llu jobs:\n", pool.size(), (unsigned long long)kJobs);
    for (std::size_t i = 0; i < pool.size(); ++i) {
        total += pool.actor(i).handled;
        std::printf("  worker %zu handled %llu jobs\n", i, (unsigned long long)pool.actor(i).handled);
    }
    std::printf("  distinct workers used: %zu / %zu\n", pool.used_count(), pool.size());
    std::printf("  total handled=%llu (expected %llu), lost=%llu, duplicated=%llu\n",
                (unsigned long long)total, (unsigned long long)kJobs,
                (unsigned long long)lost, (unsigned long long)dup);

    const bool ok = total == kJobs && lost == 0 && dup == 0 && pool.used_count() >= 2;
    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
