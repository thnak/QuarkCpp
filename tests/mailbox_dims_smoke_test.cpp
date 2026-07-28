// Dimension 1 ("Correctness — Pass/Fail") of the mailbox benchmark/test suite commissioned to
// evaluate the CURRENT shipped mailbox (include/quark/core/mailbox.hpp) and, unchanged, whatever a
// future design-debate round produces against it. This is the aggregate top-level smoke test: it
// asserts the mailbox's basic contract end to end, in one binary, so a single glance answers
// "is the mailbox's core contract intact?" without reading the whole per-dimension suite.
//
// It deliberately re-derives (not duplicates in depth) the checks the deeper per-dimension tests
// cover: mailbox_fifo_per_sender_test / mailbox_engine_fifo_exactly_once_test (dims 2-3),
// mailbox_pool_aba_stress_test (dim 4), mailbox_cancel_test / mailbox_noalloc_test (existing,
// dims 4/21). Sections:
//   A) Vyukov queue-primitive contract: Empty -> Busy -> Message -> stub re-arm -> Empty, exactly
//      as documented in mailbox.hpp's try_dequeue banner.
//   B) Single-thread FIFO + exactly-once over a modest run (sanity floor for the harder stress
//      tests below).
//   C) A small real multi-producer-to-one-actor run over the ACTUAL Engine (spawn/tell), so the
//      "basic contract" claim is proven through the production path, not just the raw queue type.
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"

using namespace quark;

namespace {
void check(bool cond, const char* what, bool& ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// --- Section A: the documented Vyukov state-machine contract, single thread, deterministic. -----
bool section_a_queue_contract() {
    bool ok = true;
    DescriptorPool pool(8);
    Mailbox mb;

    // Fresh mailbox: genuinely empty (no producer mid-publish).
    DrainResult r0 = mb.try_dequeue();
    check(r0.status == DrainStatus::Empty, "A: fresh mailbox drains Empty", ok);
    check(!mb.probe_has_work(), "A: fresh mailbox probe_has_work() is false", ok);

    // One enqueue -> exactly one Message, then back to Empty (stub re-armed).
    Descriptor* d1 = pool.acquire();
    d1->message_id = MessageId{111};
    mb.enqueue(d1);
    check(mb.probe_has_work(), "A: probe_has_work() true immediately after enqueue", ok);
    DrainResult r1 = mb.try_dequeue();
    check(r1.status == DrainStatus::Message && r1.desc == d1,
          "A: try_dequeue returns the just-enqueued descriptor", ok);
    pool.release(d1);
    DrainResult r2 = mb.try_dequeue();
    check(r2.status == DrainStatus::Empty, "A: mailbox Empty again after draining its one message", ok);

    // Multi-node: 3 enqueues, 3 Messages in order, then Empty.
    Descriptor* d2 = pool.acquire();
    Descriptor* d3 = pool.acquire();
    Descriptor* d4 = pool.acquire();
    d2->message_id = MessageId{2};
    d3->message_id = MessageId{3};
    d4->message_id = MessageId{4};
    mb.enqueue(d2);
    mb.enqueue(d3);
    mb.enqueue(d4);
    DrainResult ra = mb.try_dequeue(), rb = mb.try_dequeue(), rc = mb.try_dequeue();
    check(ra.status == DrainStatus::Message && ra.desc->message_id.value == 2,
          "A: multi-node drain order [0] == enqueue order", ok);
    check(rb.status == DrainStatus::Message && rb.desc->message_id.value == 3,
          "A: multi-node drain order [1] == enqueue order", ok);
    check(rc.status == DrainStatus::Message && rc.desc->message_id.value == 4,
          "A: multi-node drain order [2] == enqueue order", ok);
    pool.release(d2);
    pool.release(d3);
    pool.release(d4);
    DrainResult rEnd = mb.try_dequeue();
    check(rEnd.status == DrainStatus::Empty, "A: Empty after draining a 3-node chain", ok);
    return ok;
}

// --- Section B: single-thread FIFO + exactly-once over a modest run. -----------------------------
bool section_b_single_thread_fifo() {
    bool ok = true;
    constexpr int N = 5000;
    DescriptorPool pool(64);
    Mailbox mb;
    std::vector<int> order;
    order.reserve(N);

    // Interleave enqueue/dequeue in bursts so both the multi-node path and the stub re-arm
    // boundary fire repeatedly within one run.
    int produced = 0;
    while (static_cast<int>(order.size()) < N) {
        const int burst = (produced % 7) + 1;
        for (int b = 0; b < burst && produced < N; ++b, ++produced) {
            Descriptor* d = pool.acquire();
            if (d == nullptr) break;  // pool sized for the burst pattern; treated as backpressure
            d->message_id = MessageId{static_cast<std::uint64_t>(produced)};
            mb.enqueue(d);
        }
        for (;;) {
            DrainResult r = mb.try_dequeue();
            if (r.status != DrainStatus::Message) break;
            order.push_back(static_cast<int>(r.desc->message_id.value));
            pool.release(r.desc);
        }
    }
    for (;;) {  // final drain
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) break;
        order.push_back(static_cast<int>(r.desc->message_id.value));
        pool.release(r.desc);
    }

    check(static_cast<int>(order.size()) == N, "B: exactly N messages observed (exactly-once)", ok);
    bool fifo = true;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] != static_cast<int>(i)) fifo = false;
    check(fifo, "B: single-producer FIFO preserved end to end", ok);
    return ok;
}

