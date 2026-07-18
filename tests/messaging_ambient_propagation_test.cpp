// Tests the ambient trace/deadline propagation glue (006 + 009 + 018): a `tell`/`ask` issued FROM a
// running handler auto-inherits that message's trace correlation id (009) and deadline (018), without
// the discrete-message API taking a ctx argument. Mechanism: the Activation publishes the running
// handler's MessageContext on a thread-local (detail::AmbientContextScope) around every dispatch; the
// router (LocalRouter::post_message) stamps the outbound descriptor from it. A `tell` outside any
// handler (bootstrap) sees a null ambient ⇒ a fresh trace / no deadline.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/message_context.hpp"

using namespace quark;

namespace {

struct Ping {};
struct Pong {};

// Terminal actor: records the trace/deadline its inbound message carried.
struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Pong>;
    std::atomic<std::uint64_t> got_trace{0};
    std::atomic<std::int64_t> got_deadline{0};
    std::atomic<int> count{0};
    void handle(const Pong&, const MessageContext& ctx) noexcept {
        got_trace.store(ctx.trace_id, std::memory_order_relaxed);
        got_deadline.store(ctx.deadline_ns, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_release);
    }
};

// Relay: on Ping it tells Sink a Pong — the outbound tell must inherit the Relay handler's ambient
// context (which itself was inherited from the message that triggered the Relay).
struct Relay : Actor<Relay, Sequential> {
    using protocol = Protocol<Ping>;
    ActorRef<Sink>* sink = nullptr;
    void handle(const Ping&) { sink->tell(Pong{}); }
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

    detail::MessagePool pool(4096);
    Relay relay;
    Sink sink;
    auto relay_act = std::make_unique<Activation>(&relay, Relay::dispatch_table(), pool.sink());
    auto sink_act = std::make_unique<Activation>(&sink, Sink::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{1, 1, 64, 64});
    eng.register_activation(actor_id_of<Relay>(1), *relay_act);
    eng.register_activation(actor_id_of<Sink>(2), *sink_act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Relay> rref = router.get<Relay>(1);
    ActorRef<Sink> sref = router.get<Sink>(2);
    relay.sink = &sref;
    eng.start();

    auto wait_for = [&](int n) {
        for (std::uint64_t s = 0; sink.count.load(std::memory_order_acquire) < n; ++s)
            if (s > 4'000'000'000ULL) break;
    };

    // Round 1 — a root tell issued UNDER an ambient context (as the 009 ingress edge would establish
    // a trace + the 018 deadline). It propagates root → Relay → Sink through two real handler hops.
    {
        MessageContext root{};
        root.trace_id = 0xBEEFCAFE;
        root.deadline_ns = 123456;
        detail::AmbientContextScope amb(root);
        rref.tell(Ping{});
    }
    wait_for(1);
    check(sink.got_trace.load() == 0xBEEFCAFE, "trace id propagated root→Relay→Sink", ok);
    check(sink.got_deadline.load() == 123456, "deadline inherited root→Relay→Sink", ok);

    // Round 2 — CONTROL: a root tell with NO ambient context (bootstrap). The chain must carry a fresh
    // trace / no deadline — proving the propagation is the ambient, not some accidental global.
    rref.tell(Ping{});
    wait_for(2);
    check(sink.got_trace.load() == 0, "no ambient ⇒ fresh trace (0)", ok);
    check(sink.got_deadline.load() == 0, "no ambient ⇒ no deadline (0)", ok);

    eng.stop();
    std::printf("messaging_ambient_propagation_test: %s  (trace=0x%llX deadline=%lld)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(sink.got_trace.load()),
                static_cast<long long>(sink.got_deadline.load()));
    return ok ? 0 : 1;
}
