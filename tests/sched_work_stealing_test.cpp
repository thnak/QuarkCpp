// Tests 002-Scheduler §Work stealing — load posted to shards that are NOT an idle worker's home is
// drained by that idle worker (stealing = winning the non-home shard's drain-owner CAS). All load is
// placed on shards whose HOME worker is worker 0; workers 1 and 2 have entirely empty home shards, so
// ANY message they dispatch was necessarily stolen. Asserts both non-home lanes steal work and every
// message is dispatched.
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
constexpr std::uint64_t kPerActor = 20'000;

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
        // A little work so an initial cross-shard drain assignment holds long enough to be observed.
        volatile std::uint32_t sink = 0;
        for (int i = 0; i < 64; ++i) sink += static_cast<std::uint32_t>(i);
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

    const std::uint64_t grand_total = static_cast<std::uint64_t>(n_actors) * kPerActor;
    std::vector<Job> msgs(grand_total);
    std::vector<Descriptor> descs(grand_total);
    for (std::uint64_t i = 0; i < grand_total; ++i) {
        descs[i].payload = &msgs[i];
        stamp<Sink, Job>(descs[i]);
    }

    // Pre-load all shards, then start the lanes so all three begin scanning against a full backlog.
    std::uint64_t di = 0;
    for (std::uint64_t r = 0; r < kPerActor; ++r)
        for (std::uint32_t a = 0; a < n_actors; ++a) eng.post(sched[a], &descs[di++]);

    eng.start();
    constexpr std::uint64_t kStall = 40'000'000'000ULL;
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
