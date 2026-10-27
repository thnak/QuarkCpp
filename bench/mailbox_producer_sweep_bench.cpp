// Implements issue #4 / ADR-057 — N-producer -> 1-consumer mailbox throughput sweep.
// Reproduces (or fails to reproduce) the "contention collapse under many producers" report on the
// machine it runs on, at three levels, so the collapse can be attributed:
//   raw   : shared Mailbox, per-producer preallocated descriptors (only tail_.exchange contended).
//   pool  : real send-path building blocks — MessagePool::acquire() + Mailbox::enqueue (producers),
//           try_dequeue + MessagePool::reclaim (single consumer). No actors/scheduler.
//   batch : bench-local copy of the Vyukov queue where a producer links K nodes privately and
//           publishes them with ONE exchange (mitigation experiment; NOT in mailbox.hpp).
// Every run verifies per-(sender,receiver) FIFO and exactly-once delivery. Informational: exit 0
// unless a correctness check fails. Median of kReps runs is printed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/detail/message_pool.hpp"
#include "pal/pal.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace quark;

namespace {
constexpr int kReps = 5;
bool g_fail = false;

void pin(std::thread& t, int cpu, bool enable) {
#if defined(_WIN32)
    if (enable) SetThreadAffinityMask(t.native_handle(), DWORD_PTR{1} << cpu);
#else
    (void)t; (void)cpu; (void)enable;
#endif
}

// Local Vyukov queue with chain publish (bench-only experiment).
class BatchQueue {
public:
    BatchQueue() noexcept : head_(&stub_), tail_(&stub_) { stub_.link.next.store(nullptr, std::memory_order_relaxed); }
    void push_chain(Descriptor* first, Descriptor* last) noexcept {
        last->link.next.store(nullptr, std::memory_order_relaxed);
        Descriptor* prev = tail_.exchange(last, std::memory_order_acq_rel);
        prev->link.next.store(first, std::memory_order_release);
    }
    Descriptor* pop() noexcept {  // nullptr == empty/busy
        Descriptor* front = head_;
        Descriptor* next = front->link.next.load(std::memory_order_acquire);
        if (front == &stub_) {
            if (!next) return nullptr;
            head_ = next; front = next; next = front->link.next.load(std::memory_order_acquire);
        }
        if (next) { head_ = next; return front; }
        if (front != tail_.load(std::memory_order_acquire)) return nullptr;
        push_chain(&stub_, &stub_);
        next = front->link.next.load(std::memory_order_acquire);
        if (next) { head_ = next; return front; }
        return nullptr;
    }
private:
    Descriptor* head_;
    QUARK_CACHE_ALIGNED std::atomic<Descriptor*> tail_;
    QUARK_CACHE_ALIGNED Descriptor stub_;
};

enum class Mode { Raw, Pool, PoolPart, Batch };
constexpr std::uint64_t kWindow = 4096;  // per-producer in-flight bound for pool modes (keeps cells cache-resident)
struct alignas(64) Counter { std::atomic<std::uint64_t> v{0}; };

double run(Mode mode, unsigned P, std::uint64_t total, unsigned K, bool do_pin) {
    const std::uint64_t per = total / P;
    const std::uint64_t expected = per * P;
    std::vector<std::vector<Descriptor>> arrays;
    std::unique_ptr<detail::MessagePool> pool;
    std::vector<Counter> reclaimed(P);
    if (mode == Mode::Pool || mode == Mode::PoolPart) {
        const std::size_t parts = mode == Mode::PoolPart ? P : 1;
        pool = std::make_unique<detail::MessagePool>(
            (parts == 1 ? 1 : 3) * P * kWindow + 64 * (P + 2) + 512, parts);
    } else {
        arrays.resize(P);
        for (auto& a : arrays) a = std::vector<Descriptor>(per);
    }
    Mailbox mb;
    BatchQueue bq;
    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0};
    std::atomic<std::uint64_t> drained{0};
    std::atomic<bool> fifo_ok{true};

