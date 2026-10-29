// Implements ADR-067 D4 — ticketed enrolment, owned by Quark: the transport-agnostic `Enrollment` state
// machine on the trust anchor (ticket table, the fixed check order, single-use consumption, attempt and
// per-source limits, the asynchronous `IssueFn` hand-off to the application's key custodian, verification
// of what the custodian returns, and an audit record per attempt). 021 §Enrolment. Not on any hot path.
//
// QUARK NEVER SIGNS (ADR-067 D1). The CA private key stays in the application's custodian; Quark only
// decides WHETHER a request may be issued and checks the certificate that comes back. Crypto is behind
// the verify-only `EnrolCrypto` seam (HMAC-SHA256 compare, CSR proof-of-possession, issued-certificate
// checks) — the std-only core has no SHA-256; the mbedTLS adapter implements it.
//
// CHECK ORDER (fixed, ADR-067 D4): source under its rate limit → ticket exists → unexpired → not consumed
// → under its attempt limit → issuance capacity → constant-time HMAC → CSR proof-of-possession → CONSUME
// (single use, atomic) → IssueFn. Every presentation reserves one attempt BEFORE any crypto, so
// concurrent guesses cannot overshoot the limit; a wrong HMAC or a bad CSR leaves the ticket unconsumed
// (it survives until its attempts run out), and the HMAC is checked before the CSR so a party without the
// secret can never make the anchor do signature work. Two presentations of one ticket racing past the
// HMAC are settled by the consume step: exactly one issuance. A `Busy` refusal refunds the attempt.
//
// THE ANCHOR NEVER HONOURS A REQUESTED NAME. The CSR contributes only its public key (and the proof that
// the joiner holds the private key); the custodian is never shown the CSR. The application returns the
// NodeId it assigns in `Issued::node`; the role and cluster come from the ticket. The returned certificate
// must have exactly the canonical CN for that (cluster, NodeId), whose client bit must match the ticket's
// role, the CSR's public key, CA:FALSE, no keyCertSign/cRLSign, a bounded validity, and chain to a trusted
// root. Anything else is refused (`IssuedCertInvalid`) and the ticket stays consumed (fail closed).
//
// THREADING. All methods are thread-safe (one mutex). Crypto and every callback (IssueFn, the reply, the
// audit sink) run OUTSIDE the lock. A completion may run on any thread, at any time, at most once; one
// that arrives after the request timed out (`expire`) or after `Enrollment` is destroyed is ignored.
// Every presentation gets exactly one reply — destroying `Enrollment` replies `Shutdown` to the
// issuances still in flight. Clock readings from different threads may arrive out of order: interval
// arithmetic saturates at zero rather than wrapping. Tickets live in memory only: an anchor restart loses
// them (fail closed; re-issue the join code). Ticket secrets are wiped when a ticket is dropped.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pal/csprng.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/secret.hpp"  // detail::secure_zero
#include "quark/core/tls_identity.hpp"

namespace quark::enrol {

using Ticks = std::uint64_t;  // the anchor's own monotonic clock, nanoseconds (only intervals compared)

enum class Role : std::uint8_t { Member, Client };

using TicketId = std::array<std::byte, 16>;
using TicketSecret = std::array<std::byte, 32>;  // 256 bits from the PAL CSPRNG (ADR-067: >= 128)
using MacTag = std::array<std::byte, 32>;        // HMAC-SHA256

// What the operator hands a joiner out of band, together with the anchor's endpoint, NodeId and CA root
// hash (the join-code encoding is the transport layer's).
struct Ticket {
    TicketId id{};
    TicketSecret secret{};
    Role role = Role::Member;
    Ticks expires_at = 0;
};

// The HMAC input binds the ticket id and the cluster to the CSR under a domain prefix, so a tag computed
// for one ticket/cluster (or for any other protocol using the same secret) never verifies for another.
inline constexpr std::string_view kEnrolMacDomain{"QUARK-ENROL-v1\0", 16};

[[nodiscard]] inline std::vector<std::byte> enrol_mac_message(ClusterId cluster, const TicketId& id,
                                                              std::span<const std::byte> csr_der) {
    std::vector<std::byte> m;
    m.reserve(kEnrolMacDomain.size() + id.size() + 8 + csr_der.size());
    for (char c : kEnrolMacDomain) m.push_back(static_cast<std::byte>(c));
    m.insert(m.end(), id.begin(), id.end());
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::byte>((cluster.value >> (8 * i)) & 0xFF));
    m.insert(m.end(), csr_der.begin(), csr_der.end());
    return m;
}

