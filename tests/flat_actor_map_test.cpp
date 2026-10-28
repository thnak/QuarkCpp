// Tests ADR-064's detail::FlatActorMap (the Engine::resolve() registry): insert/overwrite/find across
// growth, absent keys, adversarial same-low-bits keys, and agreement with std::unordered_map as oracle.
// Implements 006 §Addressing.
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "quark/core/engine.hpp"
#include "quark/detail/flat_actor_map.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}
}  // namespace

int main() {
    bool ok = true;
    detail::FlatActorMap<Schedulable> m;
    check(m.size() == 0 && m.find(ActorId{TypeKey{1}, 1}) == nullptr, "empty map finds nothing", ok);

    constexpr std::uint64_t kN = 20000;  // forces many doublings from the 16-slot start
    std::vector<Schedulable> cells(kN);
    std::unordered_map<ActorId, Schedulable*> oracle;
    for (std::uint64_t i = 0; i < kN; ++i) {
        const ActorId id{TypeKey{(i % 7) + 1}, i * 0x10000ull};  // keys sharing their low bits
        m.insert_or_assign(id, &cells[i]);
        oracle[id] = &cells[i];
    }
    check(m.size() == kN, "size counts distinct ids", ok);

    bool all = true;
    for (const auto& [id, ptr] : oracle) all &= (m.find(id) == ptr);
    check(all, "every inserted id resolves to its own pointer (across growth)", ok);

    bool absent = true;
    for (std::uint64_t i = 0; i < kN; ++i) absent &= (m.find(ActorId{TypeKey{99}, i}) == nullptr);
    check(absent, "ids never inserted resolve to nullptr", ok);

    const ActorId dup{TypeKey{1}, 0};
    m.insert_or_assign(dup, &cells[5]);
    check(m.find(dup) == &cells[5] && m.size() == kN, "re-insert overwrites without growing size", ok);

    std::printf("flat_actor_map_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
