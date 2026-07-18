// Tests 004-Resources §Rules resolution correctness:
//   * a declared Cached<> resolves to the RIGHT instance (identity, not just non-null);
//   * an UNDECLARED resource is a validation error via quark::result / errc::validation;
//   * a PerMessage<> product is produced by its cached factory (fresh product per acquire);
//   * a factory returning std::unexpected fails the acquire uniformly (the 007 unexpected channel);
//   * Ambient<> values are read directly from the MessageContext (never resolved).
#include <cstdio>
#include <stop_token>
#include <string>

#include "quark/core/message_context.hpp"
#include "quark/core/resource.hpp"

using namespace quark;

namespace {

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// --- Stand-in resources -----------------------------------------------------------------------
struct Logger {
    int tag = 0;
};
struct PgPool {
    int handle = 0;
};

// A per-message product with an observable RAII lifetime.
struct PgSession {
    int id = 0;
    bool alive = true;
    ~PgSession() { alive = false; }
};

// An activation-scoped factory state (e.g. the pool). Hands out fresh sessions with rising ids.
struct SessionSource {
    int next = 1;
    bool fail = false;
    result<PgSession> operator()() {
        if (fail) return fail_session();
        return PgSession{next++, true};
    }
    static result<PgSession> fail_session() {
        return quark::fail(errc::unavailable, "pool exhausted");
    }
};

}  // namespace

int main() {
    // ---- Declared Cached<> resolves to the right INSTANCE -------------------------------------
    Logger log_a{7};
    Logger log_b{9};  // a second Logger instance NOT provided — proves identity matters
    PgPool pool{42};

    ResourceScope scope;
    scope.provide(log_a, ResourceLifetime::Activation);
    scope.provide(pool, ResourceLifetime::Node);

    Cached<Logger> log;
    Cached<PgPool> cached_pool;
    {
        result<void> w = log.wire(scope);
        check(w.has_value(), "Cached<Logger>::wire should succeed (declared)");
        result<void> w2 = cached_pool.wire(scope);
        check(w2.has_value(), "Cached<PgPool>::wire should succeed (declared)");
    }
    check(log.resolved(), "Cached<Logger> resolved");
    check(&log.get() == &log_a, "Cached<Logger> resolves to the exact provided instance");
    check(&log.get() != &log_b, "Cached<Logger> does not resolve to a look-alike instance");
    check(log.get().tag == 7, "Cached<Logger> reads the right value");
    check(cached_pool->handle == 42, "Cached<PgPool> operator-> reads the right value");

    // ---- Undeclared resource is a validation error -------------------------------------------
    struct Metrics {};
    Cached<Metrics> metrics;
    {
        result<void> w = metrics.wire(scope);
        check(!w.has_value(), "undeclared Cached<Metrics> must fail to wire");
        check(w.error().code == errc::validation,
              "undeclared resource fails with errc::validation");
    }
    {
        // Direct scope resolution of an undeclared type also validates.
        result<Metrics*> r = scope.resolve<Metrics>();
        check(!r.has_value() && r.error().code == errc::validation,
              "scope.resolve of undeclared type => errc::validation");
    }

    // ---- PerMessage<> product from a cached factory ------------------------------------------
    SessionSource src;
    Factory<PgSession> session_factory = make_factory<PgSession>(src);
    ResourceScope pm_scope;
    pm_scope.provide(session_factory, ResourceLifetime::Activation);

    PerMessage<PgSession> session;
    {
        result<void> w = session.wire(pm_scope);
        check(w.has_value(), "PerMessage<PgSession>::wire resolves the cached factory");
    }
    check(!session.acquired(), "PerMessage product not acquired before the guarded region");
    {
        result<void> a = session.acquire();
        check(a.has_value(), "PerMessage::acquire produces a product");
        check(session.acquired(), "PerMessage product present after acquire");
        check(session.get().id == 1, "first product has id 1");
        session.release();
        check(!session.acquired(), "product released at message end");
    }
    {
        result<void> a = session.acquire();
        check(a.has_value() && session.get().id == 2, "second acquire yields a FRESH product (id 2)");
        session.release();
    }

    // ---- Factory unexpected-channel failure fails the acquire uniformly ----------------------
    src.fail = true;
    {
        result<void> a = session.acquire();
        check(!a.has_value(), "a failing factory (unexpected) fails PerMessage::acquire");
        check(a.error().code == errc::unavailable, "factory error propagates unchanged");
        check(!session.acquired(), "no product is left behind on factory failure");
    }
    src.fail = false;

    // ---- Ambient<> read directly from the MessageContext (never resolved) --------------------
    std::stop_source ss;
    MessageContext ctx;
    ctx.stop = ss.get_token();
    ctx.deadline_ns = 123456;
    ctx.trace_id = 0xABCDEF;

    check(!Ambient<std::stop_token>::get(ctx).stop_requested(), "ambient stop_token not requested");
    ss.request_stop();
    check(Ambient<std::stop_token>::get(ctx).stop_requested(),
          "ambient stop_token observes cancellation");
    check(Ambient<Deadline>::get(ctx).ns() == 123456, "ambient Deadline read from context");
    check(Ambient<TraceId>::get(ctx).value == 0xABCDEF, "ambient TraceId read from context");

    std::printf("resource_resolution_test: %s (failures=%d)\n", g_failures ? "FAIL" : "OK",
                g_failures);
    return g_failures ? 1 : 0;
}