// What the returned certificate must be (see the banner). `now_unix_s` is the anchor's wall clock.
struct IssuedExpectation {
    std::string cn;
    std::span<const std::byte> spki_der;
    std::uint64_t now_unix_s = 0;
    std::uint64_t max_validity_s = 0;
};

// Verify-only crypto seam (mbedTLS adapter). Implementations must be thread-safe.
class EnrolCrypto {
public:
    virtual ~EnrolCrypto() = default;
    // Constant-time: HMAC-SHA256(secret, message) == tag.
    [[nodiscard]] virtual bool hmac_equal(std::span<const std::byte> secret, std::span<const std::byte> message,
                                          std::span<const std::byte> tag) const = 0;
    // Parse a PKCS#10 CSR and verify its self-signature (proof the joiner holds the key). On success
    // `spki_der` is the DER SubjectPublicKeyInfo. Every other CSR field is ignored.
    [[nodiscard]] virtual bool csr_public_key(std::span<const std::byte> csr_der,
                                              std::vector<std::byte>& spki_der) const = 0;
    // The LEAF (first certificate of `chain_der`, concatenated DER) meets `expect`: exactly one CN, equal
    // to `expect.cn` byte for byte (a SAN, if any, is not an identity and is ignored); subject public key
    // equal to `expect.spki_der`; basicConstraints CA:FALSE or absent; no keyCertSign / cRLSign;
    // notBefore <= now + skew and notAfter <= now + max_validity_s; and it chains to the anchor's CONFIGURED
    // trusted roots (never to a root carried inside `chain_der`).
    [[nodiscard]] virtual bool issued_ok(const IssuedExpectation& expect, std::span<const std::byte> chain_der) const = 0;
};

// One presentation, as received from a joiner (inside server-authenticated TLS; the transport fills it).
// `source` keys the per-source rate limit: the transport should use the address prefix an attacker cannot
// cheaply multiply (e.g. the IPv6 /64), not the full address. Only its first kMaxSourceKey bytes are used.
struct EnrolRequest {
    TicketId ticket_id{};
    std::vector<std::byte> csr_der;
    MacTag tag{};
    std::string source;  // rate limiting and audit only; never trusted for identity
};
inline constexpr std::size_t kMaxSourceKey = 64;

enum class Status : std::uint8_t {
    Issued,             // the reply carries the certificate chain
    RateLimited,        // this source made too many attempts in the window
    UnknownTicket,      // no such ticket, or it expired / was revoked
    TicketUsed,         // already consumed (single use)
    AttemptsExhausted,  // the ticket's attempt limit is spent
    BadMac,             // the tag does not verify (the ticket survives)
    BadCsr,             // the CSR does not parse or its signature does not verify (the ticket survives)
    Busy,               // too many issuances in flight (the attempt is refunded)
    IssuerFailed,       // the custodian/application refused or failed (the ticket stays consumed)
    IssuedCertInvalid,  // the custodian returned a certificate that is not exactly what the ticket allows
    TimedOut,           // the custodian did not complete within issue_timeout
    Shutdown,           // Enrollment was destroyed while the issuance was in flight
};

// What a joiner may learn. The detailed Status is for the anchor's audit trail; on the wire the ticket
// refusals collapse into one code (so a prober learns nothing about which ticket ids exist), and the
// issuance failures into another. BadCsr is reachable only with a valid MAC, so only the real joiner sees it.
enum class WireStatus : std::uint8_t { Issued, Refused, RetryLater, BadCsr, IssuanceFailed };

