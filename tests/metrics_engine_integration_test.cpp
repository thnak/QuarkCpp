// Tests 009-Observability wired into the REAL Engine (engine.hpp/activation.hpp), not metrics.hpp
// in a vacuum (that's metrics_test.cpp/metrics_noalloc_test.cpp — neither touches an Engine).
//
// The headline regression this guards: mailbox_enqueued/activations/wakeups fire on the PRODUCER
// side (Activation::post / Engine::schedule_and_wake), where MULTIPLE concurrent producer threads
// can target the SAME shard (many actors hash to one shard). A naive `MetricCounter::inc()`
// (relaxed load+store, single-writer only) would silently lose increments under that concurrency —
// no crash, no TSan report, just a wrong number. This test forces exactly that scenario (several
// actors on a single-shard engine, driven by concurrent producer threads) and asserts the EXACT
// expected mailbox_enqueued count, which only holds if every producer's `inc_atomic()` landed.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// ---- Concurrent-producer actor: trivial sync handler, single shard by construction ------------
struct Tick {};
struct GetSeen {};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Tick, Ask<GetSeen, std::uint64_t>>;
    void handle(const Tick&) noexcept { ++seen; }
    void handle(const Ask<GetSeen, std::uint64_t>& m) noexcept { m.respond(seen); }
    std::uint64_t seen = 0;
};

// ---- Fault actor: drives a restart + a dead-letter through the real supervised fault path ------
struct Boom {};
struct GetCount {};

struct Faulty : Actor<Faulty, Sequential> {
    using protocol = Protocol<Boom, Ask<GetCount, int>>;
    void handle(const Boom&) { ++count; throw std::runtime_error("boom"); }
    void handle(const Ask<GetCount, int>& m) noexcept { m.respond(count); }
    int count = 0;
};

}  // namespace

int main() {
    bool ok = true;

    // =============================================================================================
    // Part 1 — concurrent producers hammering a SINGLE shard: mailbox_enqueued must be exact.
    // =============================================================================================
    {
        constexpr std::uint32_t kActors = 4;
        constexpr std::uint32_t kProducers = 4;   // machine-safety cap (<=4 concurrent threads)
        constexpr std::uint64_t kPerProducer = 50'000;

        detail::MessagePool pool(1u << 16);
        // 1 shard, several workers: every actor below hashes to the SAME (only) shard, and several
        // workers contend the drain-owner CAS while several producer threads post concurrently —
        // exactly the multi-writer scenario mailbox_enqueued/activations/wakeups must survive.
        Engine<> eng(EngineConfig{/*workers*/ 3, /*shards*/ 1, /*budget*/ 64, 64});

        std::vector<std::unique_ptr<Sink>> actors;
        std::vector<std::unique_ptr<Activation>> acts;
        for (std::uint32_t a = 0; a < kActors; ++a) {
            actors.push_back(std::make_unique<Sink>());
            acts.push_back(std::make_unique<Activation>(actors.back().get(), Sink::dispatch_table(),
                                                         pool.sink()));
            eng.register_activation(actor_id_of<Sink>(a), *acts.back());
        }
        check(eng.shard_count() == 1, "single-shard engine (forces same-shard contention)", ok);

        LocalRouter router(eng.post_courier(), pool);
        std::vector<ActorRef<Sink>> refs;
        for (std::uint32_t a = 0; a < kActors; ++a) refs.push_back(router.get<Sink>(a));
        eng.start();
        // Let the 3 workers reach park() while the run-queue is still empty, so the FIRST Tick any
        // producer posts below is guaranteed to find an idle worker in idle_mask_ (engine.hpp
        // wake_one()) and snap.wakeups below is deterministically >= 1. Without this, thread-startup
        // scheduling can race the producers ahead of the workers' first scan_and_run(), in which case
        // workers never park at all ("every worker busy" — engine.hpp's own documented common case)
        // and wakeups legitimately stays 0. Observed as a genuine flake on the msvc-release Windows CI
        // leg (run 29901602862): not an engine bug, just an unsynchronized race in this test.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (std::uint32_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&refs, p, kActors] {
                for (std::uint64_t i = 0; i < kPerProducer; ++i)
                    refs[(p + i) % kActors].tell(Tick{});
            });
        }
        for (auto& t : producers) t.join();

        eng.stop();  // clean drain: waits until every posted Tick has actually been handled

        std::uint64_t total_seen = 0;
        for (auto& a : actors) total_seen += a->seen;
        const std::uint64_t expected = static_cast<std::uint64_t>(kProducers) * kPerProducer;
        check(total_seen == expected, "every Tick was actually handled (sanity on the harness itself)", ok);

        const MetricsSnapshot snap = eng.metrics_snapshot();
        check(snap.mailbox_enqueued == expected,
              "mailbox_enqueued EXACT under concurrent same-shard producers (the inc_atomic() regression check)",
              ok);
        check(snap.messages_processed == expected,
              "messages_processed EXACT (drain-owner-exclusive counter, single or multi-worker)", ok);
        check(snap.activations >= 1 && snap.activations <= expected,
              "activations counted at least once and never exceeds enqueue count (Idle->Scheduled edges)",
              ok);
        check(snap.wakeups >= 1, "at least one worker was actually woken", ok);
        check(snap.dead_letters == 0 && snap.restarts == 0, "no faults in this part", ok);

        // Prometheus exposition round-trips through the same aggregated snapshot.
        const std::string prom = eng.metrics_prometheus();
        check(prom.find("quark_mailbox_enqueued_total ") != std::string::npos,
              "metrics_prometheus() emits mailbox_enqueued", ok);
    }

    // =============================================================================================
    // Part 2 — a real supervised fault: restarts + dead_letters move exactly once per fault.
    // =============================================================================================
    {
        detail::MessagePool pool(1024);
        Engine<> eng(EngineConfig{2, 2, 64, 64});

        // spawn<A>() wires the 008 reconstruct factory + default supervision (Restart, unbounded).
        result<ActorId> spawned = eng.spawn<Faulty>(1, pool.sink());
        check(spawned.has_value(), "spawn<Faulty> succeeds", ok);

        LocalRouter router(eng.post_courier(), pool);
        ActorRef<Faulty> ref = router.get<Faulty>(1);
        eng.start();

        ref.tell(Boom{});  // ++count (->1), then throws -> handler fault -> dead-letter + Restart
        result<int> r = block_on(ref.ask<int>(GetCount{}));  // FIFO-behind the fault: post-restart state
        check(r.has_value() && r.value() == 0, "Restart reconstructed fresh state (count == 0)", ok);

        eng.stop();

        const MetricsSnapshot snap = eng.metrics_snapshot();
        check(snap.restarts == 1, "exactly one restart recorded for the one fault", ok);
        check(snap.dead_letters == 1, "exactly one dead-letter recorded for the one faulted message", ok);
    }

    std::printf("metrics_engine_integration_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
