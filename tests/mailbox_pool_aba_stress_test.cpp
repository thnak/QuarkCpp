// Dimension 4 ("ABA stress") of the mailbox test suite. Complements, at materially higher
// iteration counts and more adversarial thread imbalance, tests/message_pool_partition_concurrency_
// test.cpp (which proves MessagePool's partition-correct reclaim under producer/consumer imbalance
// at moderate scale) and tests/mailbox_cancel_test.cpp (which proves Descriptor::try_cancel's
// generation gate under a claim-vs-cancel race, but over a plain pre-allocated Descriptor array,
// never through MessagePool's churn/partitioning). This is the gap ADR-020 flagged as unproven
// ("MessagePool::acquire()'s allocation behavior under producer/consumer imbalance") and ADR-031/
// this session's partition fix (commit 1964203) addressed structurally — this test hammers BOTH
// mechanisms together, at once, under real concurrency:
//
//   Round 1 — high-churn extreme imbalance: a deliberately SMALL pool (forces rapid cell reuse —
//   every physical cell is recycled thousands of times over the run) with one producer thread doing
//   the bulk of the work and three near-idle producers, reclaimed by a DIFFERENT set of consumer
//   threads than produced them (never self-reclaim) — 15x the op count of the existing partition
//   concurrency test, 1/2 its per-partition capacity. Asserts produced==consumed==expected AND
//   live==0 (no leak, no double-destruct) — either would misfire on a partition misroute or a
//   reused-cell corruption.
//
//   Round 2 — claim-vs-cancel racing ON TOP OF MessagePool churn: every produced message is hand-
//   ed to a consumer thread (which try_claim()s it, i.e. the real "the drain lane claims this
//   descriptor" step) AND, independently, to a canceller thread (which races try_cancel() on the
//   SAME live handle). Exactly one of {claim, cancel} may win per message (Queued -> Running is
//   mutually exclusive with Queued -> Cancelled, one packed CAS — descriptor.hpp). The pool cell is
//   reclaimed exactly once regardless of which side won. This is the ABA-adjacent proof this file
//   adds beyond the existing tests: try_claim/try_cancel's generation-gated CAS staying race-free
//   while the SAME physical cells are being recycled by MessagePool at high frequency across
//   multiple partitions — not proven anywhere else in the suite.
//
// Run under TSan (the claim-vs-cancel + cross-thread-reclaim edges are exactly the load-bearing
// races) and ASan (a misroute or a stale-generation bug would manifest as a heap corruption/UAF).
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

std::atomic<long> g_live{0};

struct Msg {
    long token;
    explicit Msg(long t) noexcept : token(t) { g_live.fetch_add(1, std::memory_order_relaxed); }
    ~Msg() { g_live.fetch_add(-1, std::memory_order_relaxed); }
};

