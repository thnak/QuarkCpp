// Tests 004-Resources §Rules "Node/Shard resource storage outlives owned actors/activations"
// (ADR-021, proven as Eager's C7): the Engine's Node/Shard resource storage (`ResourceArena` +
// `ResourceScope`, resource.hpp) must be torn down AFTER every container of owned actors/activations,
// so an actor destructor that touches a `Cached<>` resource never runs against already-freed memory.
//
// `engine.hpp` gets this right by declaring `node_storage_`/`shard_storage_`/`shard_scopes_` as the
// FIRST data members (see the banner above `ResourceArena node_storage_;`) — first-declared members
// are destroyed LAST (C++'s reverse-declaration-order member destruction), so they outlive `shards_`/
// `owned_actors_`/`owned_activations_`/`registry_` below them.
//
// This test proves BOTH directions with the SAME `Owner`/`ActorLike` shape, toggled by a build flag —
// exactly the existing repo convention (compare `sched_no_lost_wakeup_test.cpp` + its
// `-DQUARK_SCHED_BROKEN_WAKEUP` control):
//   * default build (this file, auto-discovered): the FIXED order (storage declared first) — must be
//     ASan-clean and reach a clean logical teardown (TrackedResource::alive back to 0).
//   * `-DQUARK_RESOURCE_BAD_TEARDOWN_ORDER` (registered separately in CMakeLists.txt as
//     `resource_teardown_order_control`, WILL_FAIL): the REVERSED order (storage declared LAST, so
//     it is destroyed FIRST) — `~ActorLike()` then touches memory the arena has already freed, a
//     heap-use-after-free ASan MUST catch. This is the sensitivity check: without it, nothing proves
//     the "fixed" build's cleanliness is because the order is right, rather than the test being
//     vacuous.
#include <cstdio>
#include <vector>

#include "quark/core/resource.hpp"

using namespace quark;

namespace {

struct TrackedResource {
    int value;
    static inline int alive = 0;
    explicit TrackedResource(int v) : value(v) { ++alive; }
    ~TrackedResource() { --alive; }
};

// Mirrors an actor whose destructor touches a `Cached<T>`-style resolved pointer (e.g. a pool-
// checkout RAII guard releasing back into a Node/Shard-scoped pool at actor teardown, 004 §Rules).
struct ActorLike {
    TrackedResource* res;
    int last_seen = -1;
    ~ActorLike() { last_seen = res->value; }  // the touch-after-teardown hazard this test targets
};

#if defined(QUARK_RESOURCE_BAD_TEARDOWN_ORDER)
// CONTROL: actor container declared BEFORE resource storage -> storage is destroyed FIRST (reverse-
// declaration-order member destruction) -> ~ActorLike() above runs against freed memory.
struct Owner {
    std::vector<ActorLike> actors;
    ResourceArena storage;
};
#else
// FIXED — mirrors engine.hpp's actual member order: resource storage declared FIRST -> destroyed
// LAST -> every ActorLike destructor runs while its resource is still alive.
struct Owner {
    ResourceArena storage;
    std::vector<ActorLike> actors;
};
#endif

}  // namespace

int main() {
    bool clean = true;
    {
        Owner owner;
        TrackedResource* res = owner.storage.emplace<TrackedResource>(99);
        for (int i = 0; i < 4; ++i) owner.actors.push_back(ActorLike{res, -1});
        // `owner` falls out of scope at the end of this block — the teardown-order claim is tested
        // exactly HERE, in `~Owner()`'s implicit member-destruction sequence.
    }
    // Reached only if nothing crashed (ASan aborts the process on the bad-order control's UAF before
    // ever returning here). A clean run must also have released the resource exactly once.
    clean = (TrackedResource::alive == 0);

    std::printf("resource_teardown_order_test: %s (alive=%d after Owner teardown)\n",
                clean ? "OK" : "FAIL", TrackedResource::alive);
    return clean ? 0 : 1;
}