// --- Section C: the ACTUAL production path — Engine::spawn + ActorRef::tell, small multi-producer.
struct Seq { int producer; int seq; };

struct Collector : Actor<Collector, Sequential> {
    using protocol = Protocol<Seq>;
    std::atomic<int> count{0};
    std::vector<int> last_seq = std::vector<int>(4, -1);  // per-producer last seen seq
    std::atomic<int> fifo_violations{0};

    void handle(const Seq& s) noexcept {
        if (s.seq != last_seq[static_cast<std::size_t>(s.producer)] + 1)
            fifo_violations.fetch_add(1, std::memory_order_relaxed);
        last_seq[static_cast<std::size_t>(s.producer)] = s.seq;
        count.fetch_add(1, std::memory_order_release);
    }
};

bool section_c_engine_production_path() {
    bool ok = true;
    constexpr unsigned kProducers = 4;
    constexpr int kPerProducer = 2000;
    constexpr int kTotal = static_cast<int>(kProducers) * kPerProducer;

    detail::MessagePool pool(static_cast<std::size_t>(kTotal) + 256);
    Collector actor;
    auto act = std::make_unique<Activation>(&actor, Collector::dispatch_table(), pool.sink());

    Engine<> eng(EngineConfig{2, 1, 64, 64});  // 2 workers, 1 shard (<=4 threads total incl. producers)
    eng.register_activation(actor_id_of<Collector>(1), *act);
    LocalRouter router(eng.post_courier(), pool);
    ActorRef<Collector> ref = router.get<Collector>(1);
    eng.start();

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (unsigned p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int s = 0; s < kPerProducer; ++s)
                ref.tell(Seq{static_cast<int>(p), s});
        });
    }
    for (auto& th : producers) th.join();

    constexpr std::uint64_t kStall = 5'000'000'000ULL;
    std::uint64_t spins = 0;
    while (actor.count.load(std::memory_order_acquire) < kTotal) {
        if (++spins > kStall) {
            std::fprintf(stderr, "  STALL: only %d of %d delivered\n", actor.count.load(), kTotal);
            eng.stop();
            return false;
        }
    }
    eng.stop();

    check(actor.count.load() == kTotal, "C: Engine delivers exactly-once through spawn/tell", ok);
    check(actor.fifo_violations.load() == 0, "C: per-(sender,receiver) FIFO holds through spawn/tell", ok);
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    std::printf("mailbox_dims_smoke_test (dimension 1: aggregate correctness)\n");

    bool a = section_a_queue_contract();
    std::printf("  section A (queue-primitive contract): %s\n", a ? "OK" : "FAIL");
    ok &= a;

    bool b = section_b_single_thread_fifo();
    std::printf("  section B (single-thread FIFO/exactly-once): %s\n", b ? "OK" : "FAIL");
    ok &= b;

    bool c = section_c_engine_production_path();
    std::printf("  section C (Engine spawn/tell production path): %s\n", c ? "OK" : "FAIL");
    ok &= c;

    std::printf("mailbox_dims_smoke_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
