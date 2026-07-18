// Implements 020-Security §3 (boundary authorization) — the `Authorizer` seam, checked BEFORE the
// handler (between deserialize and dispatch), plus the DISTINCT security dead-letter stream a denied
// message lands in. Because the check runs at the boundary, handler code carries no `if (allowed)`
// boilerplate, a denied message NEVER touches actor state, and the denial is audited uniformly
// (020 §3 self-debate: "Boundary").
//
//   Authorizer::allow(principal, ActorId, TypeKey) -> bool
//
// THE SECURITY DEAD-LETTER IS SEPARATE from 007's poison dead-letter (020 §Audit): an authz denial is
// not a crashed/undeliverable message, so conflating the two streams would hide security signal in
// operational noise. This header defines its own bounded ring (`SecurityDeadLetter`) keyed on
// {principal, target, msg_type, reason} — it needs no `Descriptor`, so it composes without the 007
// activation surface, keeping a security-only build lean.
//
// POLICY MODEL. The default is an ACL/policy seam (`AclAuthorizer` — required-rights per (actor type,
// message type); a principal is allowed iff it DOMINATES the required rights). Pluggable to RBAC /
// OPA-Rego / a capability system (020 §3): the capability direction (a ref IS the permission) is the
// stated long-term evolution and a DEFERRED seam.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "quark/core/audit.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/principal.hpp"

namespace quark {

// ============================================================================================
// The Authorizer seam (020 §3). `allow` is a pure predicate: may `who` send a message of type
// `msg_type` to actor `target`? Runs at the boundary before dispatch; noexcept (no throw on the
// admission path). const — an authorizer holds an immutable policy after setup.
// ============================================================================================
class Authorizer {
public:
    virtual ~Authorizer() = default;
    [[nodiscard]] virtual bool allow(const Principal& who, ActorId target,
                                     TypeKey msg_type) const noexcept = 0;
};

// Trivial authorizers for the boundary tests / dev default.
class AllowAllAuthorizer final : public Authorizer {
public:
    [[nodiscard]] bool allow(const Principal&, ActorId, TypeKey) const noexcept override { return true; }
};
class DenyAllAuthorizer final : public Authorizer {
public:
    [[nodiscard]] bool allow(const Principal&, ActorId, TypeKey) const noexcept override { return false; }
};

// The default ACL policy: a required-rights bitset per (actor type, message type). A principal is
// allowed iff it DOMINATES the required rights (holds every required bit). A (type, msg) pair with no
// rule requires nothing by default in `Open` mode (allow) or everything in `Closed` mode (deny) — the
// datacenter default is `Closed` (deny-by-default: an unlisted route is refused).
class AclAuthorizer final : public Authorizer {
public:
    enum class Default : std::uint8_t { Closed = 0, Open = 1 };

    explicit AclAuthorizer(Default def = Default::Closed) : default_(def) {}

    // Require `rights` for (actor type `type`, message type `msg`). Cold setup path.
    void require(TypeKey type, TypeKey msg, std::uint64_t rights) {
        rules_[key_of(type, msg)] = rights;
    }

    [[nodiscard]] bool allow(const Principal& who, ActorId target,
                             TypeKey msg_type) const noexcept override {
        const auto it = rules_.find(key_of(target.type, msg_type));
        if (it == rules_.end()) return default_ == Default::Open;
        const Principal required{0, it->second};
        return dominates(who, required);
    }

private:
    [[nodiscard]] static std::uint64_t key_of(TypeKey type, TypeKey msg) noexcept {
        return type.value ^ (msg.value * 0x9E3779B97F4A7C15ULL);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> rules_;
    Default default_;
};

// ============================================================================================
// The security dead-letter (020 §Audit) — DISTINCT from 007's poison stream. A bounded per-boundary
// ring of denied-message records; single-writer append, 0 allocation on the record path (writes in
// place into the pre-sized buffer). Mirrors the 009 DeadLetterRegistry bounded-shed discipline but
// keys on the security tuple, not a Descriptor.
// ============================================================================================
struct SecurityDeadLetterRecord {
    Principal principal{};   // the (attenuated) principal whose message was denied
    ActorId target{};        // the actor the denied message was bound for
    TypeKey msg_type{};      // the message type key
    errc reason = errc::validation;  // why (validation = authz denial; unavailable = authn/admission)
    std::int64_t t_ns = 0;
};

class SecurityDeadLetter {
public:
    explicit SecurityDeadLetter(std::size_t capacity = 256)
        : buf_(capacity == 0 ? 1 : capacity), cap_(capacity == 0 ? 1 : capacity) {}

    SecurityDeadLetter(const SecurityDeadLetter&) = delete;
    SecurityDeadLetter& operator=(const SecurityDeadLetter&) = delete;

    // Record a denial. Single-writer, 0 allocation.
    void record(const Principal& who, ActorId target, TypeKey msg_type, errc reason,
                std::int64_t now_ns = 0) noexcept {
        SecurityDeadLetterRecord r{who, target, msg_type, reason, now_ns};
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        buf_[static_cast<std::size_t>(h % cap_)] = r;
        head_.store(h + 1, std::memory_order_relaxed);  // single-writer, no atomic RMW
    }

    [[nodiscard]] std::uint64_t total() const noexcept { return head_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t t = total();
        return t < cap_ ? static_cast<std::size_t>(t) : cap_;
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        const std::uint64_t t = total();
        return t > cap_ ? t - cap_ : 0;
    }

    void snapshot(std::vector<SecurityDeadLetterRecord>& out) const {
        const std::uint64_t t = total();
        const std::size_t n = t < cap_ ? static_cast<std::size_t>(t) : cap_;
        out.clear();
        out.reserve(n);
        const std::uint64_t start = t < cap_ ? 0 : t - cap_;
        for (std::uint64_t i = start; i < t; ++i)
            out.push_back(buf_[static_cast<std::size_t>(i % cap_)]);
    }

private:
    std::vector<SecurityDeadLetterRecord> buf_;
    std::size_t cap_;
    std::atomic<std::uint64_t> head_{0};
};

// ============================================================================================
// The boundary interceptor (020 §3). Runs the Authorizer BEFORE dispatch. On DENY it (a) records to
// the security dead-letter, (b) emits an `AuthzDenial` audit record, and (c) returns false so the
// caller drops the message WITHOUT deserializing further into / dispatching to the actor — state is
// never touched. On ALLOW it returns true and the caller proceeds to dispatch. The audit/dead-letter
// pointers are optional (nullptr = no sink wired).
// ============================================================================================
[[nodiscard]] inline bool authorize_at_boundary(const Authorizer& authz, const Principal& who,
                                                ActorId target, TypeKey msg_type,
                                                SecurityDeadLetter* sec_dl, const AuditSink* audit,
                                                std::int64_t now_ns = 0) noexcept {
    if (authz.allow(who, target, msg_type)) return true;
    if (sec_dl != nullptr) sec_dl->record(who, target, msg_type, errc::validation, now_ns);
    if (audit != nullptr)
        (*audit)(AuditRecord{AuditKind::AuthzDenial, errc::validation, who.subject,
                             "authz denied at boundary", now_ns});
    return false;
}

}  // namespace quark
