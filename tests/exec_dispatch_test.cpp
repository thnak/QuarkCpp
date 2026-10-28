// Tests 001-Actor-Execution-Model §Hybrid handler execution + ADR-007 §Dispatch — the sync-vs-async
// handler is selected at COMPILE TIME by return type (void vs quark::task<>), and the correct dense
// slot invokes the correct handler for each message type. No RTTI/virtual on the dispatch path.
#include <cassert>
#include <cstdio>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"

using namespace quark;

namespace {
struct Ship {
    int n;
};
struct Query {
    int n;
};
struct Report {
    int n;
};

struct Order : Actor<Order, Sequential> {
    using protocol = Protocol<Ship, Query, Report>;

    int ships = 0;
    int queries = 0;
    int reports = 0;

    void handle(const Ship& s) noexcept { ships += s.n; }                    // sync
    task<> handle(const Query& q) { queries += q.n; co_return; }             // async (no suspend)
    void handle(const Report& r, const MessageContext&) noexcept { reports += r.n; }  // sync + ctx

    void assert_true(bool c, const char* what, bool& ok) const {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    }
};

// ---- Compile-time selection (the whole point): resolved with zero runtime branch. -----------
static_assert(sync_handler<Order, Ship>, "Ship handler is sync (void)");
static_assert(async_handler<Order, Query>, "Query handler is async (task<>)");
static_assert(sync_handler<Order, Report>, "Report handler is sync (void, ctx overload)");
static_assert(!async_handler<Order, Ship>);
static_assert(!sync_handler<Order, Query>);
static_assert(Handles<Order, Ship> && Handles<Order, Query> && Handles<Order, Report>);
// Dense slots are the protocol positions.
static_assert(slot_of<Order, Ship>() == 0);
static_assert(slot_of<Order, Query>() == 1);
static_assert(slot_of<Order, Report>() == 2);
}  // namespace

int main() {
    bool ok = true;
    Order actor;
    Activation act{&actor, Order::dispatch_table()};

    Ship ship{3};
    Query query{5};
    Report report{7};

    Descriptor d_ship;
    d_ship.payload = &ship;
    stamp<Order, Ship>(d_ship);
    Descriptor d_query;
    d_query.payload = &query;
    stamp<Order, Query>(d_query);
    Descriptor d_report;
    d_report.payload = &report;
    stamp<Order, Report>(d_report);

    // Post all three (Idle→Scheduled on the first), acquire, drain to empty.
    const bool wake = act.post(&d_ship);
    act.post(&d_query);
    act.post(&d_report);
    actor.assert_true(wake, "first post performed the Idle->Scheduled wake edge", ok);
    actor.assert_true(act.state() == ExecState::Scheduled, "scheduled after post", ok);

    actor.assert_true(act.try_acquire(), "worker acquires Scheduled->Running", ok);
    const auto out = act.drain_step(64);
    actor.assert_true(out == Activation::DrainOutcome::DrainedEmpty, "drained empty", ok);
    actor.assert_true(!act.close_out(), "close-out releases to Idle (no more work)", ok);
    actor.assert_true(act.state() == ExecState::Idle, "idle after close-out", ok);

    actor.assert_true(actor.ships == 3, "sync Ship handler ran", ok);
    actor.assert_true(actor.queries == 5, "async Query handler ran (inline, no suspend)", ok);
    actor.assert_true(actor.reports == 7, "sync+ctx Report handler ran", ok);

    std::printf("exec_dispatch_test: %s  (ships=%d queries=%d reports=%d)\n", ok ? "OK" : "FAIL",
                actor.ships, actor.queries, actor.reports);
    return ok ? 0 : 1;
}