[[nodiscard]] constexpr WireStatus wire_status(Status s) noexcept {
    switch (s) {
        case Status::Issued: return WireStatus::Issued;
        case Status::RateLimited:
        case Status::Busy: return WireStatus::RetryLater;
        case Status::BadCsr: return WireStatus::BadCsr;
        case Status::IssuerFailed:
        case Status::IssuedCertInvalid:
        case Status::TimedOut:
        case Status::Shutdown: return WireStatus::IssuanceFailed;
        case Status::UnknownTicket:
        case Status::TicketUsed:
        case Status::AttemptsExhausted:
        case Status::BadMac: break;
    }
    return WireStatus::Refused;
}

struct Reply {
    Status status = Status::UnknownTicket;
    std::vector<std::byte> cert_chain_der;  // Issued only
};

struct AuditRecord {
    TicketId ticket_id{};
    std::string source;
    std::optional<Role> role;          // known once the ticket was found
    std::optional<NodeId> assigned;    // known once the application completed
    Status status = Status::UnknownTicket;
};

// What the application's custodian is asked to sign: a certificate for `spki_der` in `cluster`, of
// `role`. The application picks the NodeId (it owns the roster) and returns it in `Issued::node`; its
// client bit must match `role`. The CSR itself is deliberately not passed on (its requested name is never
// honoured, so the custodian has no use for it).
struct IssueRequest {
    std::uint64_t request_id = 0;
    TicketId ticket_id{};
    ClusterId cluster{};
    Role role = Role::Member;
    std::vector<std::byte> spki_der;
    std::string source;
};

struct Issued {
    NodeId node{};
    std::vector<std::byte> cert_chain_der;
};

struct Config {
    ClusterId cluster{};
    std::size_t max_tickets = 1024;
    Ticks max_ticket_ttl = 24ULL * 3600 * 1'000'000'000;  // tickets are short-lived by design
    std::uint32_t max_attempts_per_ticket = 5;
    std::uint32_t max_attempts_per_source = 20;           // per source_window
    Ticks source_window = 60ULL * 1'000'000'000;
    // Rate-limit table bound. When it is full of live windows, NEW sources are refused (RateLimited) until
    // windows age out: an attacker controlling more source keys than this can delay enrolment (never
    // forge it) — the transport's source-key choice (see EnrolRequest) bounds that.
    std::size_t max_sources = 4096;
    std::size_t max_pending_issues = 64;
    Ticks issue_timeout = 60ULL * 1'000'000'000;
    std::uint64_t max_validity_s = 365ULL * 24 * 3600;     // longest certificate Quark will hand out
};

class Enrollment {
    struct Shared;

public:
    using ReplyFn = std::function<void(Reply)>;
    using AuditFn = std::function<void(const AuditRecord&)>;
    using UnixClockFn = std::function<std::uint64_t()>;

    // Completes one IssueRequest, at most once, from any thread. Destroying it without calling it is
    // equivalent to never completing (the request times out in `expire`).
    class Completion {
    public:
        void operator()(result<Issued> outcome) {
            if (auto s = shared_.lock()) s->complete(request_id_, std::move(outcome));
            shared_.reset();
        }

    private:
        friend class Enrollment;
        friend struct Enrollment::Shared;
        Completion(std::weak_ptr<Shared> s, std::uint64_t id) : shared_(std::move(s)), request_id_(id) {}
        std::weak_ptr<Shared> shared_;
        std::uint64_t request_id_;
    };

    using IssueFn = std::function<void(IssueRequest, Completion)>;

    Enrollment(Config cfg, std::shared_ptr<const EnrolCrypto> crypto, IssueFn issue, AuditFn audit = {},
               UnixClockFn unix_now = &system_unix_now)
        : shared_(std::make_shared<Shared>(std::move(cfg), std::move(crypto), std::move(issue), std::move(audit),
                                           std::move(unix_now))) {}
    // Replies `Shutdown` to every issuance still in flight (exactly-once replies survive destruction).
    ~Enrollment() { shared_->shutdown(); }
    Enrollment(const Enrollment&) = delete;
    Enrollment& operator=(const Enrollment&) = delete;

    // Create a single-use ticket for `role`, valid for `ttl` from `now`. errc::validation for a zero or
    // over-long ttl, errc::overloaded when the table is full of live tickets, errc::unavailable if the
    // OS CSPRNG fails.
    [[nodiscard]] result<Ticket> issue_ticket(Role role, Ticks ttl, Ticks now) { return shared_->issue_ticket(role, ttl, now); }

