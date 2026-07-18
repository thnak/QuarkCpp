// Tests 004-Resources §Rules per-message product lifetime + the guarded region (ADR-009), run
// under ASan/UBSan (build-asan) for lifetime correctness:
//   * a PerMessage<> product is constructed at acquire and DESTROYED at message end (RAII); live
//     count returns to zero — no leak, no double free, no use-after-free;
//   * ProductGuard acquires all products before the (simulated) handler and releases them after;
//   * a factory that returns std::unexpected AND one that THROWS both fail the acquire uniformly,
//     the handler never runs, and any already-acquired product is released as the guard unwinds
//     (proven by the balanced live count).
#include <cstdio>
#include <stdexcept>

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

// A product that tracks how many instances are alive at any moment.
int g_live = 0;
struct Session {
    int id;
    explicit Session(int i) : id(i) { ++g_live; }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& o) noexcept : id(o.id) { ++g_live; }  // moved-from still owns a slot
    ~Session() { --g_live; }
};

// Factory states for the three channels: success, unexpected, throw.
struct OkSource {
    int next = 1;
    result<Session> operator()() { return Session{next++}; }
};
struct UnexpectedSource {
    result<Session> operator()() { return quark::fail(errc::unavailable, "no session"); }
};
struct ThrowingSource {
    result<Session> operator()() {
        throw std::runtime_error("factory blew up");
    }
};

}  // namespace

int main() {
    // ---- Product RAII: constructed at acquire, destroyed at release --------------------------
    {
        OkSource src;
        Factory<Session> fac = make_factory<Session>(src);
        ResourceScope scope;
        scope.provide(fac, ResourceLifetime::Activation);

        PerMessage<Session> pm;
        check(pm.wire(scope).has_value(), "wire ok");

        check(g_live == 0, "no product before acquire");
        check(pm.acquire().has_value(), "acquire ok");
        check(g_live == 1, "exactly one product alive after acquire");
        check(pm.get().id == 1, "product id");
        pm.release();
        check(g_live == 0, "product destroyed at message end (RAII)");
    }
    check(g_live == 0, "no leak after PerMessage scope");

    // ---- ProductGuard acquires-before-handler and releases-after (success) -------------------
    {
        OkSource src_a;
        OkSource src_b;
        Factory<Session> fa = make_factory<Session>(src_a);
        Factory<Session> fb = make_factory<Session>(src_b);
        // Two distinct product types would need two Factory<T> entries; here two PerMessage<Session>
        // share one product type but each is wired to its own factory via separate scopes.
        ResourceScope sa;
        sa.provide(fa, ResourceLifetime::Activation);
        ResourceScope sb;
        sb.provide(fb, ResourceLifetime::Activation);

        PerMessage<Session> pa;
        PerMessage<Session> pb;
        check(pa.wire(sa).has_value() && pb.wire(sb).has_value(), "wire both");

        bool handler_ran = false;
        {
            ProductGuard guard(pa, pb);
            result<void> acq = guard.acquire();
            check(acq.has_value(), "guard acquires all products before handler");
            check(g_live == 2, "both products alive inside the guarded region");
            if (acq) {
                handler_ran = true;  // simulate the handler body running with valid products
                check(pa.get().id == 1 && pb.get().id == 1, "each product from its own factory");
            }
        }  // ~ProductGuard releases both products here
        check(handler_ran, "handler ran with valid products");
        check(g_live == 0, "all products released after the guarded region");
    }
    check(g_live == 0, "no leak after ProductGuard success scope");

    // ---- Unexpected channel: acquire fails, handler skipped, no product left behind ----------
    {
        UnexpectedSource src;
        Factory<Session> fac = make_factory<Session>(src);
        ResourceScope scope;
        scope.provide(fac, ResourceLifetime::Activation);

        PerMessage<Session> pm;
        check(pm.wire(scope).has_value(), "wire ok");

        bool handler_ran = false;
        {
            ProductGuard guard(pm);
            result<void> acq = guard.acquire();
            check(!acq.has_value(), "unexpected factory fails the guarded acquire");
            check(acq.error().code == errc::unavailable, "error propagates to the 007 boundary");
            if (acq) handler_ran = true;  // must NOT run
        }
        check(!handler_ran, "handler never runs on factory failure (unexpected)");
        check(g_live == 0, "no product leaked on the unexpected channel");
    }

    // ---- Throwing channel: acquire propagates, already-acquired products still released -------
    {
        OkSource ok_src;      // first factory succeeds ...
        ThrowingSource bad;   // ... second throws => guard must release the first while unwinding
        Factory<Session> fok = make_factory<Session>(ok_src);
        Factory<Session> fbad = make_factory<Session>(bad);
        ResourceScope sok;
        sok.provide(fok, ResourceLifetime::Activation);
        ResourceScope sbad;
        sbad.provide(fbad, ResourceLifetime::Activation);

        PerMessage<Session> good;
        PerMessage<Session> throws;
        check(good.wire(sok).has_value() && throws.wire(sbad).has_value(), "wire both");

        bool handler_ran = false;
        bool caught = false;
        try {
            ProductGuard guard(good, throws);
            result<void> acq = guard.acquire();  // good acquires; throws throws => exception
            if (acq) handler_ran = true;
        } catch (const std::exception&) {
            caught = true;
        }
        check(caught, "a throwing factory propagates to the 007 boundary (caught here)");
        check(!handler_ran, "handler never runs on a throwing factory");
        check(g_live == 0, "the already-acquired product was released as the guard unwound");
    }
    check(g_live == 0, "no leak after throwing-channel scope");

    std::printf("resource_lifetime_test: %s (failures=%d)\n", g_failures ? "FAIL" : "OK",
                g_failures);
    return g_failures ? 1 : 0;
}
