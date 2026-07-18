// Shared scaffolding for the 011 timer tests. Not a test itself (no `_test.cpp` suffix, so the
// CMake glob skips it). Stands up a minimal 1-worker/1-shard engine + a counting actor so a timer
// firing (a real `tell` through 006) can be observed on the actor's lane, exactly as in production.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

namespace timertest {

// The message a fired timer delivers. `id` lets a test witness ordering / exactly-once.
struct Fire {
    int id = 0;
};

// A flush sentinel (see Harness::flush): posted from the same thread that drove the timer fires, so
// per-(sender,receiver) FIFO (006) guarantees every earlier timer `tell` is delivered once it lands.
struct Sentinel {};

// A Sequential (single-executor) sink: `order` is written only on its one drain lane, so it is a
// faithful witness of arrival order; `count` is the cross-thread progress signal the harness spins on.
struct Counter : quark::Actor<Counter, quark::Sequential> {
    using protocol = quark::Protocol<Fire, Sentinel>;
    std::vector<int> order;
    std::atomic<int> count{0};
    std::atomic<int> flushes{0};

    void handle(const Fire& f) noexcept {
        order.push_back(f.id);
        count.fetch_add(1, std::memory_order_release);
    }
    void handle(const Sentinel&) noexcept { flushes.fetch_add(1, std::memory_order_release); }
};

// Owns the engine + actor + router and hands out a ref. One engine worker thread; timer tests add at
// most one timer driver thread on top — within the ≤4-core / ≤1-timer-thread machine-safety cap.
class Harness {
public:
    explicit Harness(std::size_t pool_capacity = 4096)
        : pool_(pool_capacity),
          act_(std::make_unique<quark::Activation>(&actor_, Counter::dispatch_table(), pool_.sink())),
          eng_(quark::EngineConfig{1, 1, 256, 64}),
          router_(eng_.post_courier(), pool_) {
        eng_.register_activation(quark::actor_id_of<Counter>(1), *act_);
        eng_.start();
    }

    ~Harness() { eng_.stop(); }

    [[nodiscard]] quark::ActorRef<Counter> ref() { return router_.get<Counter>(1); }
    [[nodiscard]] Counter& actor() noexcept { return actor_; }

    // Spin until `target` messages have been delivered, or a generous cap elapses. Returns true iff
    // the target was reached (no wall-clock sleep — pure progress spin, core-capped by taskset).
    [[nodiscard]] bool drain_until(int target) {
        constexpr std::uint64_t kCap = 2'000'000'000ULL;
        for (std::uint64_t s = 0; s < kCap; ++s) {
            if (actor_.count.load(std::memory_order_acquire) >= target) return true;
        }
        return actor_.count.load(std::memory_order_acquire) >= target;
    }

    [[nodiscard]] int delivered() const noexcept { return actor_.count.load(std::memory_order_acquire); }

    // Deterministically flush the mailbox: post a Sentinel from THIS thread and wait for it. Because
    // the deterministic tick()/advance() path fires timers (their `tell`s) on this same thread, FIFO
    // ordering (006) means once the Sentinel is delivered, every prior timer `tell` already is too —
    // so a subsequent `delivered()` read reflects a settled mailbox (no wall-clock guessing).
    void flush() {
        const int want = actor_.flushes.load(std::memory_order_acquire) + 1;
        ref().tell(Sentinel{});
        constexpr std::uint64_t kCap = 2'000'000'000ULL;
        for (std::uint64_t s = 0; s < kCap; ++s)
            if (actor_.flushes.load(std::memory_order_acquire) >= want) return;
    }

private:
    quark::detail::MessagePool pool_;
    Counter actor_;
    std::unique_ptr<quark::Activation> act_;
    quark::Engine<> eng_;
    quark::LocalRouter router_;
};

inline void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace timertest
