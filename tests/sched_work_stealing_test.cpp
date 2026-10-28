// Tests 002-Scheduler §Work stealing — load posted to shards that are NOT an idle worker's home is
// drained by that idle worker (stealing = winning the non-home shard's drain-owner CAS). All load is
// placed on shards whose HOME worker is worker 0; workers 1 and 2 have entirely empty home shards, so
// ANY message they dispatch was necessarily stolen. Asserts both non-home lanes steal work and every
// message is dispatched.
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

constexpr std::uint32_t kWorkers = 3;
constexpr std::uint32_t kShards = 9;                 // home(shard)=shard%3 ⇒ {0,3,6} are worker 0's
constexpr std::uint32_t kTargetShards[] = {0, 3, 6};  // all homed to worker 0
constexpr std::uint32_t kActorsPerShard = 4;
// 20,000/actor (240,000 total) drained in ~130ms on Windows CI with the per-message busy-loop below at
// 64 iterations — short enough that worker 2's wake-from-park latency (a real OS thread wake, not a
// spin) can exceed the ENTIRE drain window, so it legitimately never gets a turn: observed FAIL
// (stolen-by-w2=0) on msvc-release TWICE, even with the below-warmup phase proving all 3 threads alive
// at start (that only rules out the startup race; it says nothing about worker 2 getting re-scheduled
// once it parks again after warmup). 10x the per-actor count for a much wider drain window.
constexpr std::uint64_t kPerActor = 200'000;
constexpr std::uint64_t kStall = 40'000'000'000ULL;

struct Job {
    std::uint32_t dummy;
};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Job>;
    std::atomic<std::uint64_t>* by_worker = nullptr;  // [kWorkers] dispatch counts
    std::atomic<std::uint64_t>* total = nullptr;

    void handle(const Job&) noexcept {
        const std::uint32_t w = current_worker_id();
        if (w < kWorkers) by_worker[w].fetch_add(1, std::memory_order_relaxed);
        // Real per-message work so the drain window (see kPerActor above) stays wide relative to a
        // worker's OS wake-from-park latency, not just relative to raw message count.
        volatile std::uint32_t sink = 0;
        for (int i = 0; i < 512; ++i) sink += static_cast<std::uint32_t>(i);
        (void)sink;
        total->fetch_add(1, std::memory_order_release);
    }
};

}  // namespace

