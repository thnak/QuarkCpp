// Implements 009-Observability §Audit + 020-Security §Audit — the `AuditSink` seam, a SIBLING of 009's
// `MetricsSink`/`TraceSink`/`DeadLetterSink`. Security-relevant events (authn failures, authz denials,
// cluster admission/rejection, secret-resolution failures, amplification-refusals) flow here as
// structured, CATEGORICAL records. This is a MINIMAL header (only error.hpp) so the lean security seams
// (authorizer.hpp, node_authority.hpp) compose it without pulling the 009 metrics/dead-letter surface;
// observability.hpp re-exports it so `#include "observability.hpp"` still yields the AuditSink.
//
// SECRETS NEVER APPEAR IN A SINK (020 §4): a record carries only categorical fields (kind, code, a
// subject id) + a BORROWED STATIC detail string — never secret bytes, never a formatted key.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "quark/core/error.hpp"

namespace quark {

enum class AuditKind : std::uint8_t {
    AuthnFailure = 0,             // authentication failure (020)
    AuthzDenial = 1,             // authorization denial (020) — also a security dead-letter
    ClusterAdmission = 2,        // cluster admission outcome (021)
    SecretResolutionFailure = 3, // a named secret could not be resolved (020 §4)
    AmplificationRefused = 4,    // a handler tried to forge a STRONGER principal; clamped (020 §3)
    PlacementRejected = 5,       // an unadmitted node was excluded from placement (020 §1)
    Other = 6,
};

struct AuditRecord {
    AuditKind kind = AuditKind::Other;
    errc code = errc::ok;
    std::uint64_t subject = 0;   // principal / node id (NEVER a secret — 020)
    std::string_view detail{};   // borrowed static string; NEVER a secret
    std::int64_t t_ns = 0;
};

// The seam: a noexcept function-pointer sink (matches the 009 sink shape — no virtual, no allocation).
struct AuditSink {
    void (*fn)(const AuditRecord&, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
    void operator()(const AuditRecord& r) const noexcept {
        if (fn) fn(r, ctx);
    }
};

// The dependency-free default AuditSink: a structured line to stderr (009 §Audit: "structured records
// to stderr/file; SIEM/OTLP export is an adapter"). Cold path only.
inline void audit_to_stderr(const AuditRecord& r, void* /*ctx*/) noexcept {
    std::fprintf(stderr, "audit kind=%u code=%u subject=%llu detail=%.*s t=%lld\n",
                 static_cast<unsigned>(r.kind), static_cast<unsigned>(r.code),
                 static_cast<unsigned long long>(r.subject),
                 static_cast<int>(r.detail.size()), r.detail.data(),
                 static_cast<long long>(r.t_ns));
}

[[nodiscard]] inline AuditSink default_audit_sink() noexcept {
    return AuditSink{&audit_to_stderr, nullptr};
}

}  // namespace quark
