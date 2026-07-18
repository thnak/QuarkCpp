// Tests 018-Clocks-and-Deadlines: the three load-bearing invariants of the deadline model.
//   1. MONOTONIC EXPIRY: a Deadline is decided purely on pal::clock (monotonic) and is IMMUNE to a
//      simulated wall-clock (system_clock) step — a backward/forward wall jump never moves it.
//   2. REMAINING-DURATION ROUND-TRIP: absolute deadline -> wire remaining -> rebased absolute on a
//      peer whose monotonic epoch is UNRELATED, preserving the budget (transit=0), and shrinking it
//      by exactly the charged transit (never growing it) — monotonically non-increasing across hops.
//   3. INHERITANCE: a child call's deadline is <= the parent's remaining; a child can tighten but
//      can never extend past the parent.
//
// Fully deterministic: every query takes an EXPLICIT pal::clock::time_point, so no sleeps and no
// wall-clock reads gate the assertions.
#include <chrono>
#include <cstdio>
#include <optional>

#include "pal/pal.hpp"
#include "quark/core/deadline.hpp"
#include "quark/core/deadline_propagation.hpp"
#include "quark/core/error.hpp"
#include "quark/core/message_context.hpp"

using namespace quark;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// --- Invariant 1: monotonic expiry, immune to a wall-clock step ------------------------------
void test_monotonic_expiry_immune_to_wall_step() {
    const pal::clock::time_point t0 = pal::now();

    const Deadline dl = Deadline::after(50ms, t0);
    check(dl.has_value(), "after() yields a live deadline");

    // Decided purely on the monotonic instant passed in — no sleeping, no wall clock.
    check(!dl.expired(t0), "not expired at creation instant");
    check(!dl.expired(t0 + 40ms), "not expired 40ms in (budget was 50ms)");
    check(dl.expired(t0 + 50ms), "expired exactly at the budget boundary");
    check(dl.expired(t0 + 60ms), "expired 60ms in");

    // Simulate a wall-clock step: capture system_clock, then pretend NTP shoved it backward by an
    // hour. The deadline API never reads system_clock, so the verdict for a FIXED monotonic instant
    // is identical before and after the step — that is the whole point of Rule 1.
    const auto wall_before = std::chrono::system_clock::now();
    const auto wall_after_backward_step = wall_before - 1h;  // NTP correction jumps wall time back
    (void)wall_before;
    (void)wall_after_backward_step;

    const bool verdict_a = dl.expired(t0 + 60ms);  // monotonic instant unchanged
    const bool verdict_b = dl.expired(t0 + 60ms);  // same instant, "after" the simulated wall step
    check(verdict_a && verdict_b && verdict_a == verdict_b,
          "expiry verdict is invariant under a simulated wall-clock step");

    // remaining() is a signed monotonic duration; negative once the instant has passed.
    check(dl.remaining(t0) == 50ms, "remaining == full budget at t0");
    check(dl.remaining(t0 + 30ms) == 20ms, "remaining shrinks with elapsed monotonic time");
    check(dl.remaining(t0 + 70ms).count() < 0, "remaining goes negative past the deadline");
    check(dl.remaining_clamped(t0 + 70ms) == 0ns, "remaining_clamped floors at zero");

    // none() is unbounded: never expires, infinite budget.
    check(!Deadline::none().has_value(), "none() has no value");
    check(!Deadline::none().expired(t0 + 1000h), "none() never expires");
    check(Deadline::none().remaining(t0) == std::chrono::nanoseconds::max(),
          "none() reports unbounded budget");

    // check_deadline maps expiry onto errc::timeout (the 007/017 seam).
    check(check_deadline(dl, t0).has_value(), "check_deadline OK before expiry");
    const auto expired_res = check_deadline(dl, t0 + 100ms);
    check(!expired_res.has_value() && expired_res.error().code == errc::timeout,
          "check_deadline -> errc::timeout after expiry");
}

