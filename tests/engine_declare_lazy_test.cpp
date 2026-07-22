// Tests 008-Metadata-and-Startup §Type registry + ADR-028 Phase 3 — `Engine::declare_lazy<A>()`
// wires the (previously unwired-in-Engine) TypeRegistry/compile_actor_metadata<A>() into a real
// Engine WITHOUT constructing an actor instance or a Schedulable. This is the seam ADR-028 Phase 4's
// ActivationBroker will consume by TypeKey to build a genuinely lazy first-touch activation; this
// phase only proves metadata compiles/publishes correctly and that NOTHING is instantiated yet.
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/resource.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

struct Ping {};

struct Worker : Actor<Worker, Sequential, Priority<0>, DrainBudget<32>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};

struct Other : Actor<Other, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};

struct DbConn {
    int fd = 7;
};

// Mirrors metadata_validation_test.cpp's NeedsDb — an actor whose 004 wire pass fails without a
// provided resource, so declare_lazy's optional WireCheck closure has the same fail-fast seam
// spawn<A>() already has.
struct NeedsDb : Actor<NeedsDb, Sequential> {
    using protocol = Protocol<Ping>;
    Cached<DbConn> db_;
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, db_); }
    void handle(const Ping&) {}
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

    // ---- 1) declare_lazy<A>() compiles + publishes the SAME metadata compile_actor_metadata<A>()
    //         would produce directly — resolved through the engine's own TypeRegistry seam. --------
    {
        Engine<> eng(EngineConfig{1, 1, 16, 64});

        result<std::uint16_t> i0 = eng.declare_lazy<Worker>();
        check(i0.has_value() && i0.value() == 0, "first declare_lazy<Worker> -> index 0", ok);

        const ActorMetadata* m = eng.type_registry().find(type_key_of<Worker>());
        check(m != nullptr, "metadata published under Worker's TypeKey", ok);
        if (m != nullptr) {
            check(m->key == type_key_of<Worker>(), "metadata.key == type_key_of<Worker>", ok);
            check(m->band == 0, "Priority<0> -> band 0", ok);
            check(m->drain_budget == 32, "DrainBudget<32> -> 32", ok);
            check(m->max_concurrency == 1, "Sequential -> max_concurrency 1", ok);
            check(m->dispatch.thunks != nullptr && m->dispatch.size == 1,
                  "dispatch table materialized", ok);
            check(m->construct != nullptr && m->destroy != nullptr && m->reconstruct.fn != nullptr,
                  "008 factories present (construct/destroy/reconstruct)", ok);
        }

        // ---- 2) NO instance/Schedulable was constructed — resolve() sees nothing for this type,
        //         and the engine's own registry() (Schedulable table) gained no entries. -----------
        check(eng.resolve(actor_id_of<Worker>(1)) == nullptr,
              "resolve() finds no Schedulable — declare_lazy never spawns", ok);
        check(eng.resolve(actor_id_of<Worker>(2)) == nullptr,
              "resolve() finds no Schedulable for any key of a declared-only type", ok);

        // ---- 3) A second, distinct type gets the next dense index (mirrors metadata_registry_test's
        //         TypeRegistry-direct proof, now through the Engine seam). ----------------------------
        result<std::uint16_t> i1 = eng.declare_lazy<Other>();
        check(i1.has_value() && i1.value() == 1, "second declare_lazy<Other> -> index 1 (dense)", ok);
        check(eng.type_registry().size() == 2, "engine's TypeRegistry holds 2 records", ok);

        // A real spawn<A>() still works normally alongside declared-but-not-spawned types.
        detail::MessagePool pool(64);
        result<ActorId> spawned = eng.spawn<Worker>(3, pool.sink());
        check(spawned.has_value(), "spawn<A>() is unaffected by an earlier declare_lazy<A>() call", ok);
        check(eng.resolve(*spawned) != nullptr,
              "the actually-spawned instance DOES resolve (unlike the declared-only one)", ok);
    }

    // ---- 4) The optional WireCheck closure: an undeclared resource fails Strict declare_lazy exactly
    //         like it fails Strict spawn<A>() (008 §Validation "Startup") — publishes nothing. --------
    {
        Engine<> eng(EngineConfig{1, 1, 16, 64});
        auto undeclared_check = [] {
            ResourceScope empty;  // provides nothing
            NeedsDb tmp;
            return tmp.wire(empty);
        };
        result<std::uint16_t> bad = eng.declare_lazy<NeedsDb>(undeclared_check);
        check(!bad.has_value() && bad.error().code == errc::validation,
              "Strict declare_lazy: undeclared resource -> errc::validation", ok);
        check(eng.type_registry().size() == 0, "Strict declare_lazy: nothing published on failure", ok);

        DbConn conn;
        ResourceScope scope;
        scope.provide(conn, ResourceLifetime::Activation);
        auto provided_check = [&] {
            NeedsDb tmp;
            return tmp.wire(scope);
        };
        result<std::uint16_t> good = eng.declare_lazy<NeedsDb>(provided_check);
        check(good.has_value(), "declare_lazy succeeds once the resource is provided", ok);
    }

    std::printf("engine_declare_lazy_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
