// ADR-077: the producer wake edge's no-transition half is a seq_cst LOAD (not a failed locked CAS).
// This litmus isolates exactly that shape against the consumer close-out's seq_cst store + seq_cst
// mailbox probe:
//
//   consumer: state.store(Idle, seq_cst);        // release_to_idle_sc()
//             saw = mailbox.load(seq_cst);        // probe_has_work()
//   producer: mailbox.store(1, seq_cst);         // mailbox enqueue
//             st = state.load(seq_cst);           // notify_enqueued_fast()'s fast read
//
// If the producer reads a state that means "someone else will drain" and the consumer's probe misses
// the message, the message is stranded. WITH seq_cst both accesses sit in one total order S, so at
// least one side must observe the other: lost == 0. The control downgrades both pairs to relaxed; on
// x86-TSO the store buffer can reorder store-past-load so BOTH miss (lost > 0), proving the ordering.
//
// Same methodology/exclusions as activation_deactivate_close_out_dekker_test.cpp / stream_close_out_
// dekker_test.cpp: the negative control is meaningful only on x86-64, not under TSan/ASan (both perturb
// or serialize the store buffer), and needs exclusive CPU access — the CMake registration for the
// control carries those gates. The positive binary runs everywhere.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "quark/core/exec_state.hpp"

using namespace quark;

namespace {
#if defined(_MSC_VER) && defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kTrials = 500'000;
#else
constexpr std::uint64_t kTrials = 50'000'000;
#endif

#if defined(QUARK_EXEC_WAKE_LOAD_NO_FENCE)
constexpr std::memory_order kStoreOrder = std::memory_order_relaxed;
constexpr std::memory_order kLoadOrder = std::memory_order_relaxed;
#else
constexpr std::memory_order kStoreOrder = std::memory_order_seq_cst;
constexpr std::memory_order kLoadOrder = std::memory_order_seq_cst;
#endif

struct alignas(64) Flag {
    std::atomic<int> v{0};
};

std::atomic<ExecState> state{ExecState::Running};  // the consumer owns the activation
Flag mailbox;                                      // the message-arrival store / probe
std::atomic<int> consumer_saw{0};
std::atomic<int> producer_wake{0};

std::atomic<int> barrier_count{0};
std::atomic<std::uint64_t> barrier_sense{0};

void barrier(std::uint64_t& local_sense) noexcept {
    local_sense ^= 1;
    if (barrier_count.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
        barrier_count.store(0, std::memory_order_relaxed);
        barrier_sense.store(local_sense, std::memory_order_release);
    } else {
        while (barrier_sense.load(std::memory_order_acquire) != local_sense) std::this_thread::yield();
    }
}
}  // namespace

int main() {
    bool no_fence = false;
#if defined(QUARK_EXEC_WAKE_LOAD_NO_FENCE)
    no_fence = true;
#endif

    std::atomic<std::uint64_t> lost{0};

    auto run = [&](int who) {
        std::uint64_t sense = 0;
        for (std::uint64_t i = 0; i < kTrials; ++i) {
            barrier(sense);
            if (who == 0) {
                state.store(ExecState::Running, std::memory_order_relaxed);  // consumer owns it again
            } else {
                mailbox.v.store(0, std::memory_order_relaxed);
            }
            barrier(sense);

            if (who == 0) {
                // Consumer close-out: Running -> Idle, then the mailbox re-probe.
                state.store(ExecState::Idle, kStoreOrder);
                consumer_saw.store(mailbox.v.load(kLoadOrder), std::memory_order_relaxed);
            } else {
                // Producer: enqueue, then the fast-path state read. Idle/Dormant would wake (CAS/readmit);
                // any other state means the current owner will drain or re-admit.
                mailbox.v.store(1, kStoreOrder);
                const ExecState st = state.load(kLoadOrder);
                producer_wake.store((st == ExecState::Idle || st == ExecState::Dormant) ? 1 : 0,
                                    std::memory_order_relaxed);
            }

            barrier(sense);
            if (who == 0 && producer_wake.load(std::memory_order_relaxed) == 0 &&
                consumer_saw.load(std::memory_order_relaxed) == 0) {
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
        ok = (l > 0);  // the control MUST strand — the load's ordering is load-bearing
        std::printf("exec_state_wake_load_dekker_test [CONTROL no-fence]: %s  (trials=%" PRIu64
                    " lost=%" PRIu64 ")\n", ok ? "FIRED" : "NOT-FIRED", kTrials, l);
    } else {
        ok = (l == 0);  // seq_cst load: the producer/consumer pair can never both miss
        std::printf("exec_state_wake_load_dekker_test: %s  (trials=%" PRIu64 " lost=%" PRIu64 ")\n",
                    ok ? "OK" : "FAIL", kTrials, l);
    }
    return ok ? 0 : 1;
}
