// Tests 025 §Part C `Stateless<N>` pool EXACTLY-ONCE under concurrency (ADR-011 Gate C). Under ≤4
// concurrent threads (2 senders + 2 workers) driving an N=4 pool of single-executor activations over a
// BOUNDED message count, the pool delivers each message to EXACTLY ONE activation:
//   * lost = 0        — every token handled;
//   * duplicated = 0  — no token handled twice (checked by SET, not count);
//   * torn = 0        — each activation is single-executor, so its per-activation state is never
//     corrupted (reconciles exactly to the global XOR/count).
// It also proves the run is NON-VACUOUS: ≥2 activations genuinely ran concurrently (max_active ≥ 2),
// each activation stayed single-executor (per-slot max_live == 1), and ≤ N activations were used.
//
// The mandatory SHARED-STATE CONTROL (build with -DQUARK_STATELESS_SHARED_STATE_CONTROL) deliberately
// shares ONE plain accumulator across activations WITHOUT the per-activation discipline: the torn
// counter (and TSan) CATCHES it — proving the teeth. Machine-safety: ≤4 threads, bounded M (a few ×10⁴
// is plenty to expose a lost/dup/torn), sub-second — NOT ADR-011's full 10⁷.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/stateless_pool.hpp"

using namespace quark;

namespace {

constexpr std::size_t kN = 4;            // pool size (activations)
constexpr unsigned kSenders = 2;         // concurrent producer threads
constexpr unsigned kWorkers = 2;         // concurrent worker threads (2+2 = 4 threads, ≤4 cap)
constexpr std::uint64_t kM = 40'000;     // BOUNDED total messages (machine safety; not ADR-011's 10⁷)
constexpr std::uint32_t kBudget = 16;    // small drain budget ⇒ many handoffs ⇒ stress the protocol

struct Job {
    std::uint64_t token;
};

// A one-time rendezvous that FORCES `need` activations to be inside handle() at once, so the
// cross-activation overlap (the non-vacuity witness AND the control's race window) is GUARANTEED rather
// than left to the scheduler — which otherwise starves this test under `ctest -j2` / sanitizer slowdown.
// The first arriver parks on the condvar (yielding its core), so even a single-core scheduler makes
// progress: the other worker gets scheduled, arrives, and trips the rendezvous. A generous timeout is a
// safety net against an unexpected single-worker run (never hit in practice — the backlog spans slots).
struct OverlapGate {
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    bool tripped = false;
    int need = 2;
    void arrive() {
        std::unique_lock<std::mutex> lk(m);
        if (tripped) return;
        if (++arrived >= need) {
            tripped = true;
            cv.notify_all();
            return;
        }
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return tripped; });
    }
};

// The shared exactly-once oracle + concurrency witnesses (all ATOMIC — infrastructure, not pool state).
struct Oracle {
    std::vector<std::atomic<std::uint8_t>>* seen = nullptr;
    std::atomic<std::uint64_t> handled{0};
    std::atomic<std::uint64_t> dup{0};
    std::atomic<int> active{0};      // cross-activation concurrency witness
    std::atomic<int> max_active{0};  // high-water of concurrently-running activations
    OverlapGate gate;                // forces ≥2 activations to overlap deterministically
};

#ifdef QUARK_STATELESS_SHARED_STATE_CONTROL
// CONTROL: a SINGLE plain accumulator shared by every activation — the per-activation discipline is
// deliberately violated. Concurrent RMWs race (TSan) and lose updates (torn value).
long g_ctrl_count = 0;
std::uint64_t g_ctrl_xor = 0;
#endif