int main() {
    bool ok = true;
    std::atomic<std::uint64_t> by_worker[kWorkers];
    for (auto& c : by_worker) c.store(0);
    std::atomic<std::uint64_t> total{0};

    Engine<> eng(EngineConfig{kWorkers, kShards, /*budget*/ 64, 64});

    // Find ActorIds that land on the target shards (all homed to worker 0).
    std::vector<Sink> actors;
    std::vector<ActorId> ids;
    actors.reserve(3 * kActorsPerShard);
    for (std::uint32_t ti = 0; ti < 3; ++ti) {
        const std::uint32_t want = kTargetShards[ti];
        std::uint32_t found = 0;
        for (std::uint64_t k = 0; found < kActorsPerShard; ++k) {
            const ActorId id{TypeKey{42}, k};
            if (eng.shard_of(id) == want) {
                ids.push_back(id);
                ++found;
            }
        }
    }

    const std::uint32_t n_actors = static_cast<std::uint32_t>(ids.size());
    actors.resize(n_actors);
    std::vector<std::unique_ptr<Activation>> acts;
    std::vector<Schedulable*> sched(n_actors);
    for (std::uint32_t a = 0; a < n_actors; ++a) {
        actors[a].by_worker = by_worker;
        actors[a].total = &total;
        acts.push_back(std::make_unique<Activation>(&actors[a], Sink::dispatch_table()));
        sched[a] = eng.register_activation(ids[a], *acts.back());
    }

    // One warm-up actor on a shard homed to worker 1 and one homed to worker 2 (worker 0's liveness
    // is proven below via sched[0], already on shard 0 — home worker 0).
    ActorId warmup_ids[2];
    for (std::uint32_t wi = 0; wi < 2; ++wi) {
        const std::uint32_t want = wi + 1;  // shard 1 (home worker 1), shard 2 (home worker 2)
        for (std::uint64_t k = 0;; ++k) {
            const ActorId id{TypeKey{42}, k};
            if (eng.shard_of(id) == want) {
                warmup_ids[wi] = id;
                break;
            }
        }
    }
    std::vector<Sink> warmup_actors(2);
    std::array<std::unique_ptr<Activation>, 2> warmup_acts;
    std::array<Schedulable*, 2> warmup_sched{};
    for (std::uint32_t wi = 0; wi < 2; ++wi) {
        warmup_actors[wi].by_worker = by_worker;
        warmup_actors[wi].total = &total;
        warmup_acts[wi] = std::make_unique<Activation>(&warmup_actors[wi], Sink::dispatch_table());
        warmup_sched[wi] = eng.register_activation(warmup_ids[wi], *warmup_acts[wi]);
    }

    eng.start();

    // Confirm all 3 worker OS threads are alive and have actually run before racing the real,
    // heavily-biased backlog below. Windows CI (2-core windows-latest runners, higher thread-start
    // latency than Linux pthreads) can let workers 0/1 fully drain a small backlog before worker 2's
    // thread ever gets its first scheduling quantum — worker 2 then legitimately steals nothing, not
    // because stealing is broken but because it was never scheduled in time to compete for it. One
    // real message per worker, waited on to completion, rules that out deterministically.
    std::array<Job, 3> warmup_jobs{};
    std::array<Descriptor, 3> warmup_descs{};
    for (auto& d : warmup_descs) stamp<Sink, Job>(d);
    warmup_descs[0].payload = &warmup_jobs[0];
    warmup_descs[1].payload = &warmup_jobs[1];
    warmup_descs[2].payload = &warmup_jobs[2];
    eng.post(sched[0], &warmup_descs[0]);
    eng.post(warmup_sched[0], &warmup_descs[1]);
    eng.post(warmup_sched[1], &warmup_descs[2]);
    {
        std::uint64_t spins = 0;
        while (total.load(std::memory_order_acquire) < 3) {
            if (++spins > kStall) {
                std::fprintf(stderr, "STALL: warm-up did not complete (total=%" PRIu64 "/3)\n",
                              total.load());
                eng.stop();
                return 1;
            }
        }
    }
    for (auto& c : by_worker) c.store(0);
    total.store(0);

    const std::uint64_t grand_total = static_cast<std::uint64_t>(n_actors) * kPerActor;
    std::vector<Job> msgs(grand_total);
    std::vector<Descriptor> descs(grand_total);
    for (std::uint64_t i = 0; i < grand_total; ++i) {
        descs[i].payload = &msgs[i];
        stamp<Sink, Job>(descs[i]);
    }

    // Load the full backlog now that all 3 lanes are confirmed running — they race to steal it.
    std::uint64_t di = 0;
    for (std::uint64_t r = 0; r < kPerActor; ++r)
        for (std::uint32_t a = 0; a < n_actors; ++a) eng.post(sched[a], &descs[di++]);

    std::uint64_t spins = 0;
    while (total.load(std::memory_order_acquire) < grand_total) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: total=%" PRIu64 "/%" PRIu64 "\n", total.load(), grand_total);
            eng.stop();
            return 1;
        }
    }
    eng.stop();

    const std::uint64_t w0 = by_worker[0].load(), w1 = by_worker[1].load(), w2 = by_worker[2].load();
    if (w1 == 0) { std::fprintf(stderr, "  CHECK FAILED: worker 1 stole no work\n"); ok = false; }
    if (w2 == 0) { std::fprintf(stderr, "  CHECK FAILED: worker 2 stole no work\n"); ok = false; }
    if (w0 + w1 + w2 != grand_total) { std::fprintf(stderr, "  CHECK FAILED: dispatch count mismatch\n"); ok = false; }
    if (total.load() != grand_total) { std::fprintf(stderr, "  CHECK FAILED: not all messages dispatched\n"); ok = false; }

    std::printf("sched_work_stealing_test: %s  (total=%" PRIu64 " home-w0=%" PRIu64
                " stolen-by-w1=%" PRIu64 " stolen-by-w2=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", grand_total, w0, w1, w2);
    return ok ? 0 : 1;
}
