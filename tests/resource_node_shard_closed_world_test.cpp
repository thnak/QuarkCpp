// Tests ADR-021's closed-world constraint (008 §"Resource-type closed-world constraint", proven as
// Eager's claim C6): a guarded `declare_lazy<A>()` (the shipped mechanism 008-Metadata-and-Startup.md
// names "guarded `add_actor_type<T>()`" in prose — ADR-008) whose `wire()` references a Node- or
// Shard-scoped resource type that was NEVER `provide_node`/`provide_shard`'d to the Engine's
// constructor MUST fail incremental Validation — `std::unexpected`, publish NOTHING, and leave the
// resource/type tables pointer-identical — never attempt live resolution against running workers.
//
// No new validation machinery is needed for this: the Engine's eager pass (ADR-021) builds each
// shard's `ResourceScope` from EXACTLY the types in the plan, so an actor's `wire()` referencing an
// unregistered type hits the SAME "undeclared resource" path `ResourceScope::resolve<T>()` already
// implements (`errc::validation`) — this test proves that closed-world property end-to-end through
// the real Engine seam.
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/resource.hpp"
#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct Ping {};

// A perfectly ordinary, already-registered-at-build actor: proves the frozen record's pointer/index
// survives a LATER failed declare_lazy<Bad> untouched.
struct Warm : Actor<Warm, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};

// A Node-scoped resource type that IS provided to the Engine's constructor.
struct KnownPool {
    int tag = 1;
};

// A Node-scoped resource type that is NEVER provided anywhere — the closed-world violator.
struct UnknownStore {
    int tag = 2;
};

struct GoodActor : Actor<GoodActor, Sequential> {
    using protocol = Protocol<Ping>;
    Cached<KnownPool> pool_;
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, pool_); }
    void handle(const Ping&) {}
};

// References a Node-scoped resource type that was never registered at first build() — the
// closed-world violation this test exists to catch.
struct BadActor : Actor<BadActor, Sequential> {
    using protocol = Protocol<Ping>;
    Cached<UnknownStore> store_;
    [[nodiscard]] result<void> wire(const ResourceScope& s) { return wire_resources(s, store_); }
    void handle(const Ping&) {}
};

KnownPool make_known_pool() { return KnownPool{7}; }

struct Trailing : Actor<Trailing, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) {}
};

}  // namespace

int main() {
    bool ok = true;

    NodeShardResourcePlan plan;
    plan.provide_node<KnownPool>(&make_known_pool);  // UnknownStore deliberately NOT provided anywhere

    Engine<> eng(EngineConfig{1, 2, 16, 64}, plan);

    // A real, already-published record to compare against after the failed attempt below.
    result<std::uint16_t> warm_idx = eng.declare_lazy<Warm>();
    check(warm_idx.has_value() && warm_idx.value() == 0, "Warm registers first, at index 0", ok);

    // A resource type that WAS provided — declare_lazy<GoodActor> against the Engine's resolved
    // shard-0 scope must succeed (the closed-world set DOES contain KnownPool).
    result<std::uint16_t> good = eng.declare_lazy<GoodActor>(&eng.shard_resource_scope(0));
    check(good.has_value() && good.value() == 1, "GoodActor (known resource type) registers cleanly",
          ok);

    // Captured immediately BEFORE the failed attempt below (no successful registration — and
    // therefore no `records_` reallocation — happens in between): the exact "table pointer +
    // high-water-mark unchanged" claim ADR-021 C6 proves is about the FAILED attempt itself, not
    // about surviving an unrelated LATER successful push_back (which may legitimately reallocate
    // `TypeRegistry`'s backing vector, same as any `std::vector::push_back`).
    const ActorMetadata* warm_before = eng.type_registry().find(type_key_of<Warm>());
    check(warm_before != nullptr, "Warm's record is published", ok);
    const std::size_t size_before = eng.type_registry().size();

    // ---- The closed-world violation: BadActor references UnknownStore, never provided anywhere ---
    result<std::uint16_t> bad = eng.declare_lazy<BadActor>(&eng.shard_resource_scope(0));
    check(!bad.has_value(), "declare_lazy<BadActor> (unregistered resource type) FAILS", ok);
    if (!bad.has_value())
        check(bad.error().code == errc::validation,
              "the failure is errc::validation (undeclared resource), not a crash/live-resolution "
              "attempt",
              ok);

    // ---- Publishes NOTHING: the type set/table is exactly as it was before the failed attempt -----
    check(eng.type_registry().size() == size_before,
          "size is UNCHANGED by the failed attempt — BadActor published NOTHING", ok);
    check(eng.type_registry().find(type_key_of<BadActor>()) == nullptr,
          "BadActor's TypeKey resolves to nothing — never published", ok);

    // ---- Resource/type tables are pointer-identical for every PRE-EXISTING record ------------------
    const ActorMetadata* warm_after = eng.type_registry().find(type_key_of<Warm>());
    check(warm_after == warm_before,
          "Warm's record pointer is IDENTICAL after the failed declare_lazy<BadActor> (no "
          "reallocation/mutation of the existing table)",
          ok);
    check(warm_after != nullptr && warm_after->index == 0, "Warm's index is untouched (still 0)", ok);

    // A subsequent, VALID declare_lazy still works normally — Strict Validation failure of one type
    // never poisons the registry for the next.
    result<std::uint16_t> trailing = eng.declare_lazy<Trailing>();
    check(trailing.has_value() && trailing.value() == 2,
          "registration continues normally after the rejected BadActor (dense index 2)", ok);

    std::printf("resource_node_shard_closed_world_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
