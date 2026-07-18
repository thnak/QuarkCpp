// Quark sample 10 — Replacement on membership change (010 minimal disruption).
//
// When the cluster's node set changes, actors must be re-placed — but rendezvous (HRW) placement moves
// as FEW actors as mathematically possible, and only the right ones. That is what makes membership
// churn cheap and coordinator-free:
//   * JOIN(x): an actor moves IFF x becomes its new argmax winner. Every moved actor now lives on x,
//     and NO actor ever moves between two nodes that both survived. Moved fraction ≈ 1/(N+1).
//   * LEAVE(x): only actors that were on x re-place; everyone else keeps their owner, and no re-placed
//     actor maps to x (it's gone). Moved fraction ≈ x's share ≈ 1/N.
//
// Contrast: naive `hash(key) % N` placement remaps almost EVERYTHING on any change. HRW remaps ~1/N.
//
// Pure function of the node set — no engine, no network.
//
// Build:  cmake -B build -DQUARK_BUILD_SAMPLES=ON && cmake --build build --target 10_replacement
// Run  :  taskset -c 0-3 build/samples/10_replacement
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

using namespace quark;

static ActorId actor(std::uint64_t key) { return ActorId{TypeKey{0xC0FFEE}, key}; }

int main() {
    bool ok = true;
    constexpr std::uint64_t K = 200'000;
    constexpr std::uint64_t N = 8;

    InProcessMembership m(NodeId{1}, {NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5},
                                      NodeId{6}, NodeId{7}, NodeId{8}});

    // Baseline ownership of K actors over the 8-node cluster.
    std::vector<std::uint64_t> before(K);
    {
        MembershipView v = m.view();
        for (std::uint64_t k = 0; k < K; ++k) before[k] = place(actor(k), v)->value;
    }

    // ---- JOIN node 9: only actors whose new winner is 9 should move ----------------------------
    m.join(NodeId{9});
    {
        MembershipView v = m.view();
        std::uint64_t moved = 0, moved_to_new = 0, moved_between_old = 0;
        for (std::uint64_t k = 0; k < K; ++k) {
            const std::uint64_t now = place(actor(k), v)->value;
            if (now != before[k]) {
                ++moved;
                if (now == 9) ++moved_to_new; else ++moved_between_old;
            }
        }
        const double frac = static_cast<double>(moved) / static_cast<double>(K);
        ok &= (moved_between_old == 0) && (moved_to_new == moved) && (frac > 0.07 && frac < 0.16);
        std::printf("JOIN(9):  moved=%.3f%%  (ideal 1/9=%.3f%%);  all moved -> node 9: %s;  moved between survivors: %llu\n",
                    frac * 100, 100.0 / 9.0, (moved_to_new == moved) ? "yes" : "NO",
                    (unsigned long long)moved_between_old);
    }

    // Refresh baseline (now a 9-node cluster), then LEAVE a node.
    std::vector<std::uint64_t> mid(K);
    {
        MembershipView v = m.view();
        for (std::uint64_t k = 0; k < K; ++k) mid[k] = place(actor(k), v)->value;
    }

    // ---- LEAVE node 3: only actors that were ON node 3 should re-place --------------------------
    m.leave(NodeId{3});
    {
        MembershipView v = m.view();
        std::uint64_t moved = 0, moved_not_owned_by_3 = 0, re_placed_to_3 = 0, was_on_3 = 0;
        for (std::uint64_t k = 0; k < K; ++k) {
            if (mid[k] == 3) ++was_on_3;
            const std::uint64_t now = place(actor(k), v)->value;
            if (now != mid[k]) {
                ++moved;
                if (mid[k] != 3) ++moved_not_owned_by_3;  // MUST be 0: survivors keep their owner
            }
            if (now == 3) ++re_placed_to_3;               // MUST be 0: node 3 is gone
        }
        ok &= (moved == was_on_3) && (moved_not_owned_by_3 == 0) && (re_placed_to_3 == 0);
        std::printf("LEAVE(3): re-placed %llu actors == the %llu that were on node 3;  survivors disturbed: %llu;  any left on 3: %llu\n",
                    (unsigned long long)moved, (unsigned long long)was_on_3,
                    (unsigned long long)moved_not_owned_by_3, (unsigned long long)re_placed_to_3);
    }

    std::printf("%s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
