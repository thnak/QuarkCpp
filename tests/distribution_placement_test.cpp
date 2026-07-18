// Tests 010-Distribution §"Placement across nodes" — HRW/Rendezvous placement is (1) DETERMINISTIC
// and coordinator-free: the same (ActorId, membership view) yields the same NodeId when computed
// independently "on every node", regardless of the order nodes appear in the view; and (2) roughly
// UNIFORM: many ActorIds spread across N nodes with a low coefficient of variation (CoV). Both are
// pure functions of the node set — no engine, no network.
#include <cmath>
#include <cstdint>
#include <cstdio>
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

ActorId actor(std::uint64_t type, std::uint64_t key) { return ActorId{TypeKey{type}, key}; }

// An independent membership snapshot over the given node ids, built in a SPECIFIC insertion order —
// so we can prove placement does not depend on the order the roster was assembled.
MembershipView view_of(const std::vector<std::uint64_t>& order) {
    auto vec = std::make_shared<std::vector<NodeId>>();
    for (auto v : order) vec->push_back(NodeId{v});
    // Placement must be order-independent; we deliberately do NOT sort here.
    return MembershipView{vec, /*epoch*/ 1};
}

}  // namespace

int main() {
    bool ok = true;

    // ---- (1) Determinism across independently-built views (different node orderings) ----------
    {
        std::vector<std::uint64_t> ascending = {10, 20, 30, 40, 50, 60, 70, 80};
        std::vector<std::uint64_t> shuffled = {50, 10, 80, 30, 70, 20, 60, 40};
        std::vector<std::uint64_t> reversed = {80, 70, 60, 50, 40, 30, 20, 10};
        MembershipView va = view_of(ascending);
        MembershipView vb = view_of(shuffled);
        MembershipView vc = view_of(reversed);

        bool all_same = true;
        constexpr std::uint64_t M = 50'000;
        for (std::uint64_t k = 0; k < M; ++k) {
            const ActorId id = actor(0xABCDEF, k);
            const auto a = place(id, va);
            const auto b = place(id, vb);
            const auto c = place(id, vc);
            if (!a || !b || !c || *a != *b || *a != *c) all_same = false;
        }
        check(all_same,
              "placement is order-independent (same NodeId from 3 independently-ordered views)", ok);

        // Also: the real seam — two InProcessMembership instances with different self() but the same
        // roster compute identical ownership (coordinator-free, every node agrees).
        InProcessMembership node1(NodeId{10}, {NodeId{10}, NodeId{20}, NodeId{30}, NodeId{40}});
        InProcessMembership node4(NodeId{40}, {NodeId{40}, NodeId{30}, NodeId{20}, NodeId{10}});
        bool agree = true;
        for (std::uint64_t k = 0; k < M; ++k) {
            const ActorId id = actor(7, k);
            if (place(id, node1.view()) != place(id, node4.view())) agree = false;
        }
        check(agree, "two nodes with the same roster compute the SAME owner for every ActorId", ok);
    }

    // ---- (2) Uniformity: CoV of the per-node share across N nodes ----------------------------
    {
        constexpr std::uint64_t N = 8;
        std::vector<std::uint64_t> ids;
        for (std::uint64_t i = 1; i <= N; ++i) ids.push_back(i * 1000 + 7);  // arbitrary node ids
        MembershipView v = view_of(ids);

        std::vector<std::uint64_t> count(N + 1, 0);  // by index into `ids`
        std::vector<std::uint64_t> idx_of;           // node value → index lookup (small N, linear)
        constexpr std::uint64_t M = 200'000;
        for (std::uint64_t k = 0; k < M; ++k) {
            const auto owner = place(actor(0x55AA, k), v);
            check(owner.has_value(), "non-empty view always places", ok);
            for (std::uint64_t i = 0; i < N; ++i)
                if (ids[i] == owner->value) {
                    ++count[i];
                    break;
                }
        }

        double mean = static_cast<double>(M) / static_cast<double>(N);
        double var = 0.0;
        std::uint64_t idle = 0;
        for (std::uint64_t i = 0; i < N; ++i) {
            const double d = static_cast<double>(count[i]) - mean;
            var += d * d;
            if (count[i] == 0) ++idle;
        }
        var /= static_cast<double>(N);
        const double cov = std::sqrt(var) / mean;
        std::printf("  placement CoV over N=%llu nodes, M=%llu ids: %.4f (idle nodes=%llu)\n",
                    static_cast<unsigned long long>(N), static_cast<unsigned long long>(M), cov,
                    static_cast<unsigned long long>(idle));
        check(idle == 0, "every node owns at least one actor (no idle node)", ok);
        check(cov < 0.05, "distribution is roughly uniform (CoV < 0.05)", ok);
    }

    // ---- Empty view degenerates to nullopt (single-node fallback is the caller's) -------------
    {
        MembershipView empty;
        check(!place(actor(1, 1), empty).has_value(), "empty membership → no placement (nullopt)", ok);
    }

    std::printf("distribution_placement_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
