// Tests 006 §Publish/Subscribe (ordered + reliable fan-out) / ADR-039 C3 (Block delivers gap-free
// identical history; producer stall bounded by the slowest LIVE subscriber) + S1 (the departure-wake:
// a producer parked on a lane that then unsubscribes is released, not left hanging forever) for
// `FanOut<M, OnSlowSubscriber<Block>>`.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "quark/core/fanout.hpp"

using namespace quark;

namespace {
struct Ev { std::uint64_t seq; };
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // ---- (A) gap-free delivery under a slow-draining (but never departing) subscriber -------------
    {
        constexpr std::uint64_t kK = 20'000;
        using FO = FanOut<Ev, OnSlowSubscriber<Block>>;
        FO fo(64);  // small lane -> the publisher WILL stall against the slow drainer repeatedly
        auto lane = fo.subscribe(ActorId{{1}, 1});
        check(lane != nullptr, "subscribe", ok);

        std::atomic<bool> stop_drain{false};
        std::atomic<std::uint64_t> received{0};
        std::uint64_t next_expected = 0;
        bool order_ok = true;
        std::thread drainer([&] {
            Ev out;
            while (!stop_drain.load(std::memory_order_relaxed)) {
                if (lane->try_pop(out)) {
                    if (out.seq != next_expected) order_ok = false;
                    ++next_expected;
                    received.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));  // deliberately slow
                }
            }
            // drain whatever is left
            while (lane->try_pop(out)) {
                if (out.seq != next_expected) order_ok = false;
                ++next_expected;
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::uint32_t departed_total = 0;
        for (std::uint64_t k = 0; k < kK; ++k) {
            auto r = fo.publish(Ev{k});  // may BLOCK until the drainer makes room
            departed_total += r.departed;
        }
        stop_drain.store(true, std::memory_order_relaxed);
        drainer.join();

        check(departed_total == 0, "no departures observed (subscriber never unsubscribed)", ok);
        check(received.load() == kK, "C3: every published message delivered (gap-free) under Block", ok);
        check(order_ok, "C3: FIFO holds under Block despite repeated producer stalls", ok);
        check(fo.in_flight() == 0, "no publish left in flight at quiescence", ok);
    }

    // ---- (B) departure-wake: unsubscribing a lane the producer is currently PARKED on releases it --
    {
        using FO = FanOut<Ev, OnSlowSubscriber<Block>>;
        FO fo(1);  // capacity 1 -> the second publish() will block on this lane
        auto lane = fo.subscribe(ActorId{{1}, 1});
        check(lane != nullptr, "subscribe", ok);

        auto r0 = fo.publish(Ev{0});
        check(r0.delivered == 1, "first publish fills the 1-slot lane", ok);

        std::atomic<bool> returned{false};
        std::atomic<std::uint32_t> departed{0};
        std::thread producer([&] {
            auto r = fo.publish(Ev{1});  // the lane is full -> this call PARKS
            departed.store(r.departed, std::memory_order_release);
            returned.store(true, std::memory_order_release);
        });

        // Give the producer a real chance to actually enter the parked state before we unsubscribe.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!returned.load(std::memory_order_acquire), "producer is genuinely parked, not racing ahead", ok);

        fo.unsubscribe(ActorId{{1}, 1});  // must release the parked producer (S1 departure-wake)

        const bool woke = wait_until([&] { return returned.load(std::memory_order_acquire); },
                                      std::chrono::seconds(2));
        check(woke, "S1: a producer parked on a departing lane is released, not left hanging forever", ok);
        if (woke) {
            check(departed.load() == 1, "the parked publish() reports this lane as departed, not delivered", ok);
            producer.join();
        } else {
            // Do NOT join a genuinely hung thread — that would hang this test process too. Report and
            // exit; the thread is leaked on process exit, which is acceptable for a failing test.
            producer.detach();
        }

        Ev out;
        check(lane->try_pop(out) && out.seq == 0, "the first (delivered) message is still drainable", ok);
        check(!lane->try_pop(out), "nothing further was queued after departure", ok);
    }

    std::fprintf(stderr, "fanout_block_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
