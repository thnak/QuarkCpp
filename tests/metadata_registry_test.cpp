// Tests 008-Metadata-and-Startup §Metadata compilation + §Type registry — the compiled ActorMetadata
// record and the TypeKey → {metadata, factory} registry:
//   1. compile_actor_metadata<A>() gathers the policy pack (band/budget/max_concurrency/supervision)
//      and the dispatch table with no RTTI/reflection.
//   2. The registry resolves metadata by TypeKey AND by dense type_index; index is dense (0,1,2,…).
//   3. The construct/reconstruct FACTORY builds a fresh actor and reconstructs true fresh state.
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/policies.hpp"
#include "quark/core/supervision.hpp"

using namespace quark;

namespace {

struct Tick {};

struct Worker : Actor<Worker, Sequential, Priority<0>, DrainBudget<32>,
                      OnFailure<Restart, MaxRestarts<3>>> {
    using protocol = Protocol<Tick>;
    int state = 41;  // non-default sentinel so "fresh" (== 41 via ctor) is observable
    void handle(const Tick&) { ++state; }
};

struct Other : Actor<Other, Sequential> {
    using protocol = Protocol<Tick>;
    void handle(const Tick&) {}
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

    // 1) Metadata compilation — the policy pack is gathered correctly.
    const ActorMetadata m = compile_actor_metadata<Worker>();
    check(m.key == type_key_of<Worker>(), "metadata.key == type_key_of<Worker>", ok);
    check(m.band == 0, "Priority<0> → band 0", ok);
    check(m.drain_budget == 32, "DrainBudget<32> → 32", ok);
    check(m.max_concurrency == 1, "Sequential → max_concurrency 1", ok);
    check(m.supervision.decision == SupervisionDirective::Restart, "OnFailure<Restart> resolved", ok);
    check(m.supervision.max_restarts == 3, "MaxRestarts<3> resolved", ok);
    check(m.dispatch.thunks != nullptr && m.dispatch.size == 1, "dispatch table materialized", ok);
    check(m.construct != nullptr && m.reconstruct.fn != nullptr, "factories present", ok);

    // 2) Registry — resolve by key and by dense index; indices are dense.
    TypeRegistry reg;
    result<std::uint16_t> i0 = reg.register_type<Worker>();
    result<std::uint16_t> i1 = reg.register_type<Other>();
    check(i0.has_value() && i0.value() == 0, "first registered type → index 0", ok);
    check(i1.has_value() && i1.value() == 1, "second registered type → index 1 (dense)", ok);
    check(reg.size() == 2, "registry holds 2 records", ok);

    const ActorMetadata* byk = reg.find(type_key_of<Worker>());
    const ActorMetadata* byi = reg.at(0);
    check(byk != nullptr && byi != nullptr && byk == byi, "find(key) == at(index) (same record)", ok);
    check(reg.find(type_key_of<Other>()) == reg.at(1), "Other resolves to index 1", ok);

    // 3) Factory — construct a fresh actor, mutate it, reconstruct → true fresh state, then destroy.
    void* raw = reg.construct(type_key_of<Worker>());
    check(raw != nullptr, "factory constructed an actor", ok);
    auto* w = static_cast<Worker*>(raw);
    check(w->state == 41, "freshly constructed actor has ctor state (41)", ok);
    w->state = 999;                                  // mutate
    reg.reconstruct(type_key_of<Worker>(), raw);     // 007 fresh-state reconstruct factory
    check(w->state == 41, "reconstruct produced FRESH state (41), not the mutated 999", ok);
    reg.destroy(type_key_of<Worker>(), raw);

    std::printf("metadata_registry_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
