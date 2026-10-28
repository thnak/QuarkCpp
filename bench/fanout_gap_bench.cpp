// ADR-056 evidence: cost of gap-notice Design A (consumer-derived from id discontinuity, shipped) vs
// Design B (producer-stamped gap fields in a 32 B envelope). Single-thread push/pop cycle on one lane,
// two regimes: healthy (drain each push) and overflowing (drain every 8th push, cap 4).
// B omits payload refcounting, so it is a LOWER BOUND on B's cost; A is measured through the real FanOut.
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "quark/core/fanout.hpp"
#include "quark/core/stream_channel.hpp"
using namespace quark;
using clk = std::chrono::steady_clock;
struct Ev {
    std::uint64_t s;
};
struct EnvB {
    void* p;
    std::uint64_t id;
    std::uint64_t gap_first;
    std::uint64_t gap_count;
};
static_assert(sizeof(FanOutEnvelope<Ev>) == 16);
volatile std::uint64_t sink;
constexpr std::uint64_t N = 5'000'000;

template <class F>
double run(F&& f) {
    auto t0 = clk::now();
    f();
    return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / N;
}

int main() {
    std::printf("sizeof envelope: A=%zu B=%zu\n", sizeof(FanOutEnvelope<Ev>), sizeof(EnvB));
    for (std::uint64_t drain_every : {1ull, 8ull}) {
        using FO = FanOut<Ev, OnSlowSubscriber<EvictAfter<4>>>;
        double res[4];
        for (int mode = 0; mode < 4; ++mode) {
            double best = 1e18;
            for (int rep = 0; rep < 5; ++rep) {
                double ns;
                std::uint64_t acc = 0;
                if (mode < 2) {
                    FO fo;
                    auto lane = fo.subscribe(ActorId{{1}, 1});
                    ns = run([&] {
                        Ev e;
                        FanOutGap g;
                        for (std::uint64_t i = 0; i < N; ++i) {
                            (void)fo.publish(Ev{i});
                            if (i % drain_every == 0)
                                while (mode == 0 ? lane->try_pop(e) : lane->try_pop(e, g)) acc += e.s + g.count;
                        }
                    });
                } else if (mode == 3) {  // A-raw: 16 B env, consumer-derived gap, no refcount
                    StreamChannel<FanOutEnvelope<Ev>> ch({4}, std::pmr::get_default_resource());
                    ns = run([&] {
                        std::uint64_t next = 0;
                        for (std::uint64_t i = 0; i < N; ++i) {
                            (void)ch.try_push(FanOutEnvelope<Ev>{nullptr, MessageId{i}});
                            if (i % drain_every == 0)
                                while (ch.occupancy() > 0) {
                                    auto idx = ch.advance_dispatch();
                                    auto x = ch.slot_at(idx);
                                    if (x.id.value != next) acc += x.id.value - next;
                                    next = x.id.value + 1;
                                    acc += x.id.value;
                                    ch.advance_tail();
                                    ch.poll_unstall();
                                }
                        }
                    });
                } else {
                    StreamChannel<EnvB> ch({4}, std::pmr::get_default_resource());
                    std::uint64_t pend_first = 0, pend = 0;
                    ns = run([&] {
                        for (std::uint64_t i = 0; i < N; ++i) {
                            EnvB e{nullptr, i, pend_first, pend};
                            if (ch.try_push(e)) pend = 0;
                            else {
                                if (!pend) pend_first = i;
                                ++pend;
                            }
                            if (i % drain_every == 0)
                                while (ch.occupancy() > 0) {
                                    auto idx = ch.advance_dispatch();
                                    EnvB x = ch.slot_at(idx);
                                    acc += x.id + x.gap_count;
                                    ch.advance_tail();
                                    ch.poll_unstall();
                                }
                        }
                    });
                }
                sink = acc;
                if (ns < best) best = ns;
            }
            res[mode] = best;
        }
        std::printf("drain_every=%llu publish+pop ns/op: A-old-pop=%.2f A-gap-pop=%.2f A-raw(16B,derived)=%.2f B-raw(32B,stamped)=%.2f\n",
                    static_cast<unsigned long long>(drain_every), res[0], res[1], res[3], res[2]);
    }
}
