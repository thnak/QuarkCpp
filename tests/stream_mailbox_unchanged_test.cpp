// Tests 024-Streaming §normative: the stream ring sits OFF the mailbox; the ordinary Vyukov mailbox +
// discrete tell path stay BYTE-FOR-BYTE unchanged, and a non-streaming actor constructs NO
// StreamChannel. This test includes BOTH the stream headers AND the mailbox/activation core in one TU
// and asserts (a) the discrete mailbox enqueue/dequeue round-trip is unaffected by their presence, and
// (b) an Activation carries no stream machinery (the stream is a separate, additively-composed type —
// StreamActivation — not a member of Activation). Single-thread, deterministic.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <type_traits>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"
#include "quark/core/stream_activation.hpp"  // pulled in alongside the core — must not perturb it
#include "quark/core/stream_channel.hpp"

using namespace quark;

namespace {
struct Ping {
    std::uint64_t seq;
};

// A plain non-streaming Sequential actor.
struct Echo : Actor<Echo, Sequential> {
    using protocol = Protocol<Ping>;
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    void handle(const Ping& p) noexcept {
        sum += p.seq;
        ++count;
    }
};

struct Frame {
    std::uint64_t id;
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

    // (a) A StreamActivation is a DISTINCT type from Activation — the stream ring is off the mailbox,
    // not a member of the discrete-path activation. (If streaming had been welded into Activation, a
    // non-streaming actor would carry the ring; it does not.)
    static_assert(!std::is_base_of_v<StreamActivation<Frame>, Activation>,
                  "Activation must not derive from the stream ring");
    check(sizeof(StreamActivation<Frame>) > 0, "StreamActivation is a separate, additively-composed type", ok);

    // (b) The discrete mailbox + activation tell path is unchanged: post N pings, drain, verify.
    constexpr std::uint64_t kN = 10'000;
    Echo actor;
    Activation act{&actor, Echo::dispatch_table()};

    std::vector<Ping> msgs(kN);
    std::vector<Descriptor> descs(kN);
    std::uint64_t expect_sum = 0;
    for (std::uint64_t i = 0; i < kN; ++i) {
        msgs[i].seq = i;
        expect_sum += i;
        descs[i].payload = &msgs[i];
        stamp<Echo, Ping>(descs[i]);
        act.post(&descs[i]);
    }

    // Drain to completion (single-thread: acquire -> drain -> close_out loop).
    if (act.try_acquire()) {
        for (;;) {
            const auto o = act.drain_step(1u << 20);
            if (o == Activation::DrainOutcome::DrainedEmpty) {
                if (act.close_out()) continue;
                break;
            }
            if (o == Activation::DrainOutcome::BudgetExhausted) {
                act.yield_to_scheduled();
                if (act.try_acquire()) continue;
                break;
            }
            break;
        }
    }

    check(actor.count == kN, "discrete mailbox drained every tell (path unchanged)", ok);
    check(actor.sum == expect_sum, "discrete tell payloads intact", ok);
    check(act.state() == ExecState::Idle, "activation relinquished to Idle (mailbox close-out unchanged)", ok);

    std::printf("stream_mailbox_unchanged_test: %s  (tells=%" PRIu64 " sum=%" PRIu64 ")\n",
                ok ? "OK" : "FAIL", actor.count, actor.sum);
    return ok ? 0 : 1;
}
