// Tests 001-Actor-Execution-Model §Cancellation + §Fairness — a queued message cancelled via the
// gen-gated CAS becomes a tombstone: the drain SKIPS it (no handler runs), reclaims it EXACTLY ONCE
// (ADR-004), and CHARGES the skip against the drain budget (002 §Fairness) so a mass-cancel cannot
// monopolize the lane. Run under ASan (no double-free/UAF) — the claim-vs-drain edge is single-lane.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {
struct Ping {
    int n;
};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Ping>;
    int handled = 0;
    void handle(const Ping& p) noexcept { handled += p.n; }
};

// A reclaim sink that counts reclamations per descriptor index (to prove exactly-once), then does
// the real gen-bump release(). This is the 002/003 pool seam stubbed for the test.
struct Recorder {
    Descriptor* base = nullptr;
    std::vector<int>* counts = nullptr;
};
void record_reclaim(Descriptor* d, void* ctx) noexcept {
    auto* r = static_cast<Recorder*>(ctx);
    (*r->counts)[static_cast<std::size_t>(d - r->base)]++;
    d->release();
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;

    // ---- Part A: mixed drain — cancelled entries skip the handler, reclaim once, don't run. ----
    {
        constexpr unsigned kN = 6;
        Sink actor;
        std::vector<Ping> msgs(kN);
        std::vector<Descriptor> descs(kN);
        std::vector<int> reclaims(kN, 0);
        Recorder rec{descs.data(), &reclaims};
        Activation act{&actor, Sink::dispatch_table(), ReclaimSink{&record_reclaim, &rec}};

        std::vector<MessageHandle> handles(kN);
        int expected_sum = 0;
        for (unsigned i = 0; i < kN; ++i) {
            msgs[i].n = static_cast<int>(i) + 1;  // 1..6
            descs[i].payload = &msgs[i];
            stamp<Sink, Ping>(descs[i]);
            handles[i] = handle_of(&descs[i]);
            act.post(&descs[i]);
        }
        // Cancel the even-indexed messages (indices 0,2,4 → values 1,3,5).
        int cancelled = 0;
        for (unsigned i = 0; i < kN; i += 2) {
            if (handles[i].cancel()) ++cancelled;
        }
        for (unsigned i = 1; i < kN; i += 2) expected_sum += msgs[i].n;  // 2+4+6

        check(cancelled == 3, "3 queued messages cancelled to tombstones", ok);
        check(act.try_acquire(), "acquire", ok);
        const auto out = act.drain_step(1000);
        check(out == Activation::DrainOutcome::DrainedEmpty, "drained all (survivors + tombstones)", ok);
        check(actor.handled == expected_sum, "only non-cancelled handlers ran", ok);

        int total_reclaims = 0, over = 0;
        for (unsigned i = 0; i < kN; ++i) {
            total_reclaims += reclaims[i];
            if (reclaims[i] != 1) ++over;
        }
        check(over == 0, "every descriptor reclaimed EXACTLY once (survivors + tombstones)", ok);
        check(total_reclaims == static_cast<int>(kN), "total reclamations == message count", ok);
        std::printf("  part A: handled=%d expected=%d cancelled=%d reclaims_total=%d\n", actor.handled,
                    expected_sum, cancelled, total_reclaims);
    }

    // ---- Part B: budget accounting — a run of tombstones charges the budget and yields the lane. --
    {
        constexpr unsigned kN = 8;
        constexpr std::uint32_t kBudget = 4;
        Sink actor;
        std::vector<Ping> msgs(kN);
        std::vector<Descriptor> descs(kN);
        std::vector<int> reclaims(kN, 0);
        Recorder rec{descs.data(), &reclaims};
        Activation act{&actor, Sink::dispatch_table(), ReclaimSink{&record_reclaim, &rec}};

        std::vector<MessageHandle> handles(kN);
        for (unsigned i = 0; i < kN; ++i) {
            msgs[i].n = 1;
            descs[i].payload = &msgs[i];
            stamp<Sink, Ping>(descs[i]);
            handles[i] = handle_of(&descs[i]);
            act.post(&descs[i]);
        }
        for (unsigned i = 0; i < kN; ++i) (void)handles[i].cancel();  // all cancelled

        check(act.try_acquire(), "acquire", ok);
        const auto out = act.drain_step(kBudget);
        check(out == Activation::DrainOutcome::BudgetExhausted,
              "a run of tombstones exhausts the budget (does not monopolize the lane)", ok);

        int reclaimed_first_step = 0;
        for (unsigned i = 0; i < kN; ++i) reclaimed_first_step += reclaims[i];
        check(reclaimed_first_step == static_cast<int>(kBudget),
              "exactly `budget` tombstone-skips were charged in the first drain step", ok);
        check(actor.handled == 0, "no handler ran (all cancelled)", ok);

        // Fairness: yield then finish the rest on the next turn.
        act.yield_to_scheduled();
        check(act.state() == ExecState::Scheduled, "Running->Scheduled on budget-exhaust", ok);
        check(act.try_acquire(), "re-acquire for the next turn", ok);
        const auto out2 = act.drain_step(1000);  // ample budget ⇒ reaches genuine emptiness
        check(out2 == Activation::DrainOutcome::DrainedEmpty, "remaining tombstones drained", ok);
        int reclaimed_total = 0, over = 0;
        for (unsigned i = 0; i < kN; ++i) {
            reclaimed_total += reclaims[i];
            if (reclaims[i] != 1) ++over;
        }
        check(reclaimed_total == static_cast<int>(kN) && over == 0,
              "all tombstones reclaimed exactly once across turns", ok);
        std::printf("  part B: first_step_reclaims=%d (budget=%u) total=%d\n", reclaimed_first_step,
                    kBudget, reclaimed_total);
    }

    std::printf("exec_cancel_budget_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
