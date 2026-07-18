// Tests ADR-007 C2 (concurrent-ask reply ordering + lifetime) — many callers ask ONE actor
// concurrently; each caller's block_on must resolve to ITS OWN correct reply (no cross-caller
// misroute, no lost wakeup, no UAF). This is the ReplyCell win-arbitration under contention: run
// under TSan/ASan. Capped at 4 caller threads (machine-safety: never saturate cores).
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"

using namespace quark;

namespace {

struct Compute {
    long token;  // caller identity — the reply must echo f(token), never another caller's
};

struct Server : Actor<Server, Sequential> {
    using protocol = Protocol<Ask<Compute, long>>;
    // A pure per-request transform: the reply is uniquely determined by the request token, so any
    // cross-caller misroute is detectable by the caller comparing against its own expected value.
    void handle(const Ask<Compute, long>& m) noexcept { m.respond(m.query.token * 2654435761L + 7L); }
};

constexpr long expected_for(long token) noexcept { return token * 2654435761L + 7L; }

}  // namespace

int main() {
    bool ok = true;

    detail::MessagePool pool(8192);
    Server actor;
    auto act = std::make_unique<Activation>(&actor, Server::dispatch_table(), pool.sink());

    // 2 worker lanes, 1 shard (Sequential actor ⇒ single-executor regardless). ≤4 threads total.
    Engine<> eng(EngineConfig{2, 1, 64, 64});
    eng.register_activation(actor_id_of<Server>(99), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Server> ref = router.get<Server>(99);
    eng.start();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;
    std::atomic<int> misroutes{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const long token = static_cast<long>(t) * 1'000'000L + i;
                result<long> r = block_on(ref.ask<long>(Compute{token}));
                if (!r.has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                } else if (r.value() != expected_for(token)) {
                    misroutes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : callers) th.join();
    eng.stop();

    if (misroutes.load() != 0) {
        std::fprintf(stderr, "  CHECK FAILED: %d cross-caller misroutes\n", misroutes.load());
        ok = false;
    }
    if (failures.load() != 0) {
        std::fprintf(stderr, "  CHECK FAILED: %d asks failed to resolve\n", failures.load());
        ok = false;
    }

    std::printf("ask_concurrent_test: %s  (threads=%d each=%d misroutes=%d failures=%d)\n",
                ok ? "OK" : "FAIL", kThreads, kPerThread, misroutes.load(), failures.load());
    return ok ? 0 : 1;
}
