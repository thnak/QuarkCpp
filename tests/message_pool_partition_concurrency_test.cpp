// Proves MessagePool's partitioning (message_pool.hpp) is correct under producer/consumer
// imbalance — the exact gap ADR-020 flagged as "unproven": P producer threads acquire() cells
// (each constructing a live Msg tracked by a shared live-count); completed descriptors are handed
// off through a plain queue to C SEPARATE consumer threads that reclaim() them — deliberately NOT
// the acquiring thread, so a design that routed reclaim() by the RECLAIMING thread's own lane
// (instead of the cell's stamped-at-acquire `home` partition) would misroute across partitions.
// Run under TSan/ASan. Capped at 4 threads per role (machine-safety: never saturate cores).
#include <atomic>
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
    int value;
    explicit Msg(int v) noexcept : value(v) { g_live.fetch_add(1, std::memory_order_relaxed); }
    ~Msg() { g_live.fetch_add(-1, std::memory_order_relaxed); }
};

// A trivial thread-safe hand-off queue. Not the path under test (MessagePool is) — a mutex here
// is fine; it only needs to move a Descriptor* from an arbitrary producer to an arbitrary consumer.
class HandoffQueue {
public:
    void push(Descriptor* d) {
        std::lock_guard<std::mutex> g(mu_);
        q_.push_back(d);
    }
    Descriptor* try_pop() {
        std::lock_guard<std::mutex> g(mu_);
        if (q_.empty()) return nullptr;
        Descriptor* d = q_.front();
        q_.pop_front();
        return d;
    }

private:
    std::mutex mu_;
    std::deque<Descriptor*> q_;
};

// Run one round: `per_producer[t]` messages from producer thread t, drained by `consumers`
// consumer threads that are NEVER the producing thread. Returns true iff produced == consumed ==
// expected AND every constructed Msg was destructed (g_live back to 0).
bool run_round(const char* label, detail::MessagePool& pool, const std::vector<int>& per_producer,
               int consumers) {
    HandoffQueue queue;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> producers_done{false};
    g_live.store(0, std::memory_order_relaxed);

    const int expected = [&] {
        int sum = 0;
        for (int n : per_producer) sum += n;
        return sum;
    }();

    std::vector<std::thread> producers;
    producers.reserve(per_producer.size());
    for (std::size_t t = 0; t < per_producer.size(); ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < per_producer[t]; ++i) {
                detail::MessagePool::Slot slot = pool.acquire(&detail::destroy_payload<Msg>);
                ::new (slot.payload) Msg(static_cast<int>(t) * 1'000'000 + i);
                slot.desc->payload = slot.payload;
                queue.push(slot.desc);
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> consumer_threads;
    consumer_threads.reserve(static_cast<std::size_t>(consumers));
    for (int c = 0; c < consumers; ++c) {
        consumer_threads.emplace_back([&] {
            for (;;) {
                Descriptor* d = queue.try_pop();
                if (d != nullptr) {
                    pool.reclaim(d);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire)) {
                    d = queue.try_pop();
                    if (d == nullptr) break;
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
    for (auto& th : consumer_threads) th.join();

    bool ok = true;
    if (produced.load() != expected) {
        std::fprintf(stderr, "  [%s] CHECK FAILED: produced=%d expected=%d\n", label, produced.load(),
                     expected);
        ok = false;
    }
    if (consumed.load() != expected) {
        std::fprintf(stderr, "  [%s] CHECK FAILED: consumed=%d expected=%d\n", label, consumed.load(),
                     expected);
        ok = false;
    }
    if (g_live.load() != 0) {
        std::fprintf(stderr, "  [%s] CHECK FAILED: live=%ld (leaked or double-destructed Msg)\n", label,
                     g_live.load());
        ok = false;
    }
    std::printf("  [%s] produced=%d consumed=%d live=%ld : %s\n", label, produced.load(),
                consumed.load(), g_live.load(), ok ? "OK" : "FAIL");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::size_t kPartitions = 8;
    constexpr int kConsumers = 4;

    // Balanced: 4 producers, equal load, more producers/consumers than partitions (forces some
    // threads to share a partition — exercises acquire()-side contention within a partition too).
    {
        detail::MessagePool pool(64, kPartitions);
        ok &= run_round("balanced", pool, std::vector<int>{5000, 5000, 5000, 5000}, kConsumers);
    }

    // Imbalanced: one hot producer, several near-idle ones — the documented trade-off is that
    // partitions don't steal from each other, so this only needs to prove CORRECTNESS (every
    // acquire reclaimed to its own origin, none lost/double-freed), not any throughput/memory bound.
    {
        detail::MessagePool pool(64, kPartitions);
        ok &= run_round("imbalanced", pool, std::vector<int>{20000, 50, 50, 50}, kConsumers);
    }

    // Single partition (num_partitions=1, i.e. today's original behavior) must still be correct —
    // the additive default path is exercised by every other existing test, but prove it here too
    // under the same cross-thread-reclaim harness for a direct before/after comparison.
    {
        detail::MessagePool pool(64, 1);
        ok &= run_round("single-partition", pool, std::vector<int>{5000, 5000, 5000, 5000}, kConsumers);
    }

    std::printf("message_pool_partition_concurrency_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
