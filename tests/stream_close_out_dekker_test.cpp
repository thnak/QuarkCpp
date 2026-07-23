// Tests 024-Streaming §Arming/wakeup — the stream close-out rides the SAME seq_cst Dekker StoreLoad
// rendezvous as the mailbox (002/ADR-004): the consumer's `release-to-idle + disarm` store and its
// occupancy re-probe must be separated by a seq_cst fence, or a producer arm that races the close-out
// window is LOST (a stranded frame). This is the ADR-014 CONTROL-6 (inherited, load-bearing): an
// isolating Dekker litmus over a BOUNDED trial count.
//   * positive (fence present): lost == 0.
//   * -DQUARK_STREAM_NO_FENCE  : the StoreLoad fence is removed -> x86-TSO reorders store-past-load and
//     both sides miss -> lost > 0 (the fence's necessity, proven, not assumed).
// Two threads (machine safe). Deterministic control flow; no wall clock.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
// ADR-014's own reference run needed the full 5M trials to reliably fire (lost 194 537/192 402 vs 0
// with the fence). 1M was trimmed in for CI runtime but is not reliably enough exposure on GH Actions'
// shared/virtualized x86_64 runners — observed NOT-FIRED (lost=0) on both clang-release and gcc-release
// x86_64 CI legs even with the control given exclusive CPU access (RUN_SERIAL), which rules out
// scheduling contention and points at trial count / hit-rate instead. 5M was bumped back in for that,
// but activation_deactivate_close_out_dekker_control (same litmus shape, same file's sibling) STILL
// reported NOT-FIRED at 5M on a later gcc-release x86_64 CI run — RUN_SERIAL'd, fully CPU-isolated, no
// contention explanation left. The remaining suspect is the runner's virtualized CPU itself: a vCPU
// that is itself time-sliced across physical cores (or has high steal time) can leave the two racing
// threads without genuine cross-core parallelism during the few-nanosecond store-buffer-drain window
// this litmus depends on, which lowers the per-trial hit rate versus dedicated reference hardware — not
// a claim that the fence is wrong, a claim that 5M trials' margin against an all-zero run isn't enough
// on THIS hardware. 10x the trials (matching the same fix applied to topic_no_quiesce_control after an
// identical symptom — see topic_subscribe_race_test.cpp) for a much lower false-negative rate; runtime
// stays well inside ctest's per-test timeout (~100s at this count, measured).
constexpr std::uint64_t kTrials = 50'000'000;

// The two close-out flags, each on its own cache line (isolate the StoreLoad — like armed_ vs the ring
// cursors). Reset every trial; the racing store/load pair runs under a sense-reversing barrier.
struct alignas(64) Flag {
    std::atomic<int> v{0};
};
Flag ax, ay;
std::atomic<int> t0_saw{0};
std::atomic<int> t1_saw{0};

// Sense-reversing 2-thread barrier so both threads execute their store/load pair in the SAME trial.
std::atomic<int> barrier_count{0};
std::atomic<std::uint64_t> barrier_sense{0};

void barrier(std::uint64_t& local_sense) noexcept {
    local_sense ^= 1;
    if (barrier_count.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
        barrier_count.store(0, std::memory_order_relaxed);
        barrier_sense.store(local_sense, std::memory_order_release);
    } else {
        while (barrier_sense.load(std::memory_order_acquire) != local_sense) {
            std::this_thread::yield();
        }
    }
}
}  // namespace

int main() {
    bool no_fence = false;
#if defined(QUARK_STREAM_NO_FENCE)
    no_fence = true;
#endif

    std::atomic<std::uint64_t> lost{0};

    auto run = [&](int who) {
        std::uint64_t sense = 0;
        for (std::uint64_t i = 0; i < kTrials; ++i) {
            barrier(sense);  // line up; then reset the flags
            if (who == 0) ax.v.store(0, std::memory_order_relaxed);
            else ay.v.store(0, std::memory_order_relaxed);
            barrier(sense);  // both flags are 0 before the racing pair

            int other;
            if (who == 0) {
                ax.v.store(1, std::memory_order_relaxed);   // "disarm / release-to-idle" store
#if !defined(QUARK_STREAM_NO_FENCE)
                std::atomic_thread_fence(std::memory_order_seq_cst);  // the load-bearing close-out fence
#endif
                other = ay.v.load(std::memory_order_relaxed);         // "occupancy / arm" re-probe
                t0_saw.store(other, std::memory_order_relaxed);
            } else {
                ay.v.store(1, std::memory_order_relaxed);
#if !defined(QUARK_STREAM_NO_FENCE)
                std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
                other = ax.v.load(std::memory_order_relaxed);
                t1_saw.store(other, std::memory_order_relaxed);
            }

            barrier(sense);  // both observations recorded
            // Dekker guarantee: at least one thread must observe the other's store. If BOTH read 0 the
            // wakeup is LOST — impossible WITH the fence, reachable WITHOUT it on x86-TSO. Thread 0 tallies.
            if (who == 0 && t0_saw.load(std::memory_order_relaxed) == 0 &&
                t1_saw.load(std::memory_order_relaxed) == 0) {
                lost.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::jthread a([&] { run(0); });
    std::jthread b([&] { run(1); });
    a.join();
    b.join();

    const std::uint64_t l = lost.load();
    bool ok;
    if (no_fence) {
        ok = (l > 0);  // the control MUST strand at least once — proving the fence load-bearing
        std::printf("stream_close_out_dekker_test [CONTROL no-fence]: %s  (trials=%" PRIu64
                    " lost=%" PRIu64 ")\n", ok ? "FIRED" : "NOT-FIRED", kTrials, l);
    } else {
        ok = (l == 0);  // WITH the fence: never a lost wakeup
        std::printf("stream_close_out_dekker_test: %s  (trials=%" PRIu64 " lost=%" PRIu64 ")\n",
                    ok ? "OK" : "FAIL", kTrials, l);
    }
    return ok ? 0 : 1;
}