    std::vector<std::thread> prod;
    for (unsigned p = 0; p < P; ++p) {
        prod.emplace_back([&, p] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) {}
            if (mode == Mode::Raw) {
                for (std::uint64_t i = 0; i < per; ++i) {
                    Descriptor* d = &arrays[p][i];
                    d->message_id = MessageId{(std::uint64_t{p} << 40) | i};
                    mb.enqueue(d);
                }
            } else if (mode == Mode::Pool || mode == Mode::PoolPart) {
                for (std::uint64_t i = 0; i < per; ++i) {
                    while (i - reclaimed[p].v.load(std::memory_order_acquire) >= kWindow) {}
                    auto s = pool->acquire(nullptr);
                    s.desc->message_id = MessageId{(std::uint64_t{p} << 40) | i};
                    mb.enqueue(s.desc);
                }
                detail::flush_current_thread_message_caches();
            } else {
                std::uint64_t i = 0;
                while (i < per) {
                    const unsigned n = static_cast<unsigned>(std::min<std::uint64_t>(K, per - i));
                    Descriptor* first = &arrays[p][i];
                    for (unsigned j = 0; j < n; ++j) {
                        Descriptor* d = &arrays[p][i + j];
                        d->message_id = MessageId{(std::uint64_t{p} << 40) | (i + j)};
                        if (j + 1 < n) d->link.next.store(&arrays[p][i + j + 1], std::memory_order_relaxed);
                    }
                    bq.push_chain(first, &arrays[p][i + n - 1]);
                    i += n;
                }
            }
        });
        pin(prod.back(), static_cast<int>(p) + 1, do_pin);
    }
    std::thread cons([&] {
        std::vector<std::uint64_t> next_seq(P, 0);
        std::uint64_t n = 0;
        while (n < expected) {
            Descriptor* d = nullptr;
            if (mode == Mode::Batch) d = bq.pop();
            else { DrainResult r = mb.try_dequeue(); if (r.status == DrainStatus::Message) d = r.desc; }
            if (!d) continue;
            const std::uint64_t id = d->message_id.value;
            const unsigned p = static_cast<unsigned>(id >> 40);
            if (p >= P || (id & ((std::uint64_t{1} << 40) - 1)) != next_seq[p]++) fifo_ok.store(false);
            if (pool) { pool->reclaim(d); if ((next_seq[p] & 31) == 0) reclaimed[p].v.store(next_seq[p], std::memory_order_release); }
            ++n;
        }
        drained.store(n);
        if (pool) detail::flush_current_thread_message_caches();
    });
    pin(cons, 0, do_pin);
    while (ready.load() < P) std::this_thread::yield();
    const auto t0 = pal::clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : prod) t.join();
    cons.join();
    const auto t1 = pal::clock::now();
    if (!fifo_ok.load() || drained.load() != expected) {
        std::printf("  [FAIL] FIFO/exactly-once violated (mode=%d P=%u)\n", static_cast<int>(mode), P);
        g_fail = true;
    }
    return static_cast<double>(expected) / std::chrono::duration<double>(t1 - t0).count() / 1e6;
}

double median(Mode m, unsigned P, std::uint64_t total, unsigned K, bool pin_) {
    std::vector<double> v;
    for (int i = 0; i < kReps; ++i) v.push_back(run(m, P, total, K, pin_));
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
}  // namespace

int main() {
    const unsigned hw = std::thread::hardware_concurrency();
    std::printf("== mailbox N-producer -> 1-consumer sweep (issue #4 / ADR-057) ==\n");
    std::printf("logical cores=%u (consumer pinned to cpu0, producers cpu1.. while P<=%u; beyond that unpinned/oversubscribed)\n",
                hw, hw - 1);
    const std::vector<unsigned> sweep = {1, 2, 4, 8, 11, 16};
    std::printf("median of %d runs, M msg/s aggregate (consumer-side wall clock)\n", kReps);
    std::printf("%-4s %10s %10s %10s %10s %10s %10s\n", "P", "raw", "pool", "pool/part", "batch8", "batch32", "raw/P1");
    double raw1 = 0;
    for (unsigned P : sweep) {
        const bool pn = P + 1 <= hw;
        const double raw = median(Mode::Raw, P, 4'000'000, 1, pn);
        const double pl = median(Mode::Pool, P, 2'000'000, 1, pn);
        const double pp = median(Mode::PoolPart, P, 2'000'000, 1, pn);
        const double b8 = median(Mode::Batch, P, 4'000'000, 8, pn);
        const double b32 = median(Mode::Batch, P, 4'000'000, 32, pn);
        if (P == 1) raw1 = raw;
        std::printf("%-4u %10.2f %10.2f %10.2f %10.2f %10.2f %9.2fx\n", P, raw, pl, pp, b8, b32, raw / raw1);
    }
    return g_fail ? 1 : 0;
}
