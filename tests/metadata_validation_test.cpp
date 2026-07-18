// Tests 008-Metadata-and-Startup §Validation (fail-fast) — the startup Validation set:
//   1. STRICT: an actor whose handler needs an UNDECLARED resource (004) fails registration with
//      errc::validation and publishes NOTHING.
//   2. RELAXED: the same error is downgraded to a WARNING and the (quarantined) type still registers.
//   3. STRICT: a type_key COLLISION (two distinct types hashing equal) is a Strict startup failure.
//   4. The Engine::spawn integration surfaces the SAME fail-fast (undeclared resource → unexpected).
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/resource.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {};

// A heavy handle the actor needs resolved at activation (004 Cached<>).
struct DbConn {
    int fd = 7;
};

struct NeedsDb : Actor<NeedsDb, Sequential> {
    using protocol = Protocol<Ping>;
    Cached<DbConn> db_;
    // The 004 explicit cold wire pass. Undeclared DbConn ⇒ errc::validation.
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, db_); }
    void handle(const Ping&) {}
};

struct Plain : Actor<Plain, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// The resource-wire validation closure the registry runs (an undeclared resource fails it).
auto undeclared_check() {
    return [] {
        ResourceScope empty;  // provides NOTHING
        NeedsDb tmp;
        return tmp.wire(empty);
    };
}

}  // namespace

int main() {
    bool ok = true;

    // 1) STRICT — undeclared resource fails registration, nothing published.
    {
        TypeRegistry reg(Validation::Strict);
        result<std::uint16_t> r = reg.register_type<NeedsDb>(undeclared_check());
        check(!r.has_value() && r.error().code == errc::validation,
              "Strict: undeclared resource → errc::validation", ok);
        check(reg.size() == 0, "Strict: nothing published on failure", ok);
        check(reg.report().has_error(), "Strict: report records the error", ok);
    }

    // 2) RELAXED — downgraded to a warning; the quarantined type still registers.
    {
        TypeRegistry reg(Validation::Permissive);
        result<std::uint16_t> r = reg.register_type<NeedsDb>(undeclared_check());
        check(r.has_value(), "Relaxed: registration continues (quarantined)", ok);
        check(reg.size() == 1, "Relaxed: the degraded type is published", ok);
        check(!reg.report().has_error() && reg.report().warnings() == 1,
              "Relaxed: error downgraded to a warning", ok);
    }

    // 3) STRICT — type_key collision (two distinct types forced to the same key) fails fast.
    {
        TypeRegistry reg(Validation::Strict);
        ActorMetadata a = compile_actor_metadata<Plain>();
        ActorMetadata b = compile_actor_metadata<NeedsDb>();
        b.key = a.key;  // force a collision (distinct types hashing equal)
        result<std::uint16_t> ra = reg.register_metadata(a, "Plain", [] { return result<void>{}; });
        result<std::uint16_t> rb = reg.register_metadata(b, "NeedsDb", [] { return result<void>{}; });
        check(ra.has_value(), "first (Plain) registers", ok);
        check(!rb.has_value() && rb.error().code == errc::validation,
              "Strict: type_key collision → errc::validation", ok);
        check(reg.size() == 1, "collision publishes nothing new", ok);
    }

    // 4) Engine::spawn integration — same fail-fast, and success when the resource IS provided.
    {
        detail::MessagePool pool(64);
        Engine<> eng(EngineConfig{1, 1, 16, 64});

        ResourceScope empty;
        result<ActorId> bad = eng.spawn<NeedsDb>(1, pool.sink(), &empty);
        check(!bad.has_value() && bad.error().code == errc::validation,
              "spawn: undeclared resource fails fast (errc::validation)", ok);

        DbConn conn;
        ResourceScope scope;
        scope.provide(conn, ResourceLifetime::Activation);
        result<ActorId> good = eng.spawn<NeedsDb>(2, pool.sink(), &scope);
        check(good.has_value(), "spawn: succeeds once the resource is provided", ok);
        // A resource-free actor spawns with no scope at all.
        result<ActorId> plain = eng.spawn<Plain>(3, pool.sink());
        check(plain.has_value(), "spawn: resource-free actor needs no scope", ok);
    }

    std::printf("metadata_validation_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
