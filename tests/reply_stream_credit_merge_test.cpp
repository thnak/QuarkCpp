// Tests ADR-018 §010 cross-node reply credit-return: the monotone MAX-MERGE `shadow_tail = max(
// shadow_tail, tail)` is idempotent under reorder + duplication (a stale/dup CreditReturn re-asserts a
// tail the producer already saw, never regresses it), whereas an ADDITIVE credit-return over-credits
// under a duplicated packet (the fired control). Plus the process-monotonic stream_id nonce (no ABA on
// the transport stream_id->ring* map). Pure logic, single-thread, deterministic.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/reply_stream.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) { std::fprintf(stderr, "  CHECK FAILED: %s\n", what); ok = false; }
}
}  // namespace

int main() {
    bool ok = true;

    // ---- monotone max-merge under reorder + duplication ----------------------------------------
    {
        CrossNodeCredit c;
        check(c.apply(5) == true, "first CreditReturn(5) advances", ok);
        check(c.shadow_tail() == 5, "shadow_tail == 5", ok);
        check(c.apply(3) == false, "reordered stale CreditReturn(3) does NOT advance", ok);
        check(c.shadow_tail() == 5, "shadow_tail never regresses (still 5)", ok);
        check(c.apply(5) == false, "duplicate CreditReturn(5) is idempotent", ok);
        check(c.shadow_tail() == 5, "duplicate does not double-credit", ok);
        check(c.apply(8) == true, "CreditReturn(8) advances", ok);
        check(c.apply(6) == false, "late reordered CreditReturn(6) after 8 is dropped", ok);
        check(c.shadow_tail() == 8, "final shadow_tail == 8 (the true max)", ok);
    }

    // ---- the ADDITIVE control OVERSHOOTS under duplication (why max-merge is load-bearing) -------
    {
        // Simulate a stream of absolute-tail CreditReturns with reorder + a duplicate. The correct
        // credit granted is the MAX seen. A naive additive scheme sums deltas and, on a duplicated
        // packet, grants credit for a slot range twice — letting the producer overwrite a live slot.
        std::vector<std::uint64_t> returns = {2, 5, 5 /*dup*/, 9, 7 /*reorder*/, 9 /*dup*/};
        CrossNodeCredit maxc;
        std::uint64_t additive = 0, prev = 0;
        for (std::uint64_t r : returns) {
            maxc.apply(r);
            // additive (mis)model: credit += (r - prev_if_greater), but a dup/reorder mis-accounts.
            if (r > prev) { additive += (r - prev); prev = r; }
            else additive += r;  // the bug: a dup/reorder adds spurious credit
        }
        check(maxc.shadow_tail() == 9, "max-merge converges to the true tail (9)", ok);
        check(additive > 9, "control fires: additive credit-return OVERSHOOTS the true tail", ok);
        std::fprintf(stderr, "  (max-merge=%llu, additive-overshoot=%llu)\n",
                     static_cast<unsigned long long>(maxc.shadow_tail()),
                     static_cast<unsigned long long>(additive));
    }

    // ---- stream_id nonce: strictly monotone, never 0 (no ABA on the ring map) --------------------
    {
        std::uint64_t a = next_stream_id();
        std::uint64_t b = next_stream_id();
        std::uint64_t c = next_stream_id();
        check(a != 0 && b != 0 && c != 0, "stream_id nonce is never 0", ok);
        check(b > a && c > b, "stream_id nonce is strictly monotone (no reuse -> no ABA)", ok);
    }

    std::fprintf(stderr, "reply_stream_credit_merge_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