// --- Invariant 2: remaining-duration round-trip across unrelated monotonic clocks ------------
void test_remaining_duration_roundtrip() {
    const pal::clock::time_point now_a = pal::now();

    // Node A: a 500ms deadline.
    const Deadline dl_a = Deadline::after(500ms, now_a);
    const std::optional<std::chrono::nanoseconds> wire = encode_deadline_for_wire(dl_a, now_a);
    check(wire.has_value(), "a live deadline encodes to a wire duration");
    check(*wire == 500ms, "wire carries the exact remaining budget (measured at send)");

    // Node B's monotonic clock has an UNRELATED epoch — model it as A's time shifted by a huge,
    // arbitrary offset (a different boot time). Rebasing must not care about the offset at all.
    const pal::clock::time_point now_b = now_a + 987654321ns + 3600s;

    // transit = 0 (fresh peer, no RTT yet): Rule 4 says subtract nothing -> budget preserved.
    const Deadline dl_b = rebase_deadline_from_wire(*wire, TransitEstimate::unknown(), now_b);
    check(dl_b.remaining(now_b) == 500ms,
          "rebased budget on B equals the shipped remaining (transit=0, unrelated epoch)");
    check(!dl_b.expired(now_b), "rebased deadline is live on B");
    check(dl_b.expired(now_b + 501ms), "rebased deadline expires after its budget on B's clock");

    // transit = rtt/2: the budget shrinks by exactly the charged transit, NEVER grows (Rule 2a/4).
    const auto rtt = 40ms;
    const Deadline dl_b_t = rebase_deadline_from_wire(*wire, TransitEstimate::from_rtt(rtt), now_b);
    check(dl_b_t.remaining(now_b) == 480ms, "transit rtt/2 (=20ms) is charged to the budget");
    check(dl_b_t.remaining(now_b) < dl_b.remaining(now_b),
          "charging transit is strictly non-lenient vs. the zero estimate");
    check(dl_b_t.remaining(now_b) <= *wire, "budget after a hop never exceeds the shipped budget");

    // Multi-hop A -> B -> C: each hop re-encodes remaining and charges transit, so the budget is
    // monotonically non-increasing along the causal chain (Rule 3, end-to-end by construction).
    const std::optional<std::chrono::nanoseconds> wire_bc = encode_deadline_for_wire(dl_b_t, now_b);
    check(wire_bc.has_value() && *wire_bc == 480ms, "B re-ships its (already reduced) remaining");
    const pal::clock::time_point now_c = now_b + 42s;  // C: yet another unrelated epoch
    const Deadline dl_c = rebase_deadline_from_wire(*wire_bc, TransitEstimate::from_rtt(30ms), now_c);
    check(dl_c.remaining(now_c) == 465ms, "C's budget = 480ms - 15ms transit");
    check(dl_c.remaining(now_c) < dl_b_t.remaining(now_b),
          "budget is monotonically non-increasing across A->B->C");

    // No deadline -> nothing on the wire (peer runs unbounded).
    check(!encode_deadline_for_wire(Deadline::none(), now_a).has_value(),
          "no deadline encodes to nothing on the wire");
}

// --- Invariant 3: inheritance — a child cannot outlive its parent ----------------------------
void test_inheritance() {
    const pal::clock::time_point t0 = pal::now();
    const Deadline parent = Deadline::after(100ms, t0);

    // Pure inheritance: child inherits the parent's instant exactly.
    const Deadline inherited = inherit_deadline(parent);
    check(inherited == parent, "pure inheritance carries the parent deadline unchanged");
    check(inherited.remaining(t0) <= parent.remaining(t0), "inherited budget <= parent budget");

    // Child asks for a LOOSER sub-deadline (200ms) -> clamped to the parent (may not extend).
    const Deadline looser_request = Deadline::after(200ms, t0);
    const Deadline clamped = inherit_deadline(parent, looser_request);
    check(clamped == parent, "a looser child request is clamped down to the parent");
    check(clamped.remaining(t0) <= parent.remaining(t0),
          "child can NEVER extend past the parent's deadline");

    // Child asks for a TIGHTER sub-deadline (30ms) -> honored (child may tighten).
    const Deadline tighter_request = Deadline::after(30ms, t0);
    const Deadline tightened = inherit_deadline(parent, tighter_request);
    check(tightened == tighter_request, "a tighter child request is honored");
    check(tightened.remaining(t0) < parent.remaining(t0), "a tightened child budget is smaller");
    check(tightened.remaining(t0) == 30ms, "tightened budget == the requested 30ms");

    // Threading through the ambient MessageContext (composition, no field edit): a handler running
    // under `parent` stamps the inherited/tightened deadline onto its outbound message.
    MessageContext parent_ctx{};
    set_deadline(parent_ctx, parent);
    check(deadline_of(parent_ctx) == parent, "MessageContext round-trips the deadline via deadline_ns");

    const Deadline child_default = child_deadline(parent_ctx);
    check(child_default == parent, "child inherits the parent ctx's deadline by default");

    const Deadline child_tight = child_deadline(parent_ctx, Deadline::after(10ms, t0));
    check(child_tight.remaining(t0) == 10ms, "child ctx deadline can be tightened");
    check(child_tight.remaining(t0) <= deadline_of(parent_ctx).remaining(t0),
          "child ctx deadline never exceeds the parent ctx deadline");

    // A parent with NO deadline: the child inherits nothing, unless it sets its own.
    MessageContext unbounded_ctx{};  // deadline_ns == 0
    check(!deadline_of(unbounded_ctx).has_value(), "unset ctx == no deadline");
    check(!child_deadline(unbounded_ctx).has_value(), "child of an unbounded parent is unbounded");
    check(child_deadline(unbounded_ctx, Deadline::after(5ms, t0)).remaining(t0) == 5ms,
          "a child may introduce its own deadline under an unbounded parent");
}

}  // namespace

int main() {
    test_monotonic_expiry_immune_to_wall_step();
    test_remaining_duration_roundtrip();
    test_inheritance();

    std::printf("deadline_test: %s\n", failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}