    // Withdraw a ticket that has not been used. Returns false for an unknown ticket and for one already
    // consumed — its certificate is issued (or being issued); revoke that identity instead (ADR-067 D2).
    bool revoke_ticket(const TicketId& id) { return shared_->revoke_ticket(id); }

    // Whether any unexpired, unconsumed ticket exists — the transport refuses an enrol hello outright
    // otherwise (no TLS work for anyone while nothing can be enrolled).
    [[nodiscard]] bool has_live_ticket(Ticks now) const { return shared_->has_live_ticket(now); }

    // Process one presentation. `reply` is called exactly once: synchronously for a refusal, or when
    // the application completes (or the request times out, or Enrollment is destroyed). Never under the lock.
    void submit(EnrolRequest req, ReplyFn reply, Ticks now) { shared_->submit(shared_, std::move(req), std::move(reply), now); }

    // Drop expired tickets and stale rate-limit windows; time out issuances older than issue_timeout
    // (their replies fire with TimedOut). Call periodically. Returns the number of timed-out issuances.
    std::size_t expire(Ticks now) { return shared_->expire(now); }

    [[nodiscard]] std::size_t live_tickets(Ticks now) const { return shared_->live_tickets(now); }
    [[nodiscard]] std::size_t pending_issues() const { return shared_->pending_issues(); }

    static std::uint64_t system_unix_now() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

private:
    // Interval since `start`, saturating at zero when a reading taken earlier on another thread arrives
    // after a later one.
    [[nodiscard]] static constexpr Ticks since(Ticks now, Ticks start) noexcept { return now > start ? now - start : 0; }

    struct TicketState {
        TicketSecret secret{};
        Role role = Role::Member;
        Ticks expires_at = 0;
        std::uint32_t attempts = 0;
        bool consumed = false;
        TicketState(const TicketSecret& s, Role r, Ticks e) : secret(s), role(r), expires_at(e) {}
        TicketState(const TicketState&) = delete;
        TicketState& operator=(const TicketState&) = delete;
        ~TicketState() { detail::secure_zero(secret.data(), secret.size()); }
    };
    struct SourceWindow {
        Ticks window_start = 0;
        std::uint32_t attempts = 0;
    };
    struct Pending {
        ReplyFn reply;
        TicketId ticket_id{};
        Role role = Role::Member;
        std::vector<std::byte> spki_der;
        std::string source;
        Ticks started = 0;
    };

    struct Shared {
        Shared(Config c, std::shared_ptr<const EnrolCrypto> cr, IssueFn i, AuditFn a, UnixClockFn u)
            : cfg(std::move(c)), crypto(std::move(cr)), issue(std::move(i)), audit(std::move(a)), unix_now(std::move(u)) {}

        result<Ticket> issue_ticket(Role role, Ticks ttl, Ticks now) {
            if (ttl == 0 || ttl > cfg.max_ticket_ttl) return fail(errc::validation, "ticket ttl out of range");
            Ticket t;
            t.role = role;
            t.expires_at = now + ttl;
            if (!pal::fill_random(std::as_writable_bytes(std::span{t.id})) ||
                !pal::fill_random(std::as_writable_bytes(std::span{t.secret})))
                return fail(errc::unavailable, "CSPRNG failure");
            std::lock_guard<std::mutex> g(mu);
            prune_tickets_locked(now);
            if (tickets.size() >= cfg.max_tickets) return fail(errc::overloaded, "ticket table is full");
            if (!tickets.try_emplace(t.id, t.secret, role, t.expires_at).second)
                return fail(errc::internal, "ticket id collision");
            return t;
        }

        bool revoke_ticket(const TicketId& id) {
            std::lock_guard<std::mutex> g(mu);
            const auto it = tickets.find(id);
            if (it == tickets.end() || it->second.consumed) return false;
            tickets.erase(it);
            return true;
        }

        bool has_live_ticket(Ticks now) const {
            std::lock_guard<std::mutex> g(mu);
            return std::any_of(tickets.begin(), tickets.end(), [&](const auto& kv) { return usable(kv.second, now); });
        }

