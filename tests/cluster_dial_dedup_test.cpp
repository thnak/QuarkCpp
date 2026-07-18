// Tests 021-Cluster-Formation-and-Lifecycle §2 (dial deduplication rule). When A and B dial each other
// concurrently, two connections form, violating 010's one-connection-per-peer invariant. It is
// resolved deterministically with NO negotiation round-trip: the connection INITIATED BY THE LOWER
// NodeId is kept, the other closed — and BOTH ends compute the same winner from ids alone. This is the
// pure RULE (real socket teardown is 019); here we prove it is symmetric, total, and mutually exclusive.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "quark/core/cluster.hpp"

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

    // Spot check: the lower id's dial survives, and both ends agree regardless of argument order.
    check(dial_winner(NodeId{1}, NodeId{2}) == NodeId{1}, "lower NodeId wins the dedup", ok);
    check(dial_winner(NodeId{2}, NodeId{1}) == NodeId{1}, "winner is independent of argument order", ok);

    // Exhaustive over a range: for every distinct ordered pair, the RULE must be
    //   (1) symmetric   — both ends compute the same surviving connection,
    //   (2) the lower id — the survivor is the min id,
    //   (3) mutually exclusive — exactly ONE side keeps the connection it initiated.
    for (std::uint64_t a = 0; a < 24; ++a) {
        for (std::uint64_t b = 0; b < 24; ++b) {
            if (a == b) continue;
            const NodeId A{a}, B{b};

            const NodeId w_ab = dial_winner(A, B);
            const NodeId w_ba = dial_winner(B, A);
            check(w_ab == w_ba, "both ends compute the same winner (symmetric)", ok);
            check(w_ab.value == (a < b ? a : b), "the survivor is the lower NodeId", ok);

            // From each end's local vantage: exactly one of them keeps ITS OWN initiated connection.
            const bool a_keeps = keep_local_dial(A, B);
            const bool b_keeps = keep_local_dial(B, A);
            check(a_keeps != b_keeps, "exactly one end keeps its own dial (no double, no zero)", ok);
            // The end that keeps its own dial is the lower id — i.e. the dial_winner.
            const NodeId keeper = a_keeps ? A : B;
            check(keeper == w_ab, "the end that keeps its dial is the computed winner", ok);
        }
    }

    std::printf("cluster_dial_dedup_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
