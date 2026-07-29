// Tests 004-Resources §Rules "Any resource storage backed by a bump/arena allocator ... must carry
// an explicit destructor-thunk list — bulk-reclaiming arena memory does not, by itself, invoke
// placement-constructed destructors" (ADR-021, proven as Eager's C8).
//
// `ResourceArena` (resource.hpp) is the shipped Node/Shard resource storage; it carries exactly such
// a list (see its class banner). This test proves both directions, toggled by a build flag (mirrors
// resource_teardown_order_test.cpp / the repo's existing `-DQUARK_*_CONTROL` convention):
//   * default build (this file, auto-discovered): the SHIPPED `ResourceArena` — every placement-
//     constructed object's destructor MUST run when the arena is torn down (self-checking: a live
//     counter must return to 0 — a correctness assertion independent of any sanitizer), and must be
//     LeakSanitizer-clean (no leaked heap owned by those destructors).
//   * `-DQUARK_RESOURCE_NO_THUNK_LIST` (registered separately in CMakeLists.txt as
//     `resource_arena_thunk_list_control`, WILL_FAIL): a deliberately naive arena — a bare
//     `std::pmr::monotonic_buffer_resource` with NO destructor-thunk list — bulk-reclaims its backing
//     bytes on destruction WITHOUT invoking any placement-constructed destructor. The self-check
//     alone catches this (the live counter stays nonzero — a plain, sanitizer-independent FAIL), and
//     under ASan+LeakSanitizer every leaked `LeakyResource::payload` is ALSO independently reported.
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <utility>

#include "quark/core/resource.hpp"

using namespace quark;

namespace {

// A resource whose destructor must run for its OWN heap allocation to be freed — a skipped
// destructor is therefore a genuine, LeakSanitizer-detectable leak, not just a missed side effect.
struct LeakyResource {
    std::unique_ptr<int> payload;
    static inline int alive = 0;
    explicit LeakyResource(int v) : payload(std::make_unique<int>(v)) { ++alive; }
    ~LeakyResource() { --alive; }
};

#if defined(QUARK_RESOURCE_NO_THUNK_LIST)
// CONTROL: bulk pmr reclaim only — no destructor-thunk list (the exact bug ADR-021 C8 found).
struct ThunklessArena {
    std::pmr::monotonic_buffer_resource mbr{4096, std::pmr::new_delete_resource()};
    template <class T, class... Args>
    T* emplace(Args&&... args) {
        void* mem = mbr.allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }
};
using ArenaUnderTest = ThunklessArena;
#else
using ArenaUnderTest = ResourceArena;  // the shipped, thunk-list-carrying arena (resource.hpp)
#endif

}  // namespace

int main() {
    static constexpr int kCount = 32;
    {
        ArenaUnderTest arena;
        for (int i = 0; i < kCount; ++i) (void)arena.emplace<LeakyResource>(i);
        // `arena` falls out of scope here — the destructor-thunk-list claim is tested exactly HERE.
    }

    const bool clean = (LeakyResource::alive == 0);
    std::printf("resource_arena_thunk_list_test: %s (%d/%d destructors ran)\n",
                clean ? "OK" : "FAIL", kCount - LeakyResource::alive, kCount);
    return clean ? 0 : 1;
}