        std::size_t live_tickets(Ticks now) const {
            std::lock_guard<std::mutex> g(mu);
            return static_cast<std::size_t>(
                std::count_if(tickets.begin(), tickets.end(), [&](const auto& kv) { return usable(kv.second, now); }));
        }

        std::size_t pending_issues() const {
            std::lock_guard<std::mutex> g(mu);
            return pending.size();
        }

        void submit(const std::shared_ptr<Shared>& self, EnrolRequest req, ReplyFn reply, Ticks now) {
            if (req.source.size() > kMaxSourceKey) req.source.resize(kMaxSourceKey);
            AuditRecord rec{req.ticket_id, req.source, std::nullopt, std::nullopt, Status::UnknownTicket};
            TicketSecret secret{};
            std::optional<Status> refused;
            // 1. Under the lock: rate limit, ticket checks, capacity, and reserve one attempt (no crypto yet).
            {
                std::lock_guard<std::mutex> g(mu);
                refused = admit_source_locked(req.source, now);
                if (!refused) refused = reserve_attempt_locked(req.ticket_id, now, rec, secret);
            }
            if (refused) return refuse(rec, *refused, reply);  // callbacks never run under the lock
            // 2. Outside the lock, in the fixed order: HMAC (cheap, gates everything), then the CSR.
            const auto msg = enrol_mac_message(cfg.cluster, req.ticket_id, req.csr_der);
            const bool mac_ok = crypto->hmac_equal(secret, msg, req.tag);
            detail::secure_zero(secret.data(), secret.size());
            if (!mac_ok) return refuse(rec, Status::BadMac, reply);
            std::vector<std::byte> spki;
            if (!crypto->csr_public_key(req.csr_der, spki)) return refuse(rec, Status::BadCsr, reply);
            // 3. Consume (single use) and register the pending issuance.
            std::uint64_t id = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                const auto it = tickets.find(req.ticket_id);
                if (it == tickets.end() || it->second.expires_at <= now) {
                    refused = Status::UnknownTicket;
                } else if (it->second.consumed) {
                    refused = Status::TicketUsed;
                } else if (pending.size() >= cfg.max_pending_issues) {
                    refused = Status::Busy;
                    --it->second.attempts;  // capacity is the anchor's problem, not the joiner's
                } else {
                    it->second.consumed = true;
                    id = ++next_request_id;
                    pending.emplace(id, Pending{std::move(reply), req.ticket_id, *rec.role, spki, req.source, now});
                }
            }
            if (refused) return refuse(rec, *refused, reply);
            // 4. Hand off to the application. It completes later, on any thread.
            issue(IssueRequest{id, req.ticket_id, cfg.cluster, *rec.role, std::move(spki), std::move(req.source)},
                  Completion{self, id});
        }

        void complete(std::uint64_t id, result<Issued> outcome) {
            Pending p;
            {
                std::lock_guard<std::mutex> g(mu);
                const auto it = pending.find(id);
                if (it == pending.end()) return;  // already completed, timed out, or shut down
                p = std::move(it->second);
                pending.erase(it);
            }
            AuditRecord rec{p.ticket_id, p.source, p.role, std::nullopt, Status::IssuerFailed};
            if (!outcome) return finish(rec, Reply{Status::IssuerFailed, {}}, p.reply);
            rec.assigned = outcome->node;
            const bool role_ok = is_client_node(outcome->node) == (p.role == Role::Client);
            const IssuedExpectation expect{format_quark_cn(cfg.cluster, outcome->node), p.spki_der, unix_now(),
                                           cfg.max_validity_s};
            if (!role_ok || !crypto->issued_ok(expect, outcome->cert_chain_der)) {
                rec.status = Status::IssuedCertInvalid;
                return finish(rec, Reply{Status::IssuedCertInvalid, {}}, p.reply);
            }
            rec.status = Status::Issued;
            finish(rec, Reply{Status::Issued, std::move(outcome->cert_chain_der)}, p.reply);
        }