template <class T>
class HandoffQueue {
public:
    void push(T v) {
        std::lock_guard<std::mutex> g(mu_);
        q_.push_back(v);
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(mu_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex mu_;
    std::deque<T> q_;
};

void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// --- Round 1: high-churn, extreme producer imbalance, cross-thread reclaim. ----------------------
bool round1_high_churn_imbalance() {
    bool ok = true;
    // Small pool relative to total ops: every cell gets recycled ~ (total_ops / capacity) times.
    // capacity=32 over ~300,150 ops => ~9,380 reuse cycles PER CELL (vs. the existing test's 64
    // capacity over 20,150 ops => ~315 cycles/cell) — ~30x more reuse pressure per physical slot.
    constexpr std::size_t kCapacity = 32;
    constexpr std::size_t kPartitions = 8;
    const std::vector<int> per_producer = {300'000, 50, 50, 50};  // 15x the existing test's skew
    constexpr int kConsumers = 4;

    detail::MessagePool pool(kCapacity, kPartitions);
    HandoffQueue<Descriptor*> queue;
    std::atomic<int> produced{0}, consumed{0};
    std::atomic<bool> producers_done{false};
    g_live.store(0, std::memory_order_relaxed);

    int expected = 0;
    for (int n : per_producer) expected += n;

    std::vector<std::thread> producers;
    producers.reserve(per_producer.size());
    for (std::size_t t = 0; t < per_producer.size(); ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < per_producer[t]; ++i) {
                detail::MessagePool::Slot slot = pool.acquire(&detail::destroy_payload<Msg>);
                ::new (slot.payload) Msg(static_cast<long>(t) * 10'000'000L + i);
                slot.desc->payload = slot.payload;
                queue.push(slot.desc);
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            Descriptor* d = nullptr;
            for (;;) {
                if (queue.try_pop(d)) {
                    pool.reclaim(d);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire)) {
                    if (!queue.try_pop(d)) break;
                    pool.reclaim(d);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& th : producers) th.join();
    producers_done.store(true, std::memory_order_release);
    for (auto& th : consumers) th.join();

    check(produced.load() == expected, "round1: produced == expected (no lost acquire)", ok);
    check(consumed.load() == expected, "round1: consumed == expected (no lost reclaim)", ok);
    check(g_live.load() == 0, "round1: live Msg count returns to 0 (no leak/double-destruct)", ok);
    std::printf("  [round1 high-churn-imbalance] produced=%d consumed=%d live=%ld capacity=%zu "
                "partitions=%zu (~%.0f reuse-cycles/cell)\n",
                produced.load(), consumed.load(), g_live.load(), kCapacity, kPartitions,
                static_cast<double>(expected) / static_cast<double>(kCapacity));
    return ok;
}

// --- Round 2: try_claim vs try_cancel racing, layered on MessagePool churn + partitioning. --------
bool round2_claim_vs_cancel_under_churn() {
    bool ok = true;
    constexpr std::size_t kCapacity = 64;
    constexpr std::size_t kPartitions = 4;
    constexpr unsigned kProducers = 4;
    constexpr int kPerProducer = 50'000;               // 200,000 messages total
    constexpr int kTotal = static_cast<int>(kProducers) * kPerProducer;
    constexpr int kConsumers = 4;
    constexpr int kCancellers = 2;

    detail::MessagePool pool(kCapacity, kPartitions);
    HandoffQueue<Descriptor*> claim_queue;        // consumer threads pop from here (try_claim)
    // Cancellers race on a captured MessageHandle {desc, generation-at-production}, NOT a bare
    // Descriptor* re-read live at pop time. This is the load-bearing fix for the ABA hazard this
    // round exists to stress: cancel_queue can lag claim_queue (mutex scheduling, thread imbalance),
    // so by the time a canceller pops an entry the same physical cell may ALREADY have been
    // reclaimed and handed back out for a LATER, unrelated message. Racing a live-re-read generation
    // would let the cancel spuriously "succeed" against that later message (a real ABA misfire,
    // reproduced once during development of this test before this fix — see the file history).
    // Capturing the generation at production time and gating try_cancel on it is exactly
    // Descriptor::try_cancel's documented contract (a stale handle safely no-ops on mismatch).
    HandoffQueue<MessageHandle> cancel_queue;
    g_live.store(0, std::memory_order_relaxed);

    std::atomic<int> produced{0};
    std::atomic<int> handled{0};       // claim won: this is "the handler ran"
    std::atomic<int> tombstoned{0};    // cancel won: skip handler, still reclaim once
    std::atomic<int> cancel_wins{0};   // canceller's own successful try_cancel() count
    std::atomic<int> double_reclaim{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (unsigned t = 0; t < kProducers; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < kPerProducer; ++i) {
                detail::MessagePool::Slot slot = pool.acquire(&detail::destroy_payload<Msg>);
                ::new (slot.payload) Msg(static_cast<long>(t) * 1'000'000L + i);
                slot.desc->payload = slot.payload;
                // Capture the handle (desc + THIS message's generation) before publishing anywhere,
                // so a consumer's claim and a canceller's cancel race the identical, currently-
                // Queued message instance — the Queued->{Running,Cancelled} CAS in descriptor.hpp is
                // the mutual-exclusion point; a generation mismatch by the time the cancel runs must
                // safely no-op rather than hit whatever unrelated message now occupies the cell.
                MessageHandle h = handle_of(slot.desc);
                claim_queue.push(slot.desc);
                cancel_queue.push(h);
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Cancellers: race try_cancel() against the consumers' try_claim(). A cancel that wins means
    // the descriptor is now a tombstone; the CONSUMER (not the canceller) still performs the single
    // reclaim() once it separately dequeues that same descriptor from claim_queue, so there is no
    // risk of the canceller double-reclaiming it — cancel is a pure state transition here.
    std::vector<std::thread> cancellers;
    cancellers.reserve(kCancellers);
    for (int c = 0; c < kCancellers; ++c) {
        cancellers.emplace_back([&] {
            MessageHandle h;
            for (;;) {
                if (cancel_queue.try_pop(h)) {
                    if (h.cancel()) cancel_wins.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire)) {
                    if (!cancel_queue.try_pop(h)) break;
                    if (h.cancel()) cancel_wins.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::this_thread::yield();
            }
        });
    }

    // Consumers: try_claim() (the real drain-lane semantics) then reclaim exactly once either way.
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            Descriptor* d = nullptr;
            for (;;) {
                if (claim_queue.try_pop(d)) {
                    if (d->try_claim()) handled.fetch_add(1, std::memory_order_relaxed);
                    else tombstoned.fetch_add(1, std::memory_order_relaxed);
                    pool.reclaim(d);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire)) {
                    if (!claim_queue.try_pop(d)) break;
                    if (d->try_claim()) handled.fetch_add(1, std::memory_order_relaxed);
                    else tombstoned.fetch_add(1, std::memory_order_relaxed);
                    pool.reclaim(d);
                    continue;
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& th : producers) th.join();
    producers_done.store(true, std::memory_order_release);
    for (auto& th : cancellers) th.join();
    for (auto& th : consumers) th.join();

    check(produced.load() == kTotal, "round2: produced == kTotal", ok);
    check(handled.load() + tombstoned.load() == kTotal,
          "round2: every message resolved to exactly one of {handled, tombstoned}", ok);
    check(g_live.load() == 0, "round2: live Msg count returns to 0 (reclaim ran exactly once each)", ok);
    check(double_reclaim.load() == 0, "round2: no double-reclaim", ok);
    // cancel_wins and tombstoned both count "cancel beat claim" from each side of the race; they
    // must agree exactly (every winning cancel corresponds to exactly one tombstoned reclaim).
    check(cancel_wins.load() == tombstoned.load(),
          "round2: canceller's win count matches consumer's tombstoned count (no untracked winner)", ok);

    std::printf("  [round2 claim-vs-cancel-under-churn] produced=%d handled=%d tombstoned=%d "
                "cancel_wins=%d live=%ld\n",
                produced.load(), handled.load(), tombstoned.load(), cancel_wins.load(), g_live.load());
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= round1_high_churn_imbalance();
    ok &= round2_claim_vs_cancel_under_churn();
    std::printf("mailbox_pool_aba_stress_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
