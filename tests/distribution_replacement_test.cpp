// Tests 010-Distribution §"Membership" re-placement — HRW's MINIMAL-DISRUPTION property. Adding or
// removing a node re-places ONLY the actors whose argmax changed, and no actor ever moves between two
// nodes that both survive the change:
//   * JOIN(x): an actor moves IFF x becomes its new winner; every moved actor now maps to x, and no
//     actor moves between two pre-existing nodes. Expected moved fraction ≈ 1/(N+1).
//   * LEAVE(x): only actors previously owned by x re-place; every other actor keeps its owner, and no
//     re-placed actor maps to x. Expected moved fraction ≈ (share of x) ≈ 1/N.
// This is what makes membership churn cheap and coordinator-free (placement.hpp / ADR-006 C2).
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

using namespace quark;

namespace {

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

ActorId actor(std::uint64_t key) { return ActorId{TypeKey{0xC0FFEE}, key}; }

}  // namespace

int main() {
    bool ok = true;
    constexpr std::uint64_t K = 200'000;
    constexpr std::uint64_t N = 8;
    (void)N;

    InProcessMembership m(NodeId{1}, {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5},
                                      NodeId{6}, NodeId{7}, NodeId{8}});

    // Baseline ownership.
    std::vector<std::uint64_t> before(K);
    {
        MembershipView v = m.view();
        for (std::uint64_t k = 0; k < K; ++k) before[k] = place(actor(k), v)->value;
    }

    // ---- JOIN a new node (id 9) --------------------------------------------------------------
    {
        m.join(NodeId{9});
        MembershipView v = m.view();
        std::uint64_t moved = 0, moved_to_new = 0, moved_between_old = 0;
        for (std::uint64_t k = 0; k < K; ++k) {
            const std::uint64_t now = place(actor(k), v)->value;
            if (now != before[k]) {
                ++moved;
                if (now == 9) ++moved_to_new;
                else ++moved_between_old;
            }
        }
        const double frac = static_cast<double>(moved) / static_cast<double>(K);
        std::printf("  JOIN(9): moved=%.4f (ideal 1/9=%.4f), all-to-new=%llu, between-old=%llu\n",
                    frac, 1.0 / 9.0, static_cast<unsigned long long>(moved_to_new),
                    static_cast<unsigned long long>(moved_between_old));
        check(moved_between_old == 0, "JOIN: no actor moves between two pre-existing nodes", ok);
        check(moved_to_new == moved, "JOIN: every moved actor now maps to the new node", ok);
        check(frac > 0.07 && frac < 0.16, "JOIN: moved fraction ≈ 1/(N+1) (minimal disruption)", ok);
    }

    // Refresh baseline to the 9-node roster.
    {
        MembershipView v = m.view();
        for (std::uint64_t k = 0; k < K; ++k) before[k] = place(actor(k), v)->value;
    }

    // ---- LEAVE a node (id 3) -----------------------------------------------------------------
    {
        m.leave(NodeId{3});
        MembershipView v = m.view();
        std::uint64_t moved = 0, moved_from_three = 0, moved_from_other = 0, moved_to_three = 0;
        for (std::uint64_t k = 0; k < K; ++k) {
            const std::uint64_t now = place(actor(k), v)->value;
            if (now != before[k]) {
                ++moved;
                if (before[k] == 3) ++moved_from_three;
                else ++moved_from_other;
                if (now == 3) ++moved_to_three;
            }
        }
        const double frac = static_cast<double>(moved) / static_cast<double>(K);
        std::printf("  LEAVE(3): moved=%.4f (ideal 1/9=%.4f), from-3=%llu, from-other=%llu\n", frac,
                    1.0 / 9.0, static_cast<unsigned long long>(moved_from_three),
                    static_cast<unsigned long long>(moved_from_other));
        check(moved_from_other == 0, "LEAVE: only actors owned by the departed node re-place", ok);
        check(moved_to_three == 0, "LEAVE: no actor re-places onto the departed node", ok);
        check(moved == moved_from_three, "LEAVE: all moved actors were owned by the departed node", ok);
        check(frac > 0.06 && frac < 0.18, "LEAVE: moved fraction ≈ share of the departed node", ok);
    }

    std::printf("distribution_replacement_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
