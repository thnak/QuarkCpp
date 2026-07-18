// Tests 006-Messaging-and-Addressing §tell + §Delivery semantics — `tell` delivers every message,
// preserves per-(sender,receiver) FIFO, and is exactly-once locally. One sender posts a strictly
// increasing sequence through an ActorRef; the (Sequential, single-executor) actor records arrival
// order on its drain lane. Asserts: all N arrive, in send order, each exactly once.
#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Seq {
    int n;
};

struct Logger : Actor<Logger, Sequential> {
    using protocol = Protocol<Seq>;
    std::vector<int> got;             // written only on the single drain lane (single-executor)
    std::atomic<int> count{0};

    void handle(const Seq& s) noexcept {
        got.push_back(s.n);
        count.fetch_add(1, std::memory_order_release);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    constexpr int N = 1000;

    detail::MessagePool pool(N + 64);
    Logger actor;
    auto act = std::make_unique<Activation>(&actor, Logger::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{/*workers*/ 1, /*shards*/ 1, /*budget*/ 64, 64});
    eng.register_activation(actor_id_of<Logger>(42), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Logger> ref = router.get<Logger>(42);

    eng.start();
    for (int i = 0; i < N; ++i) ref.tell(Seq{i});  // one sender → per-(sender,receiver) FIFO

    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (actor.count.load(std::memory_order_acquire) < N) {
        if (++spins > kStall) {
            std::fprintf(stderr, "STALL: only %d of %d delivered\n", actor.count.load(), N);
            eng.stop();
            return 1;
        }
    }
    eng.stop();

    check(static_cast<int>(actor.got.size()) == N, "exactly N messages delivered (exactly-once)", ok);
    bool ordered = true;
    for (std::size_t i = 0; i < actor.got.size(); ++i)
        if (actor.got[i] != static_cast<int>(i)) ordered = false;
    check(ordered, "per-(sender,receiver) FIFO preserved (arrival order == send order)", ok);

    std::printf("tell_fifo_test: %s  (delivered=%zu)\n", ok ? "OK" : "FAIL", actor.got.size());
    return ok ? 0 : 1;
}
