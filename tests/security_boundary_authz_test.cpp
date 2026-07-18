// Tests 020-Security §3 — BOUNDARY authorization: the Authorizer check runs BEFORE dispatch, so a
// denied message NEVER touches actor state and lands in a DISTINCT security dead-letter stream (not
// 007's poison stream). A denial is also audited uniformly.
//
// CONTROL (adversarial, must FIRE): a DenyAllAuthorizer (and an ACL that requires a right the principal
// lacks) blocks the message — the mutation is proven NOT to happen, the security dead-letter records it,
// and the audit sink fires. The positive path (AllowAll / a principal that dominates the required
// rights) shows the SAME message mutates state and produces no dead-letter — isolating the guard.
#include <cstdio>
#include <vector>

#include "quark/core/authorizer.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metadata.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

// A stand-in actor whose state must NEVER change on a denied message.
struct AccountState {
    int balance = 0;
};

// The boundary: authorize, then (only if allowed) dispatch to the handler that mutates state. This is
// the interceptor shape the runtime installs between deserialize and dispatch (020 §3).
bool deliver_at_boundary(const Authorizer& authz, const Principal& who, ActorId target, TypeKey msg,
                         SecurityDeadLetter& sec_dl, const AuditSink& audit, AccountState& state) {
    if (!authorize_at_boundary(authz, who, target, msg, &sec_dl, &audit, 1234)) {
        return false;  // denied — handler NOT invoked, state untouched
    }
    state.balance += 100;  // the "handler" mutation — reachable ONLY past the boundary check
    return true;
}

// A counting audit sink.
int g_audits = 0;
void count_audit(const AuditRecord& r, void* ctx) noexcept {
    (void)r;
    ++*static_cast<int*>(ctx);
}
}  // namespace

int main() {
    bool ok = true;

    const ActorId target{TypeKey{0xACC0}, 42};
    const TypeKey withdraw{0xDEAD};
    const Principal user{7, 1u << 0};  // holds bit 0 only

    // --- Positive path: AllowAll ⇒ the message mutates state, no dead-letter. -----------------------
    {
        g_audits = 0;
        AllowAllAuthorizer authz;
        SecurityDeadLetter sec_dl(16);
        AuditSink audit{&count_audit, &g_audits};
        AccountState state{};
        const bool delivered = deliver_at_boundary(authz, user, target, withdraw, sec_dl, audit, state);
        check(delivered, "allow: message delivered", ok);
        check(state.balance == 100, "allow: state mutated (handler ran)", ok);
        check(sec_dl.total() == 0, "allow: no security dead-letter", ok);
        check(g_audits == 0, "allow: no audit denial", ok);
    }

    // --- CONTROL: DenyAll ⇒ state is NEVER touched; the message lands in the security dead-letter. ---
    {
        g_audits = 0;
        DenyAllAuthorizer authz;
        SecurityDeadLetter sec_dl(16);
        AuditSink audit{&count_audit, &g_audits};
        AccountState state{};
        const bool delivered = deliver_at_boundary(authz, user, target, withdraw, sec_dl, audit, state);
        check(!delivered, "CONTROL: deny blocks delivery", ok);
        check(state.balance == 0, "CONTROL: denied message NEVER mutated actor state", ok);
        check(sec_dl.total() == 1, "CONTROL: denial recorded in the security dead-letter", ok);
        check(g_audits == 1, "CONTROL: denial audited", ok);

        std::vector<SecurityDeadLetterRecord> snap;
        sec_dl.snapshot(snap);
        check(snap.size() == 1 && snap[0].target == target && snap[0].msg_type == withdraw,
              "CONTROL: dead-letter record carries the denied {target,msg}", ok);
        check(snap[0].reason == errc::validation, "CONTROL: reason is an authz denial (validation)", ok);
    }

    // --- CONTROL 2: an ACL that REQUIRES a right the principal lacks ⇒ denied; a principal that has it
    //     ⇒ allowed. Proves the ACL policy, not just the trivial authorizers, gates at the boundary. --
    {
        AclAuthorizer acl(AclAuthorizer::Default::Closed);
        acl.require(target.type, withdraw, /*rights*/ 1u << 3);  // needs bit 3

        SecurityDeadLetter sec_dl(16);
        AuditSink audit{&count_audit, &g_audits};

        AccountState s_denied{};
        const bool d1 = deliver_at_boundary(acl, user, target, withdraw, sec_dl, audit, s_denied);
        check(!d1 && s_denied.balance == 0, "CONTROL: ACL denies a principal missing the required right", ok);

        AccountState s_ok{};
        const Principal privileged{9, (1u << 0) | (1u << 3)};  // holds the required bit
        const bool d2 = deliver_at_boundary(acl, privileged, target, withdraw, sec_dl, audit, s_ok);
        check(d2 && s_ok.balance == 100, "ACL: a principal holding the required right is allowed", ok);
    }

    std::printf("security_boundary_authz_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
