// Tests 020-Security §1 — cluster admission GATES placement: an unauthenticated/unadmitted node is
// INVISIBLE to HRW placement. Because placement is any-node-hosts-any-actor (010), a node that cannot
// be selected can never be assigned an actor and can never pull actor state. This is the sharpest
// distributed control, so it is proven over MANY actor ids, not a single case.
//
// CONTROL (adversarial, must FIRE): a rogue node is LIVE in the membership view but NOT admitted by the
// NodeAuthority. Over N distinct actor ids, `place_admitted` must NEVER return it — even for the ids
// whose UNGATED HRW winner IS the rogue node (proving the gate actually excludes a would-be winner).
#include <cstdio>
#include <vector>

#include "quark/core/membership.hpp"
#include "quark/core/node_authority.hpp"
#include "quark/core/placement.hpp"

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

    // A 4-node view; node 9 is the ROGUE (live, but not admitted).
    const NodeId rogue{9};
    InProcessMembership mem(NodeId{1}, {NodeId{1}, NodeId{2}, NodeId{3}, rogue});
    const MembershipView view = mem.view();
    check(view.contains(rogue), "precondition: rogue node is LIVE in the view", ok);

    // The trust anchor admits 1,2,3 — NOT the rogue.
    AllowlistNodeAuthority authority;
    authority.admit(NodeId{1});
    authority.admit(NodeId{2});
    authority.admit(NodeId{3});

    constexpr std::uint64_t kN = 5000;
    std::uint64_t rogue_ungated_wins = 0;
    std::uint64_t rogue_gated_wins = 0;
    std::uint64_t gated_empty = 0;

    for (std::uint64_t k = 0; k < kN; ++k) {
        const ActorId id{TypeKey{0xA000}, k};

        // UNGATED placement over the full live set (010) — the rogue CAN win here.
        const auto ungated = place(id, view);
        if (ungated && *ungated == rogue) ++rogue_ungated_wins;

        // ADMISSION-GATED placement — the rogue must be excluded.
        const auto gated = place_admitted(id, view, authority);
        if (!gated) {
            ++gated_empty;
        } else {
            if (*gated == rogue) ++rogue_gated_wins;
            check(authority.admitted(*gated), "gated winner is always an admitted node", ok);
        }
    }

    // The gate is non-vacuous ONLY if the rogue would otherwise have won some ids.
    check(rogue_ungated_wins > 0, "non-vacuous: the rogue WOULD win some ids ungated (HRW)", ok);
    check(rogue_gated_wins == 0, "CONTROL: admission-gated placement NEVER selects the rogue node", ok);
    check(gated_empty == 0, "every id still places onto an admitted node (3 remain)", ok);

    // Revoke node 3 too: gated placement still never picks the rogue, only 1 or 2.
    authority.revoke(NodeId{3});
    for (std::uint64_t k = 0; k < 1000; ++k) {
        const ActorId id{TypeKey{0xB000}, k};
        const auto gated = place_admitted(id, view, authority);
        check(gated && (*gated == NodeId{1} || *gated == NodeId{2}),
              "after revoke: only the two admitted nodes can win", ok);
    }

    std::printf("security_admission_placement_test: %s (rogue ungated-wins=%llu gated-wins=%llu)\n",
                ok ? "OK" : "FAIL", static_cast<unsigned long long>(rogue_ungated_wins),
                static_cast<unsigned long long>(rogue_gated_wins));
    return ok ? 0 : 1;
}
