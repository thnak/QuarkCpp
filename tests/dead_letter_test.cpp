// Tests 009-Observability §Dead-letter inspection — the 009 DeadLetterRegistry plugs into the
// activation.hpp DeadLetterSink seam, receives an undeliverable message with the RIGHT reason, and
// stays BOUNDED under a flood (shed-don't-buffer, ADR-009 S4): a fixed-capacity ring retains the
// most-recent records and counts the rest as dropped — memory never grows.
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/dead_letter.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

struct Poison {
    int n;
};

// A handler that always faults — every message routes to dead-letter (007 -> 009 sink). NOT
// noexcept: the throw propagates to the Activation handler-boundary guard, which dead-letters it.
struct Poisoner : Actor<Poisoner, Sequential> {
    using protocol = Protocol<Poison>;
    void handle(const Poison&) { throw std::runtime_error("boom"); }
};
}  // namespace

int main() {
    // ---- The sink receives an undeliverable message with the right reason --------------------
    {
        DeadLetterRegistry dlq(256);
        Poisoner actor;
        // Resume: keep the actor alive after each fault (the message is dead-lettered regardless of
        // the directive — dead_letter_and_reclaim runs before the supervision switch).
        Activation act{&actor, Poisoner::dispatch_table(), {}, /*max_concurrency=*/1,
                       SupervisionPolicy{SupervisionDirective::Resume}};
        act.set_dead_letter_sink(dlq.as_sink());

        Poison p{42};
        Descriptor d;
        d.payload = &p;
        d.trace_id = 0xABCDEF;
        stamp<Poisoner, Poison>(d);

        act.post(&d);
        check(act.try_acquire(), "acquire Scheduled->Running");
        (void)act.drain_step(8);
        (void)act.close_out();

        check(dlq.total() == 1, "one dead-letter recorded");
        check(act.dead_letters() == 1, "activation dead_letters observer agrees");
        std::vector<DeadLetterRecord> recs;
        dlq.snapshot(recs);
        check(recs.size() == 1, "one retained record");
        if (!recs.empty()) {
            check(recs[0].err.code == errc::supervised_stop, "reason code is supervised_stop");
            check(recs[0].err.detail == std::string_view{"handler_fault"}, "reason detail is handler_fault");
            check(recs[0].trace_id == 0xABCDEF, "record captured the trace_id");
            check(recs[0].msg_slot == slot_of<Poisoner, Poison>(), "record captured the msg slot");
        }
    }

    // ---- Bounded under flood: fixed-capacity ring, shed-don't-buffer --------------------------
    {
        constexpr std::size_t kCap = 8;
        constexpr int kFlood = 5000;
        DeadLetterRegistry dlq(kCap);
        Poisoner actor;
        Activation act{&actor, Poisoner::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Resume}};
        act.set_dead_letter_sink(dlq.as_sink());

        std::vector<Descriptor> ds(static_cast<std::size_t>(kFlood));
        Poison p{7};
        for (int i = 0; i < kFlood; ++i) {
            ds[static_cast<std::size_t>(i)].payload = &p;
            ds[static_cast<std::size_t>(i)].trace_id = static_cast<std::uint64_t>(i);
            stamp<Poisoner, Poison>(ds[static_cast<std::size_t>(i)]);
            act.post(&ds[static_cast<std::size_t>(i)]);
        }
        check(act.try_acquire(), "acquire for flood drain");
        // Drain everything (Resume keeps the actor alive across all faults).
        for (;;) {
            const auto out = act.drain_step(1024);
            if (out == Activation::DrainOutcome::DrainedEmpty) break;
            if (out == Activation::DrainOutcome::BudgetExhausted) continue;
            break;  // Busy/Suspended not expected on this synchronous single-thread drive
        }
        (void)act.close_out();

        check(dlq.total() == static_cast<std::uint64_t>(kFlood), "all flood messages counted");
        check(dlq.size() == kCap, "ring retains exactly capacity (bounded)");
        check(dlq.capacity() == kCap, "capacity is fixed at construction");
        check(dlq.dropped() == static_cast<std::uint64_t>(kFlood) - kCap, "the rest were shed");

        std::vector<DeadLetterRecord> recs;
        dlq.snapshot(recs);
        check(recs.size() == kCap, "snapshot bounded to capacity");
        // Retain-recent: the last kCap trace_ids are [kFlood-kCap, kFlood).
        bool recent = true;
        for (std::size_t i = 0; i < recs.size(); ++i)
            if (recs[i].trace_id != static_cast<std::uint64_t>(kFlood - kCap) + i) recent = false;
        check(recent, "ring retained the most-recent records (oldest->newest)");
    }

    std::printf("dead_letter_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