void bump_max(std::atomic<int>& m, int v) noexcept {
    int prev = m.load(std::memory_order_relaxed);
    while (v > prev && !m.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

struct Worker : Actor<Worker, Sequential> {
    using protocol = Protocol<Job>;
    Oracle* oracle = nullptr;

    // Per-activation state (PLAIN — mutated only inside handle(); race-free BECAUSE single-executor,
    // the exec-state handoff giving happens-before across worker handoffs — TSan proves it).
    std::uint64_t acc_count = 0;
    std::uint64_t acc_xor = 0;

    // Per-activation single-executor witness.
    std::atomic<int> live{0};
    std::atomic<int> max_live{0};

    void handle(const Job& j) noexcept {
        const int nl = live.fetch_add(1, std::memory_order_acq_rel) + 1;
        bump_max(max_live, nl);
        const int na = oracle->active.fetch_add(1, std::memory_order_acq_rel) + 1;
        bump_max(oracle->max_active, na);
        // Deterministically rendezvous ≥2 activations here (once): both increment `active` before either
        // decrements ⇒ max_active reaches ≥2, and (in the control) both hit the shared RMW at once ⇒ the
        // race manifests. Robust to scheduler/sanitizer timing, unlike a bare spin.
        oracle->gate.arrive();

        // Exactly-once by SET: CAS the token's seen byte 0→1; a second delivery loses the CAS ⇒ dup.
        std::uint8_t e = 0;
        if (!(*oracle->seen)[j.token].compare_exchange_strong(e, 1, std::memory_order_acq_rel))
            oracle->dup.fetch_add(1, std::memory_order_relaxed);

#ifdef QUARK_STATELESS_SHARED_STATE_CONTROL
        // Shared plain accumulator RMW'd by ≥2 activations concurrently → data race + lost updates. The
        // spin widens the read-modify-write window so the tearing is reliable even without TSan.
        long tmp = g_ctrl_count;
        for (volatile int s = 0; s < 64; ++s) {
        }
        g_ctrl_count = tmp + 1;
        g_ctrl_xor ^= j.token;
#else
        acc_count += 1;        // per-activation, single-executor ⇒ intact
        acc_xor ^= j.token;
#endif

        oracle->handled.fetch_add(1, std::memory_order_release);
        oracle->active.fetch_sub(1, std::memory_order_acq_rel);
        live.fetch_sub(1, std::memory_order_acq_rel);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    Oracle oracle;
    std::vector<std::atomic<std::uint8_t>> seen(kM);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);
    oracle.seen = &seen;

    // Pre-allocate messages + descriptors COLD (senders only enqueue; workers only drain/reclaim).
    std::vector<Job> msgs(kM);
    std::vector<Descriptor> descs(kM);
    for (std::uint64_t i = 0; i < kM; ++i) {
        msgs[i].token = i;
        descs[i].payload = &msgs[i];
        stamp<Worker, Job>(descs[i]);
    }

    oracle.gate.need = static_cast<int>(kWorkers);  // rendezvous all workers ⇒ guaranteed overlap

    StatelessPool<Worker> pool(kN, PoolRoute::LeastLoaded);
    for (std::size_t i = 0; i < pool.size(); ++i) pool.actor(i).oracle = &oracle;

    std::atomic<bool> stop{false};

    // (1) CONCURRENT PRODUCERS: kSenders threads post all M messages, partitioned so every token is
    // posted EXACTLY once. Concurrent posting exercises the pool's multi-producer routing + MPSC
    // enqueue. Posting the full backlog BEFORE draining guarantees both workers find work on distinct
    // slots at once ⇒ sustained cross-activation overlap (a robust, timing-independent non-vacuity),
    // while the exactly-once surface — route-to-one-activation + single-executor-per-activation under
    // concurrent DRAIN — is exercised in full below.
    {
        std::vector<std::jthread> senders;
        senders.reserve(kSenders);
        const std::uint64_t per = kM / kSenders;
        for (unsigned s = 0; s < kSenders; ++s) {
            const std::uint64_t lo = s * per;
            const std::uint64_t hi = (s + 1 == kSenders) ? kM : lo + per;
            senders.emplace_back([&, lo, hi] {
                for (std::uint64_t i = lo; i < hi; ++i) pool.post(&descs[i]);
            });
        }
    }  // senders joined — the whole backlog is now queued across the pool

    // (2) CONCURRENT CONSUMERS: kWorkers threads busy-poll the acquire edge of each slot (the harness
    // stands in for 002's targeted wakeup). The exec-state CAS guarantees ≤1 worker drains any given
    // slot at once (single-executor); two workers drain two DIFFERENT slots concurrently.
    std::vector<std::jthread> workers;
    workers.reserve(kWorkers);
    for (unsigned w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                bool did = false;
                for (std::size_t i = 0; i < pool.size(); ++i) {
                    Activation& act = pool.activation(i);
                    if (!act.try_acquire()) continue;
                    did = true;
                    for (;;) {
                        const auto o = act.drain_step(kBudget);
                        if (o == Activation::DrainOutcome::DrainedEmpty) {
                            if (act.close_out()) continue;
                            break;
                        }
                        if (o == Activation::DrainOutcome::BudgetExhausted) {
                            act.yield_to_scheduled();
                            break;
                        }
                        if (o == Activation::DrainOutcome::Busy) {
                            continue;  // producer mid-publish: bounded spin, keep Running
                        }
                        break;  // Suspended: no async handler in this pool
                    }
                }
                if (!did) std::this_thread::yield();
            }
        });
    }

    // Wait for every message to be handled, then stop the workers.
    constexpr std::uint64_t kStallLimit = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (oracle.handled.load(std::memory_order_acquire) < kM) {
        if (++spins > kStallLimit) {
            std::fprintf(stderr, "STALL: handled %" PRIu64 " / %" PRIu64 "\n",
                         oracle.handled.load(), kM);
            stop.store(true, std::memory_order_release);
            return 1;
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();

    // ---- Exactly-once accounting -------------------------------------------------------------
    std::uint64_t lost = 0;
    std::uint64_t expected_xor = 0;
    for (std::uint64_t t = 0; t < kM; ++t) {
        if (seen[t].load(std::memory_order_relaxed) == 0) ++lost;
        expected_xor ^= t;
    }
    const std::uint64_t dup = oracle.dup.load(std::memory_order_relaxed);

    // Reconcile the per-activation state (or the shared control accumulator) against the global truth.
#ifdef QUARK_STATELESS_SHARED_STATE_CONTROL
    const std::uint64_t observed_count = static_cast<std::uint64_t>(g_ctrl_count);
    const std::uint64_t observed_xor = g_ctrl_xor;
#else
    std::uint64_t observed_count = 0, observed_xor = 0;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        observed_count += pool.actor(i).acc_count;
        observed_xor ^= pool.actor(i).acc_xor;
    }
#endif
    const std::uint64_t torn =
        (observed_count != kM ? 1u : 0u) + (observed_xor != expected_xor ? 1u : 0u);

    std::size_t used = pool.used_count();
    int worst_slot_live = 0;
    for (std::size_t i = 0; i < pool.size(); ++i)
        worst_slot_live = std::max(worst_slot_live, pool.actor(i).max_live.load());
    const int max_active = oracle.max_active.load();

    std::printf(
        "  N=%zu senders=%u workers=%u M=%" PRIu64 " | lost=%" PRIu64 " dup=%" PRIu64 " torn=%" PRIu64
        " | used=%zu max_active=%d worst_slot_max_live=%d observed_count=%" PRIu64 "\n",
        kN, kSenders, kWorkers, kM, lost, dup, torn, used, max_active, worst_slot_live, observed_count);

#ifdef QUARK_STATELESS_SHARED_STATE_CONTROL
    // The CONTROL passes iff it CAUGHT the tearing (torn > 0). Under TSan the shared-plain race is
    // reported (and, with halt_on_error, aborts non-zero) BEFORE we get here — the WILL_FAIL teeth.
    const bool caught = (torn > 0);
    std::printf("stateless_pool_shared_state_control: %s (caught torn=%" PRIu64
                ", shared-state race — exactly the discipline the real pool keeps)\n",
                caught ? "OK (teeth fired)" : "FAIL (did not catch)", torn);
    return caught ? 0 : 1;
#else
    check(lost == 0, "no message lost (every token handled exactly once)", ok);
    check(dup == 0, "no message duplicated (each token handled by exactly one activation)", ok);
    check(torn == 0, "no torn state (each activation single-executor; state reconciles exactly)", ok);
    check(used <= kN, "at most N activations were used (the pool never exceeds N)", ok);
    check(worst_slot_live == 1, "each activation stayed single-executor (per-slot max_live == 1)", ok);
    check(max_active >= 2, "≥2 activations genuinely ran CONCURRENTLY (non-vacuous parallelism)", ok);
    std::printf("stateless_pool_exactly_once_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
#endif
}
