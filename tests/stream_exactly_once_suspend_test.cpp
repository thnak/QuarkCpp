// Tests 024-Streaming §Why disp/tail are split + the ADR-014 promotion gate — EXACTLY-ONCE across
// async-suspend, wired to the REAL 002 exec-state machine + the 015 admission gate (NOT a model
// resolver). Three DISTINCT thread roles with real cross-thread exec-state handoff:
//   * 1 PRODUCER thread pushes frames (lossless, stalls on credit);
//   * 2 WORKER lanes {try_acquire -> resume_if_ready -> drain -> close_out} (single-executor via the
//     exec-state CAS — the same ExecStateCell the Engine drains actors with);
//   * 1 CARRIER thread completes each parked async frame via the 015 TRANSFER (Parked->Scheduled, no
//     descriptor re-enqueue); the parked frame's continuation runs on the WORKER LANE (transferred,
//     not resumed on the carrier), and `tail` (credit) advances ONLY then.
// = 4 threads total (machine-safety cap). Suspend is biased to the ring's wrap boundary (deterministic
// splitmix64 schedule) so the producer laps the parked slot — the overwrite race is ATTEMPTED, not
// merely possible. BOUNDED frame count (a few x10^5 — decisive, not ADR-014's 10^7 which risks the box).
//
// Oracles (per-frame monotone id + checksum + FIFO): lost==dup==torn==fifo_violations==0, max
// descriptor membership==1 (no double-enqueue/orphan), max running executors==1 (single-executor),
// credit returned ONLY for COMPLETED frames (tail_final==N; credit_for_parked_not_completed==0).
//
// CONTROLS (separate -D targets, self-checking — exit 0 iff the defect FIRED):
//   QUARK_STREAM_SINGLE_CURSOR  — collapse disp/tail (credit at dispatch): credit_for_parked>0 (and
//                                 tears/loses via the overwrite it enables) — ADR-014 CONTROL-4.
//   QUARK_STREAM_REENQUEUE      — re-enqueue + inline carrier resume instead of transfer: dup>0 /
//                                 membership>1 / running>1 (two executors) — ADR-014 CONTROL-5.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <thread>
#include <vector>

#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"

using namespace quark;

namespace {

constexpr std::uint64_t kN = 300'000;   // BOUNDED (decisive; NOT the 10^7 that risks the machine)
constexpr std::uint32_t kCap = 256;
constexpr std::uint32_t kBudget = 32;
constexpr std::uint64_t kSeed = 0xc0ffee24ULL;  // the ADR-014 suspend-schedule seed

struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};
static_assert(sizeof(Frame) <= kStreamInlineMax);

// Deterministic suspend schedule, biased to the ring WRAP boundary (where the producer laps the
// parked slot — the hazardous overwrite window). ~1/16 of frames suspend, plus every near-wrap frame.
[[nodiscard]] bool is_async(std::uint64_t id) noexcept {
    if ((id & (kCap - 1)) >= kCap - 4) return true;      // near the wrap boundary
    return (detail::splitmix64(id ^ kSeed) & 0xF) == 0;  // deterministic ~1/16 elsewhere
}

// The per-frame stream handler + exactly-once oracle. on_frame/on_resume run ONLY on a worker lane
// under the single-executor seal, so the plain (non-atomic) oracle fields are safe across lane
// handoffs exactly as in exec_single_executor_test (the exec-state release/acquire is the happens-
// before). `processed` is atomic only so the main thread can poll for termination.
struct Oracle {
    std::vector<std::uint8_t> seen;
    std::uint64_t expected = 0;
    std::uint64_t dup = 0, torn = 0, fifo = 0, inline_completed = 0, async_completed = 0;
    std::atomic<std::uint64_t> processed{0};

    explicit Oracle(std::uint64_t n) : seen(n, 0) {}

    void count(const Frame& f) noexcept {
        if (f.checksum != detail::splitmix64(f.id)) ++torn;
        if (f.id != expected) ++fifo;
        expected = f.id + 1;
        if (f.id < seen.size()) {
            if (seen[f.id]) ++dup; else seen[f.id] = 1;
        }
        processed.fetch_add(1, std::memory_order_release);
    }

