// Tests 004/007 §"PerMessage<T> factory failure" (ADR-009 residual risk #6) — the
// `OnResourceFailure<FailMessage|Degrade>` knob, driven directly against one Activation lane (no
// Engine, mirrors supervision_decision_test.cpp's style):
//   * FailMessage (default) — `ProductGuard::acquire_or_throw()` on a failed factory throws
//     `ResourceFailure`, classified as `FailureSource::Resource` (not a generic handler throw): the
//     dead-letter sink observes the FACTORY's OWN error, not a generic "handler_fault" label, and the
//     supervision decision still applies uniformly.
//   * Degrade — the handler calls `ProductGuard::acquire()` directly and proceeds without the
//     resource on failure; no 007 fault is raised at all.
//   * A plain (non-`ResourceFailure`) throw is still classified as `FailureSource::HandlerThrow` —
//     proves the new catch clause doesn't swallow ordinary faults.
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "quark/core/actor.hpp"
#include "quark/core/resource.hpp"
#include "quark/core/supervision.hpp"
#include "reentrancy_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct Query {};
struct Boom {};

struct Session {
    int id = 0;
};

// An activation-scoped factory: hands out fresh sessions, or fails on demand.
struct SessionSource {
    int next = 1;
    bool fail = false;
    result<Session> operator()() {
        if (fail) return quark::fail(errc::unavailable, "pool exhausted");
        return Session{next++};
    }
};

// FailMessage (default): a failed acquire throws ResourceFailure via acquire_or_throw().
struct FailMsgActor : Actor<FailMsgActor, Sequential> {
    using protocol = Protocol<Query, Boom>;
    PerMessage<Session> session_;
    int handled = 0;
    void handle(const Query&) {
        ProductGuard guard(session_);
        guard.acquire_or_throw();
        ++handled;
    }
    void handle(const Boom&) { throw std::runtime_error("ordinary fault, not a resource failure"); }
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, session_); }
};

// Degrade: a failed acquire is handled by the actor itself — no 007 fault.
struct DegradeActor : Actor<DegradeActor, Sequential, OnResourceFailure<Degrade>> {
    using protocol = Protocol<Query>;
    PerMessage<Session> session_;
    int handled = 0;
    int degraded = 0;
    void handle(const Query&) {
        ProductGuard guard(session_);
        if (result<void> r = guard.acquire(); !r) {
            ++degraded;
            return;
        }
        ++handled;
    }
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, session_); }
};

struct Feeder {
    std::vector<Descriptor> descs;
    std::vector<Query> queries;
    std::vector<Boom> booms;
    explicit Feeder(std::size_t cap) : descs(cap), queries(cap), booms(cap) {}
    template <class A>
    void query(Activation& act, std::size_t i) {
        descs[i].payload = &queries[i];
        stamp<A, Query>(descs[i]);
        act.post(&descs[i]);
    }
    template <class A>
    void boom(Activation& act, std::size_t i) {
        descs[i].payload = &booms[i];
        stamp<A, Boom>(descs[i]);
        act.post(&descs[i]);
    }
};

void capture_dead_letter(Descriptor*, error e, void* ctx) noexcept {
    *static_cast<error*>(ctx) = e;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Compile-time knob resolution -----------------------------------------------------
    check(!resource_failure_degrades<FailMsgActor>(), "FailMsgActor (no OnResourceFailure) => FailMessage", ok);
    check(resource_failure_degrades<DegradeActor>(), "DegradeActor (OnResourceFailure<Degrade>) => Degrade", ok);

    // ---- FailMessage: a failed acquire fails the message as FailureSource::Resource -------
    {
        SessionSource src;
        Factory<Session> factory = make_factory<Session>(src);
        ResourceScope scope;
        scope.provide(factory, ResourceLifetime::Activation);

        FailMsgActor a;
        check(a.wire(scope).has_value(), "FailMessage: wire() resolves the factory", ok);

        Activation act{&a, FailMsgActor::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Resume}};
        error last_dl{};
        act.set_dead_letter_sink({&capture_dead_letter, &last_dl});

        Feeder f(3);
        src.fail = false;
        f.query<FailMsgActor>(act, 0);  // succeeds
        drive(act);
        src.fail = true;
        f.query<FailMsgActor>(act, 1);  // factory fails -> ResourceFailure -> FailureSource::Resource
        drive(act);

        check(a.handled == 1, "FailMessage: only the succeeding query incremented handled", ok);
        check(act.faults() == 1, "FailMessage: exactly one 007 fault contained", ok);
        check(act.dead_letters() == 1, "FailMessage: the failed acquire dead-lettered the message", ok);
        check(last_dl.code == errc::unavailable,
              "FailMessage: dead-letter observes the FACTORY's own error code (not a generic label)", ok);
        check(last_dl.detail == "pool exhausted",
              "FailMessage: dead-letter observes the factory's own error detail", ok);
        check(!act.is_stopped(), "FailMessage: Resume kept the actor live", ok);

        // A plain throw is still classified as an ordinary handler fault (not a resource failure) —
        // proves the new ResourceFailure-specific catch clause doesn't swallow other exceptions.
        src.fail = false;
        f.boom<FailMsgActor>(act, 2);
        drive(act);
        check(act.faults() == 2, "FailMessage: the ordinary throw is ALSO contained (2 faults total)", ok);
        check(last_dl.code == errc::supervised_stop,
              "FailMessage: an ordinary throw records the generic handler_fault error, not the "
              "factory's error code from the PRIOR resource fault", ok);
    }

    // ---- Degrade: a failed acquire proceeds without the resource, no 007 fault ------------
    {
        SessionSource src;
        src.fail = true;  // every acquire fails, to specifically exercise the degrade path
        Factory<Session> factory = make_factory<Session>(src);
        ResourceScope scope;
        scope.provide(factory, ResourceLifetime::Activation);

        DegradeActor a;
        check(a.wire(scope).has_value(), "Degrade: wire() resolves the factory", ok);

        Activation act{&a, DegradeActor::dispatch_table(), {}, 1,
                       SupervisionPolicy{SupervisionDirective::Resume}};
        Feeder f(1);
        f.query<DegradeActor>(act, 0);
        drive(act);

        check(a.degraded == 1 && a.handled == 0,
              "Degrade: the handler observed the failed acquire itself and proceeded degraded", ok);
        check(act.faults() == 0, "Degrade: no 007 fault — acquire() failure never reaches the guard", ok);
        check(act.dead_letters() == 0, "Degrade: the message was NOT dead-lettered", ok);
    }

    std::printf("supervision_resource_failure_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
