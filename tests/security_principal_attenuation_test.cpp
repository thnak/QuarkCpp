// Tests 020-Security §3 — principal propagation is ATTENUATING ONLY: a handler may substitute a WEAKER
// principal downstream (least-privilege delegation), but the runtime stamps outbound principals by
// INTERSECTING rights with the inbound context and can NEVER forge a STRONGER one (the inverse of the
// deadline's monotonic tightening, 018). Mirrors messaging_ambient_propagation_test's ambient-scope
// machinery: the principal rides the MessageContext down a causal chain.
//
// CONTROL (adversarial, must FIRE): a handler tries to forge a principal with a right its inbound
// context lacked (amplification). The stamp REFUSES it — the outbound principal never gains the right —
// and `is_amplification` flags the attempt so it can be audited.
#include <cstdio>

#include "quark/core/message_context.hpp"
#include "quark/core/security.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// Rights bits for the test.
constexpr std::uint64_t kRead = 1u << 0;
constexpr std::uint64_t kWrite = 1u << 1;
constexpr std::uint64_t kAdmin = 1u << 2;

// The runtime's outbound stamp: attenuate `requested` against the ambient inbound principal. This is
// what a secured send verb calls — it CANNOT return a right the ambient lacked.
Principal stamp_outbound(const Principal& requested) noexcept {
    const MessageContext* amb = detail::tl_current_ctx;
    const Principal inbound = amb ? amb->principal : Principal{};
    return attenuate(inbound, requested);
}
}  // namespace

int main() {
    bool ok = true;

    // Root principal: read+write, NOT admin.
    Principal root{/*subject*/ 100, /*rights*/ kRead | kWrite};

    // --- Attenuation HOLDS: a handler under `root` delegates a READ-ONLY principal downstream. -------
    {
        MessageContext ctx{};
        ctx.principal = root;
        detail::AmbientContextScope amb(ctx);

        // (a) inherit unchanged — same authority propagates.
        const Principal inherited = stamp_outbound(inherit(root));
        check(inherited.rights == (kRead | kWrite), "inherit propagates full inbound authority", ok);

        // (b) attenuate to read-only — a WEAKER principal is allowed downstream.
        const Principal readonly = stamp_outbound(Principal{200, kRead});
        check(readonly.rights == kRead, "attenuation to a weaker (read-only) principal holds", ok);
        check(dominates(root, readonly), "root dominates the attenuated principal", ok);
    }

    // --- CONTROL: AMPLIFICATION IS REFUSED. A handler under read-only `root` tries to forge admin. ---
    {
        Principal readonly_inbound{100, kRead};
        MessageContext ctx{};
        ctx.principal = readonly_inbound;
        detail::AmbientContextScope amb(ctx);

        const Principal forged_request{100, kRead | kWrite | kAdmin};  // asks for MORE than it holds
        check(is_amplification(readonly_inbound, forged_request),
              "CONTROL: the forged request IS detected as amplification", ok);

        const Principal stamped = stamp_outbound(forged_request);
        check((stamped.rights & kAdmin) == 0, "CONTROL: forged admin right REFUSED (never stamped)", ok);
        check((stamped.rights & kWrite) == 0, "CONTROL: forged write right REFUSED (never stamped)", ok);
        check(stamped.rights == kRead, "CONTROL: outbound clamped to the inbound authority (read-only)", ok);
        check(!dominates(stamped, forged_request), "CONTROL: stamped principal is strictly weaker", ok);
    }

    // --- Monotonic shrink down a MULTI-HOP chain: authority only ever narrows, never widens. --------
    {
        Principal hop0{1, kRead | kWrite | kAdmin};
        MessageContext c0{};
        c0.principal = hop0;
        detail::AmbientContextScope a0(c0);
        // hop1 delegates read+write (drops admin)
        Principal hop1 = stamp_outbound(Principal{1, kRead | kWrite});
        MessageContext c1{};
        c1.principal = hop1;
        detail::AmbientContextScope a1(c1);
        // hop2 TRIES to regain admin — refused; and further attenuates to read-only.
        Principal hop2_forge = stamp_outbound(Principal{1, kAdmin});
        check(hop2_forge.rights == 0, "multi-hop: regaining a dropped right is refused (empty)", ok);
        Principal hop2 = stamp_outbound(Principal{1, kRead});
        check(hop2.rights == kRead, "multi-hop: further attenuation to read-only holds", ok);
        check(dominates(hop0, hop1) && dominates(hop1, hop2), "multi-hop authority is monotone shrinking", ok);
    }

    std::printf("security_principal_attenuation_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