    // Returns Suspended for async frames (deferring the oracle count to on_resume); Completed inline
    // otherwise (counting now, credit returns immediately).
    [[nodiscard]] FrameVerdict on_frame(const Frame& f, std::uint64_t /*idx*/) noexcept {
        if (is_async(f.id)) return FrameVerdict::Suspended;
        count(f);
        ++inline_completed;
        return FrameVerdict::Completed;
    }
    // The 015 completion continuation, run on the LANE: re-read the (pinned) parked slot and count it.
    // A correct split cursor pins the slot (no overwrite); CONTROL-4 credits it early and the producer
    // overwrites it -> the re-read tears/loses.
    void on_resume(const Frame& f, std::uint64_t /*idx*/) noexcept {
        count(f);
        ++async_completed;
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
    bool control = false;
#if defined(QUARK_STREAM_SINGLE_CURSOR) || defined(QUARK_STREAM_REENQUEUE)
    control = true;
#endif

    StreamActivation<Frame>::Config cfg;
    cfg.capacity = kCap;
    cfg.low_watermark = kCap / 4;
    std::pmr::monotonic_buffer_resource mr;
    StreamActivation<Frame> sa(cfg, &mr);
    Oracle oracle(kN);

    result<StreamRef<Frame>> ref = open_stream(sa);
    if (!ref.has_value()) {
        std::fprintf(stderr, "bind failed\n");
        return 1;
    }

    std::atomic<bool> stop{false};

    // 2 WORKER lanes — the real 002 drain loop (single-executor via the exec-state CAS).
    std::vector<std::jthread> workers;
    for (int w = 0; w < 2; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                if (!sa.try_acquire()) {           // Scheduled -> Running (at most one worker wins)
                    std::this_thread::yield();
                    continue;
                }
                for (;;) {
                    sa.resume_if_ready(oracle);     // drain a carrier-transferred completion, on the lane
                    const StreamDrainOutcome o = sa.drain(kBudget, oracle);
                    if (o == StreamDrainOutcome::DrainedEmpty) {
                        if (sa.close_out()) continue;  // re-acquired: raced work
                        break;                          // relinquished to Idle
                    }
                    if (o == StreamDrainOutcome::BudgetExhausted) {
                        sa.yield_to_scheduled();       // Running -> Scheduled (fairness)
                        break;
                    }
                    break;  // Suspended: parked; the carrier will transfer it back
                }
            }
        });
    }

    // 1 CARRIER thread — the 015 completion: transfer each parked frame back (Parked -> Scheduled)
    // WITHOUT re-enqueuing the readiness. The only transition out of Parked is this readmit, so it
    // always succeeds once we observe Parked.
    std::jthread carrier([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (sa.parked()) {
                (void)sa.carrier_complete(oracle);  // ready_ publish + Parked->Scheduled transfer
            } else {
                std::this_thread::yield();
            }
        }
    });

    // 1 PRODUCER thread — lossless push (stalls on credit; never sheds). A CONTROL defect can WEDGE
    // the producer (QUARK_STREAM_REENQUEUE double-advances `tail` on the carrier AND the lane, so the
    // unsigned credit `capacity-(head-tail)` underflows and `push_blocking` parks forever on corrupted
    // credit): so we must NOT join the producer before the watchdog — a wedged producer is un-joinable.
    std::atomic<bool> producer_done{false};
    std::jthread producer([&] {
        for (std::uint64_t id = 0; id < kN; ++id) {
            ref->push_blocking(Frame{id, detail::splitmix64(id)});
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Progress-sampling wedge watchdog (no wall clock — a spin budget on the CONSUMED count). Clean
    // completion == every frame consumed (processed >= N). A wedge (control defect firing, or a real
    // regression) == the consumed count stops advancing for the whole budget while N is unreached. This
    // fires WITHOUT joining the (possibly wedged) producer — the old harness join()ed it first and hung.
    constexpr std::uint64_t kNoProgressLimit = 1'000'000'000ULL;
    std::uint64_t last = 0, idle = 0;
    bool wedged = false;
    for (;;) {
        const std::uint64_t p = oracle.processed.load(std::memory_order_acquire);
        if (p >= kN) break;                       // all frames consumed -> clean completion
        if (p != last) { last = p; idle = 0; }    // progress -> reset the stall counter
        else if (++idle > kNoProgressLimit) { wedged = true; break; }
    }
    stop.store(true, std::memory_order_release);

    if (wedged) {
        // The producer is parked in push_blocking on corrupted credit and cannot be joined; the worker/
        // carrier threads are still spinning. Print the verdict and _Exit so the process terminates
        // deterministically (the parked producer consumes ~0 CPU; _Exit skips the un-joinable dtors).
        // Reachable only for a CONTROL whose defect wedges (-> FIRED, pass) or a real regression (-> FAIL).
        std::printf("stream_exactly_once_suspend_test%s: %s  (N=%" PRIu64 " WEDGED — producer parked on "
                    "corrupted credit; max_membership=%d max_running=%d credit_for_parked=%" PRIu64 ")\n",
                    control ? " [CONTROL]" : "", control ? "FIRED" : "FAIL", kN,
                    sa.max_descriptor_membership(), sa.max_running_executors(), sa.credit_for_parked());
        std::fflush(stdout);
        std::_Exit(control ? 0 : 1);  // a control that FIRED (via wedge) passes; a wedged real path fails
    }

    // Clean completion: every frame consumed, so the producer pushed them all and is joinable.
    producer.join();
    carrier.join();
    for (auto& t : workers) t.join();

    std::uint64_t missing = 0;
    for (std::uint64_t i = 0; i < kN; ++i)
        if (!oracle.seen[i]) ++missing;
    const std::uint64_t lost = missing;
    const int max_membership = sa.max_descriptor_membership();
    const int max_running = sa.max_running_executors();
    const std::uint64_t credit_for_parked = sa.credit_for_parked();
    const std::uint64_t tail_final = sa.tail();

    const bool clean = !wedged && lost == 0 && oracle.dup == 0 && oracle.torn == 0 && oracle.fifo == 0 &&
                       max_membership <= 1 && max_running <= 1 && credit_for_parked == 0 &&
                       tail_final == kN;

    std::printf("stream_exactly_once_suspend_test%s: %s  (N=%" PRIu64 " lost=%" PRIu64 " dup=%" PRIu64
                " torn=%" PRIu64 " fifo=%" PRIu64 " max_membership=%d max_running=%d credit_for_parked=%"
                PRIu64 " tail_final=%" PRIu64 " inline=%" PRIu64 " async=%" PRIu64 " wedged=%d)\n",
                control ? " [CONTROL]" : "",
                (control ? (clean ? "NOT-FIRED" : "FIRED") : (clean ? "OK" : "FAIL")), kN, lost,
                oracle.dup, oracle.torn, oracle.fifo, max_membership, max_running, credit_for_parked,
                tail_final, oracle.inline_completed, oracle.async_completed, wedged ? 1 : 0);

    if (control) {
        // A control PASSES iff it FIRED (corrupted an oracle / wedged) — proving the detector's teeth.
        const bool fired = !clean;
        return fired ? 0 : 1;
    }
    return clean ? 0 : 1;
}
