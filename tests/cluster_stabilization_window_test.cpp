// Tests 021-Cluster-Formation-and-Lifecycle §3 (stabilization / anti-flap window) deterministically on
// a VIRTUAL clock, modelled on sim_virtual_clock_window_test. A membership change must HOLD for a
// configurable settle interval before it drives re-placement of healthy actors:
//   * a node that FLAPS within the window causes ZERO re-placement (no thrash),
//   * a genuine change SUSTAINED beyond the window commits exactly ONCE, at the settle boundary — not
//     before.
// The window's decision is a pure function of (roster digest, virtual now); we assert the exact
// behavior at virtual times INSIDE vs OUTSIDE the window.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/ids.hpp"

using namespace quark;

namespace {

constexpr std::int64_t kSettle = 300'000'000;  // 300 ms settle interval

std::uint64_t digest_of(std::vector<NodeId> nodes) {
    return roster_digest(std::span<const NodeId>(nodes));
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    const std::uint64_t stable = digest_of({NodeId{1}, NodeId{2}, NodeId{3}});
    const std::uint64_t changed = digest_of({NodeId{1}, NodeId{2}, NodeId{3}, NodeId{4}});  // +node 4

    // ---- FLAP within the window ⇒ 0 re-placements. ------------------------------------------------
    {
        StabilizationWindow win{kSettle};
        win.set_committed(stable);
        int replacements = 0;
        std::int64_t now = 0;

        // t=0: node 4 appears (roster changes) — arm pending, do NOT re-place yet.
        if (win.observe(changed, now)) ++replacements;
        check(win.pending(), "a fresh change arms the settle window (pending)", ok);

        // t=100 ms (INSIDE the window): still changed, still holding — no re-place.
        now = 100'000'000;
        if (win.observe(changed, now)) ++replacements;

        // t=200 ms (INSIDE the window): node 4 flaps back out before settling.
        now = 200'000'000;
        if (win.observe(stable, now)) ++replacements;
        check(!win.pending(), "flapping back to the committed roster clears the pending change", ok);

        // t=1000 ms: long after — the flap must have driven NOTHING.
        now = 1'000'000'000;
        if (win.observe(stable, now)) ++replacements;

        check(replacements == 0, "a flap within the settle window causes ZERO re-placement", ok);
        check(win.committed() == stable, "the committed roster is unchanged by a flap", ok);
    }

    // ---- SUSTAINED change beyond the window ⇒ exactly 1 re-placement, at the settle boundary. ------
    {
        StabilizationWindow win{kSettle};
        win.set_committed(stable);
        int replacements = 0;
        std::int64_t fired_at = -1;

        // t=0: change appears.
        if (win.observe(changed, 0)) ++replacements;
        // t=299 ms: still INSIDE the window (299 < 300) — must NOT fire yet.
        if (win.observe(changed, 299'000'000)) {
            ++replacements;
            fired_at = 299'000'000;
        }
        check(replacements == 0, "no re-placement strictly before the settle interval elapses", ok);

        // t=300 ms: settle interval reached — fire exactly once.
        if (win.observe(changed, 300'000'000)) {
            ++replacements;
            fired_at = 300'000'000;
        }
        check(replacements == 1, "a sustained change re-places exactly once", ok);
        check(fired_at == 300'000'000, "re-placement fires AT the settle boundary, not before", ok);
        check(win.committed() == changed, "the sustained change becomes the committed roster", ok);

        // t=400 ms and beyond: no further re-placement for the same roster (idempotent commit).
        if (win.observe(changed, 400'000'000)) ++replacements;
        if (win.observe(changed, 5'000'000'000)) ++replacements;
        check(replacements == 1, "a settled roster does not re-place again", ok);
    }

    std::printf("cluster_stabilization_window_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
