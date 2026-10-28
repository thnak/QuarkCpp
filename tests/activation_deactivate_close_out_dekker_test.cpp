// Tests ADR-028 Phase 1 §close_out_retire — the Deactivate close-out rides the SAME seq_cst Dekker
// StoreLoad rendezvous as every other Activation close-out (002/ADR-004, and 024's stream close-out,
// tests/stream_close_out_dekker_test.cpp): the consumer's `retire_to_dormant` store and its
// mailbox-occupancy re-probe must be StoreLoad-ordered against the producer's enqueue + exec-state
// read, or a producer message that races the close-out window is LOST — the eviction would commit
// while a message sits unseen in the mailbox (ADR-028's "[Deactivate, M] abort-eviction never loses
// M" claim, S2). ADR-066 spells that ordering as seq_cst OPERATIONS on both sides (a seq_cst store,
// then a seq_cst load) instead of a release store + standalone seq_cst fence; this litmus mirrors it.
//   * positive (seq_cst store/load): lost == 0 — the race ALWAYS resolves to "abort eviction,
//     dispatch M" or "producer's own notify_enqueued edge already sees the retire", never "both miss".
//   * -DQUARK_ACTIVATION_DEACTIVATE_NO_FENCE : the StoreLoad ordering is removed (the same store/load
//     pair downgraded to relaxed) -> x86-TSO reorders store-past-load and both sides can miss ->
//     lost > 0 (the ordering's necessity, proven, not assumed).
//
// This is the repo's own established isolating-Dekker-litmus methodology, matching
// stream_close_out_dekker_test.cpp. (With the seq_cst-operation spelling, TSan now also models every
// edge of the real rendezvous; this litmus remains the hardware store-buffer-reorder proof.)
// Two threads (machine safe). Deterministic control flow; no wall clock.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
// See stream_close_out_dekker_test.cpp's kTrials comment: ADR-014's reference run needed 5M trials to
// reliably fire; 1M proved insufficient on GH Actions' shared x86_64 runners even under RUN_SERIAL
// exclusive scheduling (observed NOT-FIRED, lost=0, on both clang-release and gcc-release x86_64 CI).
// 5M itself then ALSO reported NOT-FIRED (lost=0) on a later gcc-release x86_64 CI run, fully
// RUN_SERIAL-isolated with zero other tests sharing the runner — ruling out scheduling contention a
// second time and pointing at the trial count's margin against the runner's own (likely virtualized,
// possibly time-sliced-vCPU) hit rate for this few-nanosecond store-buffer-drain window. 10x again,
// matching topic_no_quiesce_control's precedent for the identical symptom.
#if defined(_MSC_VER) && defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kTrials = 500'000;
#else
constexpr std::uint64_t kTrials = 50'000'000;
#endif

// The racing pair's orders: the real close-out's seq_cst store + seq_cst probe (ADR-066), or - the
// control - the same pair with the StoreLoad ordering removed.
#if defined(QUARK_ACTIVATION_DEACTIVATE_NO_FENCE)
constexpr std::memory_order kStoreOrder = std::memory_order_relaxed;
constexpr std::memory_order kLoadOrder = std::memory_order_relaxed;
#else
constexpr std::memory_order kStoreOrder = std::memory_order_seq_cst;
constexpr std::memory_order kLoadOrder = std::memory_order_seq_cst;
#endif

// ax = the consumer's "retire_to_dormant" store; ay = the producer's "message enqueued" store. Each
// on its own cache line (isolate the StoreLoad, like the real retire_to_dormant/mailbox pair).
struct alignas(64) Flag {
    std::atomic<int> v{0};
};
Flag ax, ay;
std::atomic<int> consumer_saw{0};
std::atomic<int> producer_saw{0};

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
#if defined(QUARK_ACTIVATION_DEACTIVATE_NO_FENCE)
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
                // "consumer": close_out_retire()'s retire_to_dormant() seq_cst store.
                ax.v.store(1, kStoreOrder);
                other = ay.v.load(kLoadOrder);  // probe_has_work() seq_cst re-probe
                consumer_saw.store(other, std::memory_order_relaxed);
            } else {
                // "producer": a message's seq_cst mailbox enqueue + seq_cst exec-state CAS read.
                ay.v.store(1, kStoreOrder);
                other = ax.v.load(kLoadOrder);  // "did the retire already commit?" read
                producer_saw.store(other, std::memory_order_relaxed);
            }

            barrier(sense);  // both observations recorded
            // Dekker guarantee: at least one side must observe the other's store — either the
            // consumer's probe sees the message (aborts eviction, dispatches M) or the producer's
            // read sees the retire is in flight (its own notify_enqueued edge is what wakes a future
            // reactivation path, per ADR-028 Phase 4 — not exercised here, only the ordering is).
            // If BOTH read 0, the message is LOST under a real eviction: impossible WITH seq_cst,
            // reachable WITHOUT it on x86-TSO. Thread 0 tallies.
            if (who == 0 && consumer_saw.load(std::memory_order_relaxed) == 0 &&
                producer_saw.load(std::memory_order_relaxed) == 0) {
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
        ok = (l > 0);  // the control MUST strand at least once — proving the ordering load-bearing
        std::printf("activation_deactivate_close_out_dekker_test [CONTROL no-fence]: %s  (trials=%"
                    PRIu64 " lost=%" PRIu64 ")\n", ok ? "FIRED" : "NOT-FIRED", kTrials, l);
    } else {
        ok = (l == 0);  // WITH seq_cst: never a lost message across the [Deactivate, M] race
        std::printf("activation_deactivate_close_out_dekker_test: %s  (trials=%" PRIu64
                    " lost=%" PRIu64 ")\n", ok ? "OK" : "FAIL", kTrials, l);
    }
    return ok ? 0 : 1;
}
