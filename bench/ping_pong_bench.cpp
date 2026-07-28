// Ping-pong round-trip latency benchmark — dimension 24 of the mailbox benchmark suite. Distinct
// from bench/ask_bench.cpp's request/reply round trip (which routes its reply through a pooled
// ReplyCell, ADR-007 §5 — a single-shot, generation-fenced rendezvous): this file is a TRUE
// two-actor ping-pong over plain `tell()` — Actor A tells Actor B, B tells back to A, repeat — the
// round trip a naive "two actors bouncing messages" workload actually produces, exercising TWO
// independent mailboxes/Activations each lap instead of one Ask envelope + one ReplyCell.
//
//  A) same-shard (both actors on shard 0, whichever worker drains each): the tighter-loop number.
//  B) cross-shard (A and B forced onto DIFFERENT shards, so a lap can cross worker lanes): the more
//     realistic multi-actor deployment shape; compare against A for the added scheduling cost.
//
// Pin it: `taskset -c 0-3 build/bench/ping_pong_bench` (2 shards need at least 2 worker lanes for
// section B to actually parallelize the two actors across lanes).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct Ball { std::uint64_t lap; pal::clock::time_point sent_at; };

// PongActor: bounces every ball straight back to whoever it came from. It needs a courier + the
// sender's ActorId to reply — carried inline in the message itself (a raw ActorRef isn't
// trivially-copyable-safe to embed generically here, so PongActor is templated per pairing below;
// simplest: give it a fixed reference to PingActor's ActorRef, wired after both are spawned).
struct PingActor;
struct PongActor;

struct PingActor : Actor<PingActor, Sequential> {
    using protocol = Protocol<Ball>;
    ActorRef<PongActor>* pong = nullptr;
    std::vector<double>* rtt_ns = nullptr;
    // Polled from the benchmark's driver thread while the ACTOR's drain lane (a different thread)
    // updates it — must be atomic, not a plain std::uint64_t*: a plain, unsynchronized cross-thread
    // spin-read is a data race that the optimizer is free to (and in practice does) hoist out of
    // the driver's busy-wait loop, turning a real progress signal into a permanent stall.
    std::atomic<std::uint64_t>* laps_done = nullptr;
    std::uint64_t total_laps = 0;

    void handle(const Ball& b) noexcept;  // defined after PongActor (needs its complete type)
};

struct PongActor : Actor<PongActor, Sequential> {
    using protocol = Protocol<Ball>;
    ActorRef<PingActor>* ping = nullptr;
    void handle(const Ball& b) noexcept { ping->tell(b); }  // bounce straight back, same lap
};

void PingActor::handle(const Ball& b) noexcept {
    const auto now = pal::clock::now();
    rtt_ns->push_back(std::chrono::duration<double, std::nano>(now - b.sent_at).count());
    const std::uint64_t done = laps_done->fetch_add(1, std::memory_order_release) + 1;
    if (done < total_laps) {
        Ball next{b.lap + 1, pal::clock::now()};
        pong->tell(next);
    }
}

double run_ping_pong(unsigned shard_count, const char* label, std::uint64_t warmup,
                     std::uint64_t laps) {
    detail::MessagePool pool(64, /*num_partitions=*/1);
    PingActor ping_actor;
    PongActor pong_actor;
    auto ping_act = std::make_unique<Activation>(&ping_actor, PingActor::dispatch_table(), pool.sink());
    auto pong_act = std::make_unique<Activation>(&pong_actor, PongActor::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{shard_count > 1 ? 2u : 1u, shard_count, 64, 64});
    // Keys chosen so, when shard_count>1, they typically land on different shards (ActorId hash);
    // for shard_count==1 they necessarily share shard 0 regardless of key.
    eng.register_activation(actor_id_of<PingActor>(1), *ping_act);
    eng.register_activation(actor_id_of<PongActor>(2), *pong_act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<PingActor> ping_ref = router.get<PingActor>(1);
    ActorRef<PongActor> pong_ref = router.get<PongActor>(2);

    ping_actor.pong = &pong_ref;
    pong_actor.ping = &ping_ref;
    std::vector<double> rtt_ns;
    rtt_ns.reserve(warmup + laps);
    std::atomic<std::uint64_t> laps_done{0};
    ping_actor.rtt_ns = &rtt_ns;
    ping_actor.laps_done = &laps_done;
    ping_actor.total_laps = warmup + laps;

    eng.start();
    ping_ref.tell(Ball{0, pal::clock::now()});  // kick off lap 0

    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (laps_done.load(std::memory_order_acquire) < warmup + laps) {
        if (++spins > kStall) {
            std::fprintf(stderr, "%s: STALL at lap %llu/%llu\n", label,
                         static_cast<unsigned long long>(laps_done.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(warmup + laps));
            eng.stop();
            return -1.0;
        }
    }
    eng.stop();

    std::vector<double> steady(rtt_ns.begin() + static_cast<long>(warmup), rtt_ns.end());
    bench::Stats s = bench::summarize(steady);
    std::printf("%s:\n", label);
    std::printf("  p50  = %8.1f ns\n", s.p50);
    std::printf("  p99  = %8.1f ns\n", s.p99);
    std::printf("  p999 = %8.1f ns\n", s.p999);
    std::printf("  mean = %8.1f ns   stddev = %.1f ns   CoV = %.3f   (n=%zu laps)\n", s.mean, s.stddev,
                s.cov, s.n);
    return s.p50;
}

}  // namespace

int main() {
    std::printf("== Quark ping-pong round-trip bench (dim 24; pin -c 0-3) ==\n");
    std::printf("(A single tell()-based round trip between two actors — NOT ask/ReplyCell, cf.\n"
                " ask_bench.cpp, which measures that separate mechanism)\n\n");

    constexpr std::uint64_t kWarmup = 5'000;
    constexpr std::uint64_t kLaps = 100'000;

    run_ping_pong(1, "A) same-shard ping-pong (both actors, shard 0)", kWarmup, kLaps);
    std::printf("\n");
    run_ping_pong(2, "B) cross-shard ping-pong (actors split across 2 shards/workers)", kWarmup, kLaps);
    return 0;
}
