// Tests 022-Resource-Governance-and-Overload-Control §"bound every exhaustible resource" — the
// bounded mailbox + Overflow policy, the archetype (006/022). At the bound: Overflow::Block
// backpressures (frame NOT admitted, NOT dead-lettered — the caller backs off); Overflow::DropNewest
// / Fail shed the NEW frame at admission (producer-side, caller owns it); Overflow::DropOldest admits
// the newest and the lane sheds the OLDEST down to the bound, dead-lettering it (errc::overloaded).
// Every shed/block is counted (009). Admitted messages keep per-actor FIFO (single-executor).
#include <cassert>
#include <cstdio>
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

struct Item {
    std::uint64_t id;
};

struct Sink : Actor<Sink, Sequential> {
    using protocol = Protocol<Item>;
    std::vector<std::uint64_t> processed;
    void handle(const Item& m) { processed.push_back(m.id); }
};

// Build a governed activation with a fixed bound + overflow policy (no HotCell — static fallback).
Activation::GovernanceConfig gov_cfg(std::uint32_t bound, Overflow ov) {
    Activation::GovernanceConfig gc;
    gc.static_bound = bound;
    gc.static_overflow = ov;
    return gc;
}

void drain_all(Activation& act) {
    check(act.try_acquire(), "acquire Scheduled->Running");
    for (;;) {
        const auto out = act.drain_step(1024);
        if (out == Activation::DrainOutcome::DrainedEmpty) break;
        if (out == Activation::DrainOutcome::BudgetExhausted) continue;
        break;
    }
    (void)act.close_out();
}
}  // namespace

int main() {
    constexpr std::uint32_t kBound = 4;

    // ---- Overflow::Block — at the bound the producer is told to back off ----------------------
    {
        Sink actor;
        Activation act{&actor, Sink::dispatch_table()};
        act.enable_governance(gov_cfg(kBound, Overflow::Block));

        std::vector<Item> items(8);
        std::vector<Descriptor> ds(8);
        auto post = [&](std::uint32_t i) {
            items[i].id = i;
            ds[i].payload = &items[i];
            ds[i].message_id = MessageId{i};
            stamp<Sink, Item>(ds[i]);
            return act.post_governed(&ds[i]);
        };
        for (std::uint32_t i = 0; i < kBound; ++i)
            check(post(i).result == Activation::AdmitResult::Admitted, "block: bound frames admitted");
        check(act.mailbox_depth() == kBound, "block: depth == bound");

        const auto over = post(kBound);
        check(over.result == Activation::AdmitResult::Blocked, "block: at bound ⇒ Blocked");
        check(act.governance_blocks() == 1, "block: one backpressure counted");
        check(act.governance_sheds() == 0, "block: nothing dead-lettered on Block");

        drain_all(act);
        check(actor.processed.size() == kBound, "block: exactly bound messages processed");
        check(act.mailbox_depth() == 0, "block: depth back to 0 after drain");

        // Now that space freed, the previously-blocked frame is admitted.
        check(post(kBound).result == Activation::AdmitResult::Admitted, "block: re-admit after drain");
        drain_all(act);
        check(actor.processed.size() == kBound + 1, "block: re-admitted frame processed");
    }

    // ---- Overflow::DropNewest — the NEW frame is shed at admission (caller dead-letters it) ----
    {
        Sink actor;
        Activation act{&actor, Sink::dispatch_table()};
        act.enable_governance(gov_cfg(kBound, Overflow::DropNewest));
        DeadLetterRegistry dlq(64);

        std::vector<Item> items(8);
        std::vector<Descriptor> ds(8);
        auto post = [&](std::uint32_t i) {
            items[i].id = i;
            ds[i].payload = &items[i];
            ds[i].message_id = MessageId{i};
            ds[i].trace_id = i;
            stamp<Sink, Item>(ds[i]);
            return act.post_governed(&ds[i]);
        };
        for (std::uint32_t i = 0; i < kBound; ++i)
            check(post(i).result == Activation::AdmitResult::Admitted, "dropnewest: bound admitted");

        const auto over = post(kBound);  // id 4 — the newest, must be shed
        check(over.result == Activation::AdmitResult::Shed, "dropnewest: newest shed at admission");
        check(act.governance_sheds() == 1, "dropnewest: one shed counted");
        // The caller owns the shed descriptor — dead-letter it (producer-side, its own send-pool path).
        dlq.record(&ds[kBound], error{errc::overloaded, "drop_newest"});
        check(dlq.total() == 1, "dropnewest: shed frame dead-lettered");

        drain_all(act);
        check(actor.processed.size() == kBound, "dropnewest: only the bound survivors ran");
        // Survivors are ids 0..3 (the newest, id 4, was the one dropped).
        bool fifo = true;
        for (std::uint32_t i = 0; i < kBound; ++i)
            if (actor.processed[i] != i) fifo = false;
        check(fifo, "dropnewest: admitted survivors keep FIFO order");
    }

    // ---- Overflow::DropOldest — admit the newest; lane sheds the oldest, dead-letters it --------
    {
        constexpr std::uint32_t kExtra = 2;
        constexpr std::uint32_t kTotal = kBound + kExtra;  // 6
        Sink actor;
        Activation act{&actor, Sink::dispatch_table()};
        act.enable_governance(gov_cfg(kBound, Overflow::DropOldest));
        DeadLetterRegistry dlq(64);
        act.set_dead_letter_sink(dlq.as_sink());

        std::vector<Item> items(kTotal);
        std::vector<Descriptor> ds(kTotal);
        for (std::uint32_t i = 0; i < kTotal; ++i) {
            items[i].id = i;
            ds[i].payload = &items[i];
            ds[i].message_id = MessageId{i};
            ds[i].trace_id = i;
            stamp<Sink, Item>(ds[i]);
            const auto pa = act.post_governed(&ds[i]);
            check(pa.result == Activation::AdmitResult::Admitted,
                  "dropoldest: producer admits the newest (over bound)");
        }
        check(act.mailbox_depth() == kTotal, "dropoldest: all admitted before drain");

        drain_all(act);
        // The lane shed the 2 oldest (ids 0,1) and ran the newest 4 (ids 2,3,4,5).
        check(act.governance_sheds() == kExtra, "dropoldest: excess oldest shed on the lane");
        check(dlq.total() == kExtra, "dropoldest: shed-oldest frames dead-lettered");
        check(actor.processed.size() == kBound, "dropoldest: bound newest survivors ran");
        bool newest = true;
        for (std::uint32_t i = 0; i < kBound; ++i)
            if (actor.processed[i] != kExtra + i) newest = false;  // ids 2,3,4,5 in FIFO
        check(newest, "dropoldest: survivors are the newest, in FIFO order");

        std::vector<DeadLetterRecord> recs;
        dlq.snapshot(recs);
        check(recs.size() == kExtra, "dropoldest: two dead-letter records");
        bool reason_ok = !recs.empty();
        for (const auto& rec : recs)
            if (rec.err.code != errc::overloaded) reason_ok = false;
        check(reason_ok, "dropoldest: dead-letter reason is errc::overloaded");
        // The shed frames are the oldest (trace ids 0,1).
        check(recs.size() == 2 && recs[0].trace_id == 0 && recs[1].trace_id == 1,
              "dropoldest: the OLDEST frames were the ones shed");
    }

    std::printf("governance_bounded_mailbox_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