        std::size_t expire(Ticks now) {
            std::vector<Pending> timed_out;
            {
                std::lock_guard<std::mutex> g(mu);
                prune_tickets_locked(now);
                std::erase_if(sources, [&](const auto& kv) { return since(now, kv.second.window_start) >= cfg.source_window; });
                for (auto it = pending.begin(); it != pending.end();) {
                    if (since(now, it->second.started) >= cfg.issue_timeout) {
                        timed_out.push_back(std::move(it->second));
                        it = pending.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (Pending& p : timed_out) {
                AuditRecord rec{p.ticket_id, p.source, p.role, std::nullopt, Status::TimedOut};
                finish(rec, Reply{Status::TimedOut, {}}, p.reply);
            }
            return timed_out.size();
        }

        void shutdown() {
            std::map<std::uint64_t, Pending> in_flight;
            {
                std::lock_guard<std::mutex> g(mu);
                in_flight.swap(pending);
            }
            for (auto& [id, p] : in_flight) {
                AuditRecord rec{p.ticket_id, p.source, p.role, std::nullopt, Status::Shutdown};
                finish(rec, Reply{Status::Shutdown, {}}, p.reply);
            }
        }

        [[nodiscard]] static bool usable(const TicketState& t, Ticks now) { return !t.consumed && t.expires_at > now; }

        // Expired tickets go; consumed ones too once their issuance is no longer in flight (a pruned id is
        // then refused as UnknownTicket — still a refusal, and the same on the wire).
        void prune_tickets_locked(Ticks now) {
            std::erase_if(tickets, [&](const auto& kv) {
                return kv.second.expires_at <= now || (kv.second.consumed && !pending_for(kv.first));
            });
        }
        [[nodiscard]] bool pending_for(const TicketId& id) const {
            return std::any_of(pending.begin(), pending.end(), [&](const auto& kv) { return kv.second.ticket_id == id; });
        }

        // The ticket checks of step 1; on success one attempt is reserved and the secret copied out.
        std::optional<Status> reserve_attempt_locked(const TicketId& id, Ticks now, AuditRecord& rec, TicketSecret& secret) {
            const auto it = tickets.find(id);
            if (it == tickets.end() || it->second.expires_at <= now) return Status::UnknownTicket;
            TicketState& t = it->second;
            rec.role = t.role;
            if (t.consumed) return Status::TicketUsed;
            if (t.attempts >= cfg.max_attempts_per_ticket) return Status::AttemptsExhausted;
            if (pending.size() >= cfg.max_pending_issues) return Status::Busy;  // no attempt, no crypto
            ++t.attempts;
            secret = t.secret;
            return std::nullopt;
        }

        // Count one attempt for `source` in its current window; a refusal status if over the limit or the
        // table is full of live windows.
        std::optional<Status> admit_source_locked(const std::string& source, Ticks now) {
            auto it = sources.find(source);
            if (it == sources.end()) {
                if (sources.size() >= cfg.max_sources) {
                    std::erase_if(sources, [&](const auto& kv) { return since(now, kv.second.window_start) >= cfg.source_window; });
                    if (sources.size() >= cfg.max_sources) return Status::RateLimited;
                }
                it = sources.emplace(source, SourceWindow{now, 0}).first;
            } else if (since(now, it->second.window_start) >= cfg.source_window) {
                it->second = SourceWindow{now, 0};
            }
            if (it->second.attempts >= cfg.max_attempts_per_source) return Status::RateLimited;
            ++it->second.attempts;
            return std::nullopt;
        }

        void refuse(AuditRecord& rec, Status s, ReplyFn& reply) {
            rec.status = s;
            finish(rec, Reply{s, {}}, reply);
        }
        void finish(const AuditRecord& rec, Reply r, ReplyFn& reply) {
            if (audit) audit(rec);
            if (reply) reply(std::move(r));
        }

        const Config cfg;
        const std::shared_ptr<const EnrolCrypto> crypto;
        const IssueFn issue;
        const AuditFn audit;
        const UnixClockFn unix_now;
        mutable std::mutex mu;
        std::map<TicketId, TicketState> tickets;
        std::unordered_map<std::string, SourceWindow> sources;
        std::map<std::uint64_t, Pending> pending;
        std::uint64_t next_request_id = 0;
    };

    std::shared_ptr<Shared> shared_;
};

}  // namespace quark::enrol
