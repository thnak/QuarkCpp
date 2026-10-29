// Tests ADR-067 D4 — the `Enrollment` state machine (enrollment.hpp), std-only. A MockCrypto stands in
// for the verify-only seam (keyed toy MAC, toy CSR and certificate encodings); real HMAC/X.509 checks
// belong to the mbedTLS adapter.
//
// Load-bearing properties (each with a positive control):
//   C4  a ticket authorises at most one certificate, of exactly its role and cluster: single use (also
//       when presentations race past the MAC), unknown/expired/revoked tickets refused, the MAC binds
//       ticket and cluster, the application's NodeId must carry the ticket's role bit, and a returned
//       certificate that is not exactly what the ticket allows is refused (the ticket stays consumed);
//   order a wrong MAC or a bad CSR does not consume the ticket; the MAC gates all CSR work; a used ticket
//       costs no crypto; attempts are reserved before crypto, so racing guesses cannot overshoot;
//   bounds ticket TTL / table size, per-source rate limit (and its table bound), in-flight issuance cap
//       (refused before any crypto, attempt refunded), issuance timeout, out-of-order clock readings;
//   replies exactly once per presentation (also on timeout and destruction); completions at most once;
//       callbacks never run under the lock; one audit record per attempt; the wire collapses refusals.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "quark/core/enrollment.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

using namespace quark;
using namespace quark::enrol;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

constexpr ClusterId kCluster{77};
constexpr Ticks kSec = 1'000'000'000;
constexpr std::uint64_t kUnixNow = 1'800'000'000;

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v;
    for (char c : s) v.push_back(static_cast<std::byte>(c));
    return v;
}
std::string string_of(std::span<const std::byte> b) {
    std::string s;
    for (std::byte x : b) s.push_back(static_cast<char>(x));
    return s;
}

// Toy MAC: FNV-1a over (secret, message), spread into 32 bytes. Stands in for HMAC-SHA256.
MacTag toy_mac(std::span<const std::byte> secret, std::span<const std::byte> msg) {
    std::uint64_t h = 1469598103934665603ULL;
    auto fold = [&](std::span<const std::byte> s) {
        for (std::byte b : s) {
            h ^= static_cast<unsigned char>(b);
            h *= 1099511628211ULL;
        }
    };
    fold(secret);
    fold(msg);
    MacTag t{};
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = static_cast<std::byte>((h >> (8 * (i % 8))) ^ i);
    return t;
}

// Toy CSR: "CSR|<spki>|<requested name>|OK" — valid (proof of possession) iff it ends in "|OK".
std::vector<std::byte> toy_csr(std::string_view spki, std::string_view requested_name = "quark:77:1", bool pop_ok = true) {
    return bytes_of("CSR|" + std::string(spki) + "|" + std::string(requested_name) + (pop_ok ? "|OK" : "|XX"));
}
// Toy certificate: "CERT|<cn>|<spki>|<ca 0/1>|<validity seconds>".
std::vector<std::byte> toy_cert(std::string_view cn, std::string_view spki, bool ca = false, std::uint64_t validity = 86400) {
    return bytes_of("CERT|" + std::string(cn) + "|" + std::string(spki) + "|" + (ca ? "1" : "0") + "|" + std::to_string(validity));
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '|') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

// Wait (bounded) until `need` threads have arrived — used inside the mock MAC check to hold every racing
// presentation past phase 1 before any reaches the consume step.
struct Gate {
    std::atomic<int> arrived{0};
    int need = 0;
    std::chrono::milliseconds patience{2000};  // how long an arrival waits for the rest
    void arrive_and_wait() {
        arrived.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (arrived.load() < need && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    }
};

class MockCrypto final : public EnrolCrypto {
public:
    mutable std::atomic<int> mac_calls{0}, csr_calls{0}, issued_calls{0};
    std::function<void()> on_mac;  // test hook, runs inside hmac_equal (outside Enrollment's lock)
    bool hmac_equal(std::span<const std::byte> secret, std::span<const std::byte> message,
                    std::span<const std::byte> tag) const override {
        mac_calls.fetch_add(1);
        if (on_mac) on_mac();
        const MacTag want = toy_mac(secret, message);
        return tag.size() == want.size() && std::equal(tag.begin(), tag.end(), want.begin());
    }
    bool csr_public_key(std::span<const std::byte> csr_der, std::vector<std::byte>& spki_der) const override {
        csr_calls.fetch_add(1);
        const auto f = split(string_of(csr_der));
        if (f.size() != 4 || f[0] != "CSR" || f[3] != "OK") return false;
        spki_der = bytes_of(f[1]);
        return true;
    }
    bool issued_ok(const IssuedExpectation& e, std::span<const std::byte> chain_der) const override {
        issued_calls.fetch_add(1);
        const auto f = split(string_of(chain_der));
        return f.size() == 5 && f[0] == "CERT" && f[1] == e.cn && f[2] == string_of(e.spki_der) && f[3] == "0" &&
               std::stoull(f[4]) <= e.max_validity_s && e.now_unix_s == kUnixNow;
    }
};

EnrolRequest request_for(const Ticket& t, std::string_view spki, std::string source = "10.0.0.9",
                         ClusterId cluster = kCluster) {
    EnrolRequest r;
    r.ticket_id = t.id;
    r.csr_der = toy_csr(spki);
    r.tag = toy_mac(t.secret, enrol_mac_message(cluster, t.id, r.csr_der));
    r.source = std::move(source);
    return r;
}
EnrolRequest bad_mac_for(const Ticket& t, std::string source = "10.0.0.9") {
    EnrolRequest r = request_for(t, "K", std::move(source));
    r.tag[0] ^= std::byte{1};
    return r;
}

// Captures replies and the application's view of issue requests.
struct Harness {
    std::shared_ptr<MockCrypto> crypto = std::make_shared<MockCrypto>();
    std::mutex mu;
    std::vector<IssueRequest> requests;
    std::vector<Enrollment::Completion> completions;
    std::vector<AuditRecord> audits;
    bool auto_complete = true;  // complete synchronously inside IssueFn
    NodeId next_node{100};
    std::unique_ptr<Enrollment> e;

    explicit Harness(Config cfg = default_config()) {
        e = std::make_unique<Enrollment>(
            cfg, crypto,
            [this](IssueRequest req, Enrollment::Completion done) {
                std::unique_lock<std::mutex> g(mu);
                requests.push_back(req);
                if (!auto_complete) {
                    completions.push_back(std::move(done));
                    return;
                }
                const NodeId node = req.role == Role::Client ? NodeId{next_node.value | kClientNodeBit} : next_node;
                g.unlock();
                done(Issued{node, toy_cert(format_quark_cn(kCluster, node), string_of(req.spki_der))});
            },
            [this](const AuditRecord& a) {
                std::lock_guard<std::mutex> g(mu);
                audits.push_back(a);
            },
            [] { return kUnixNow; });
    }
    static Config default_config() {
        Config c;
        c.cluster = kCluster;
        return c;
    }
    // The synchronous reply, if one was given (an accepted presentation waiting for the application has none).
    std::optional<Reply> submit(EnrolRequest r, Ticks now) {
        std::optional<Reply> out;
        e->submit(std::move(r), [&](Reply rep) { out = std::move(rep); }, now);
        return out;
    }
    Status status_of(EnrolRequest r, Ticks now) {
        const auto rep = submit(std::move(r), now);
        return rep ? rep->status : Status::Shutdown;  // tests that expect a synchronous reply never see this
    }
};
}  // namespace

int main() {
    bool ok = true;

    // ---- CN helper, wire collapse --------------------------------------------------------------------
    {
        ClusterId c{};
        NodeId n{};
        check(format_quark_cn(ClusterId{7}, NodeId{42}) == "quark:7:42", "member CN format", ok);
        const NodeId client{kClientNodeBit | 5};
        check(parse_quark_cn(format_quark_cn(ClusterId{7}, client), c, n) && c.value == 7 && n == client,
              "client CN round-trips through the strict parser", ok);
        check(wire_status(Status::UnknownTicket) == WireStatus::Refused && wire_status(Status::TicketUsed) == WireStatus::Refused &&
                  wire_status(Status::AttemptsExhausted) == WireStatus::Refused && wire_status(Status::BadMac) == WireStatus::Refused,
              "the wire collapses every ticket refusal into one code (no ticket-id oracle)", ok);
        check(wire_status(Status::RateLimited) == WireStatus::RetryLater && wire_status(Status::Busy) == WireStatus::RetryLater &&
                  wire_status(Status::IssuedCertInvalid) == WireStatus::IssuanceFailed &&
                  wire_status(Status::TimedOut) == WireStatus::IssuanceFailed && wire_status(Status::Issued) == WireStatus::Issued,
              "retry-later and issuance-failed classes", ok);
    }

    // ---- Tickets: TTL bounds, table bound, distinct random ids/secrets, revoke ----------------------
    {
        Config cfg = Harness::default_config();
        cfg.max_tickets = 2;
        cfg.max_ticket_ttl = 100 * kSec;
        Harness h(cfg);
        check(!h.e->issue_ticket(Role::Member, 0, 0), "a zero ttl is refused", ok);
        check(!h.e->issue_ticket(Role::Member, 101 * kSec, 0), "an over-long ttl is refused", ok);
        auto t1 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        auto t2 = h.e->issue_ticket(Role::Client, 50 * kSec, 0);
        check(t1 && t2 && t1->id != t2->id && t1->secret != t2->secret && t1->secret != TicketSecret{},
              "tickets get distinct random ids and secrets", ok);
        const auto full = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        check(!full && full.error().code == errc::overloaded, "a full ticket table refuses loudly", ok);
        check(static_cast<bool>(h.e->issue_ticket(Role::Member, 10 * kSec, 11 * kSec)),
              "control: an expired ticket's slot is reclaimed", ok);
        check(h.e->has_live_ticket(11 * kSec) && h.e->live_tickets(11 * kSec) == 2, "two live tickets", ok);
        check(h.e->revoke_ticket(t2->id) && !h.e->revoke_ticket(t2->id), "revoke_ticket withdraws once", ok);
        check(h.status_of(request_for(*t2, "K"), 12 * kSec) == Status::UnknownTicket, "a revoked ticket is refused", ok);
        check(h.e->live_tickets(11 * kSec) == 1 && !h.e->has_live_ticket(30 * kSec),
              "has_live_ticket is false once every ticket expired (the transport refuses enrol hellos)", ok);
    }

    // ---- Happy path: the role and cluster come from the ticket, the NodeId from the application -----
    {
        Harness h;
        auto t = h.e->issue_ticket(Role::Client, 60 * kSec, 0);
        const auto r = h.submit(request_for(*t, "SPKI-A"), 1);
        check(r && r->status == Status::Issued && !r->cert_chain_der.empty(), "a valid presentation is issued", ok);
        check(h.requests.size() == 1 && h.requests[0].role == Role::Client && h.requests[0].cluster == kCluster &&
                  string_of(h.requests[0].spki_der) == "SPKI-A",
              "IssueRequest carries the ticket's role/cluster and only the CSR's public key", ok);
        check(h.audits.size() == 1 && h.audits[0].status == Status::Issued && h.audits[0].assigned &&
                  is_client_node(*h.audits[0].assigned) && h.audits[0].source == "10.0.0.9",
              "one audit record with the assigned NodeId and source", ok);
        const int macs = h.crypto->mac_calls.load();
        check(h.status_of(request_for(*t, "SPKI-B"), 2) == Status::TicketUsed && h.requests.size() == 1,
              "C4: a used ticket never issues again", ok);
        check(h.crypto->mac_calls.load() == macs, "and its reuse is refused before any crypto", ok);
        check(!h.e->revoke_ticket(t->id), "a consumed ticket cannot be 'revoked' (its certificate exists)", ok);
        (void)h.e->expire(3);
        check(h.status_of(request_for(*t, "SPKI-B"), 4) == Status::UnknownTicket && h.requests.size() == 1,
              "and stays refused after the consumed ticket is pruned", ok);
    }

    // ---- Refusals: unknown, expired, MAC bound to ticket + cluster ---------------------------------
    {
        Harness h;
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        auto other = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        Ticket forged = *t;
        forged.id[0] ^= std::byte{1};
        check(h.status_of(request_for(forged, "K"), 1) == Status::UnknownTicket, "an unknown ticket id is refused", ok);
        check(h.status_of(request_for(*t, "K"), 10 * kSec) == Status::UnknownTicket, "an expired ticket is refused", ok);
        auto t3 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        EnrolRequest wrong_ticket = request_for(*t3, "K");
        wrong_ticket.tag = toy_mac(other->secret, enrol_mac_message(kCluster, t3->id, wrong_ticket.csr_der));
        check(h.status_of(wrong_ticket, 1) == Status::BadMac, "a tag made with another ticket's secret fails", ok);
        check(h.status_of(request_for(*t3, "K", "10.0.0.9", ClusterId{78}), 1) == Status::BadMac,
              "a tag bound to another cluster fails", ok);
        check(h.crypto->csr_calls.load() == 0, "the MAC gates all CSR work (no CSR parsed for a bad MAC)", ok);
        check(h.status_of(request_for(*t3, "K"), 1) == Status::Issued, "control: the ticket survives bad MACs and then issues", ok);
        check(h.audits.size() == 5, "one audit record per attempt, refused or not", ok);
    }

    // ---- Attempt limit; a bad CSR counts but does not consume ---------------------------------------
    {
        Config cfg = Harness::default_config();
        cfg.max_attempts_per_ticket = 3;
        Harness h(cfg);
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        EnrolRequest bad_csr = request_for(*t, "K");
        bad_csr.csr_der = toy_csr("K", "quark:77:1", false);
        bad_csr.tag = toy_mac(t->secret, enrol_mac_message(kCluster, t->id, bad_csr.csr_der));
        check(h.status_of(bad_csr, 1) == Status::BadCsr, "a CSR without proof of possession is refused", ok);
        check(h.status_of(bad_mac_for(*t), 1) == Status::BadMac, "a wrong MAC is refused", ok);
        check(h.status_of(request_for(*t, "K"), 1) == Status::Issued,
              "control: the third (valid) attempt is still allowed and issues", ok);
        auto t2 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        for (int i = 0; i < 3; ++i) (void)h.submit(bad_mac_for(*t2), 1);
        const int macs = h.crypto->mac_calls.load();
        check(h.status_of(request_for(*t2, "K"), 1) == Status::AttemptsExhausted && h.crypto->mac_calls.load() == macs,
              "after max attempts even a valid tag is refused, without MAC work", ok);
    }

    // ---- Races: presentations held past phase 1 until all arrive, so the later steps are contested ----
    {
        Config cfg = Harness::default_config();
        cfg.max_attempts_per_ticket = 8;
        Harness h(cfg);
        Gate gate;
        gate.need = 8;
        h.crypto->on_mac = [&] { gate.arrive_and_wait(); };
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        std::atomic<int> issued{0}, used{0}, replies{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&, i] {
                h.e->submit(request_for(*t, "K" + std::to_string(i), "10.0.2." + std::to_string(i)), [&](Reply r) {
                    replies.fetch_add(1);
                    if (r.status == Status::Issued) issued.fetch_add(1);
                    if (r.status == Status::TicketUsed) used.fetch_add(1);
                }, 1);
            });
        for (auto& th : threads) th.join();
        check(gate.arrived.load() == 8, "all eight presentations passed the MAC concurrently (the race is real)", ok);
        check(issued.load() == 1 && used.load() == 7 && replies.load() == 8 && h.requests.size() == 1,
              "C4: eight presentations racing past the MAC yield ONE issuance and seven refusals", ok);

        cfg.max_attempts_per_ticket = 5;
        Harness g(cfg);
        Gate guess_gate;
        guess_gate.need = 6;  // reached only if more than the 5-attempt budget gets to the MAC
        guess_gate.patience = std::chrono::milliseconds{300};  // a correct run always waits this out
        g.crypto->on_mac = [&] { guess_gate.arrive_and_wait(); };
        auto t2 = g.e->issue_ticket(Role::Member, 10 * kSec, 0);
        threads.clear();
        for (int i = 0; i < 16; ++i)
            threads.emplace_back([&, i] { g.e->submit(bad_mac_for(*t2, "10.0.1." + std::to_string(i)), [](Reply) {}, 1); });
        for (auto& th : threads) th.join();
        check(g.crypto->mac_calls.load() == 5,
              "16 concurrent guesses spend exactly the 5-attempt budget of MAC checks (reserved before crypto)", ok);
    }

    // ---- A ticket withdrawn between the two locked phases is not issued -----------------------------
    {
        Harness h;
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        h.crypto->on_mac = [&] { (void)h.e->revoke_ticket(t->id); };
        check(h.status_of(request_for(*t, "K"), 1) == Status::UnknownTicket && h.requests.empty(),
              "a ticket revoked while its MAC is checked is not issued", ok);
    }

    // ---- The returned certificate must be exactly what the ticket allows ---------------------------
    {
        struct Case {
            const char* what;
            std::function<Issued(const IssueRequest&)> make;
        };
        const std::vector<Case> cases = {
            {"a NodeId whose client bit contradicts the ticket's role",
             [](const IssueRequest& r) {
                 const NodeId n{kClientNodeBit | 9};  // ticket is Member
                 return Issued{n, toy_cert(format_quark_cn(kCluster, n), string_of(r.spki_der))};
             }},
            {"a CN that is not the canonical one for the assigned NodeId",
             [](const IssueRequest& r) { return Issued{NodeId{9}, toy_cert("quark:77:10", string_of(r.spki_der))}; }},
            {"a CN for another cluster",
             [](const IssueRequest& r) { return Issued{NodeId{9}, toy_cert("quark:78:9", string_of(r.spki_der))}; }},
            {"a different public key than the CSR's",
             [](const IssueRequest&) { return Issued{NodeId{9}, toy_cert("quark:77:9", "OTHER")}; }},
            {"a CA:TRUE certificate",
             [](const IssueRequest& r) { return Issued{NodeId{9}, toy_cert("quark:77:9", string_of(r.spki_der), true)}; }},
            {"a validity span beyond max_validity_s",
             [](const IssueRequest& r) {
                 return Issued{NodeId{9}, toy_cert("quark:77:9", string_of(r.spki_der), false, 366ULL * 86400)};
             }},
        };
        for (const Case& c : cases) {
            Harness h;
            h.auto_complete = false;
            auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
            std::optional<Reply> got;
            h.e->submit(request_for(*t, "SPKI"), [&](Reply r) { got = std::move(r); }, 1);
            h.completions.at(0)(c.make(h.requests.at(0)));
            check(got && got->status == Status::IssuedCertInvalid && got->cert_chain_der.empty(), c.what, ok);
            check(h.status_of(request_for(*t, "SPKI"), 2) == Status::TicketUsed, "and the ticket stays consumed (fail closed)", ok);
        }
        Harness h;  // control: the canonical certificate is accepted
        h.auto_complete = false;
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        std::optional<Reply> got;
        h.e->submit(request_for(*t, "SPKI"), [&](Reply r) { got = std::move(r); }, 1);
        h.completions.at(0)(Issued{NodeId{9}, toy_cert("quark:77:9", "SPKI")});
        check(got && got->status == Status::Issued, "control: the exact certificate is accepted", ok);
    }

    // ---- Async completion: failure, at-most-once, timeout, stale clock, destruction ----------------
    {
        Harness h;
        h.auto_complete = false;
        auto t = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        int replies = 0;
        Reply last{};
        h.e->submit(request_for(*t, "K"), [&](Reply r) { ++replies; last = std::move(r); }, 1);
        check(replies == 0 && h.e->pending_issues() == 1, "a presentation waits for the application", ok);
        check(!h.e->revoke_ticket(t->id), "a ticket whose issuance is in flight cannot be revoked", ok);
        Enrollment::Completion copy = h.completions.at(0);
        h.completions.at(0)(fail(errc::unavailable, "custodian offline"));
        check(replies == 1 && last.status == Status::IssuerFailed, "a custodian failure is reported", ok);
        copy(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        check(replies == 1, "a second completion is ignored (at most once)", ok);
        check(h.status_of(request_for(*t, "K"), 2) == Status::TicketUsed, "the ticket stays consumed after a failure", ok);

        auto t2 = h.e->issue_ticket(Role::Member, 100 * kSec, 0);
        int r2 = 0;
        Status s2{};
        h.e->submit(request_for(*t2, "K"), [&](Reply r) { ++r2; s2 = r.status; }, 5 * kSec);
        check(h.e->expire(4 * kSec) == 0 && r2 == 0,
              "an expire() whose clock reading predates the issuance does not time it out (no wrap)", ok);
        check(h.e->expire(30 * kSec) == 0 && r2 == 0, "not timed out before issue_timeout", ok);
        check(h.e->expire(66 * kSec) == 1 && r2 == 1 && s2 == Status::TimedOut, "timed out after issue_timeout", ok);
        h.completions.at(1)(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        check(r2 == 1 && h.e->pending_issues() == 0, "a completion after the timeout is ignored", ok);

        auto t3 = h.e->issue_ticket(Role::Member, 100 * kSec, 0);
        int r3 = 0;
        Status s3{};
        h.e->submit(request_for(*t3, "K"), [&](Reply r) { ++r3; s3 = r.status; }, 1);
        Enrollment::Completion orphan = h.completions.at(2);
        h.e.reset();
        check(r3 == 1 && s3 == Status::Shutdown, "destroying Enrollment replies Shutdown to in-flight issuances", ok);
        orphan(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        check(r3 == 1, "a completion after destruction is a no-op", ok);
    }

    // ---- In-flight cap: refused before any crypto, attempt refunded, ticket not consumed -------------
    {
        Config cfg = Harness::default_config();
        cfg.max_pending_issues = 1;
        cfg.max_attempts_per_ticket = 1;
        Harness h(cfg);
        h.auto_complete = false;
        auto t1 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        auto t2 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        std::optional<Reply> first;
        h.e->submit(request_for(*t1, "K"), [&](Reply r) { first = std::move(r); }, 1);
        check(!first && h.e->pending_issues() == 1, "the first issuance is in flight", ok);
        const int macs = h.crypto->mac_calls.load();
        const auto busy1 = h.submit(request_for(*t2, "K"), 1);
        const auto busy2 = h.submit(request_for(*t2, "K"), 1);
        check(busy1 && busy1->status == Status::Busy && busy2 && busy2->status == Status::Busy &&
                  h.requests.size() == 1 && h.crypto->mac_calls.load() == macs,
              "a full in-flight table refuses synchronously, before any crypto", ok);
        h.completions.at(0)(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        std::optional<Reply> second;
        h.e->submit(request_for(*t2, "K"), [&](Reply r) { second = std::move(r); }, 2);
        check(!second && h.requests.size() == 2,
              "control: Busy cost neither the ticket nor its single attempt; it issues once there is room", ok);
        h.completions.at(1)(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        check(second && second->status == Status::Issued, "and the reply arrives on completion", ok);
    }
    {
        // Capacity that fills BETWEEN the two locked phases: the late Busy refunds the attempt too.
        Config cfg = Harness::default_config();
        cfg.max_pending_issues = 1;
        cfg.max_attempts_per_ticket = 1;
        Harness h(cfg);
        h.auto_complete = false;
        auto t1 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        auto t2 = h.e->issue_ticket(Role::Member, 10 * kSec, 0);
        bool fired = false;
        h.crypto->on_mac = [&] {
            if (fired) return;
            fired = true;
            h.e->submit(request_for(*t1, "K"), [](Reply) {}, 1);  // takes the only in-flight slot
        };
        check(h.status_of(request_for(*t2, "K"), 1) == Status::Busy && h.requests.size() == 1,
              "the in-flight table filled during t2's MAC check: t2 is refused Busy at the consume step", ok);
        h.completions.at(0)(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        std::optional<Reply> retry;
        h.e->submit(request_for(*t2, "K"), [&](Reply r) { retry = std::move(r); }, 2);
        check(!retry && h.requests.size() == 2, "control: the late Busy refunded t2's single attempt; the retry issues", ok);
        // Complete it before `retry` leaves scope: destroying the harness would otherwise reply Shutdown into it.
        h.completions.at(1)(Issued{NodeId{9}, toy_cert("quark:77:9", "K")});
        check(retry && retry->status == Status::Issued, "and the retry's certificate is delivered", ok);
    }

    // ---- Per-source rate limit and its table bound --------------------------------------------------
    {
        Config cfg = Harness::default_config();
        cfg.max_attempts_per_source = 2;
        cfg.max_attempts_per_ticket = 100;
        cfg.max_sources = 3;
        Harness h(cfg);
        auto t = h.e->issue_ticket(Role::Member, 1000 * kSec, 0);
        (void)h.submit(bad_mac_for(*t, "1.2.3.4"), 1);
        (void)h.submit(bad_mac_for(*t, "1.2.3.4"), 1);
        const int macs = h.crypto->mac_calls.load();
        check(h.status_of(bad_mac_for(*t, "1.2.3.4"), 1) == Status::RateLimited && h.crypto->mac_calls.load() == macs,
              "a source over its window limit is refused before any crypto", ok);
        check(h.status_of(request_for(*t, "K", "5.6.7.8"), 1) == Status::Issued, "control: another source is unaffected", ok);
        (void)h.submit(bad_mac_for(*t, "9.9.9.9"), 1);
        check(h.status_of(bad_mac_for(*t, "8.8.8.8"), 1) == Status::RateLimited,
              "a full source table refuses NEW sources while every window is live", ok);
        auto t2 = h.e->issue_ticket(Role::Member, 1000 * kSec, 0);
        check(h.status_of(request_for(*t2, "K", "8.8.8.8"), 61 * kSec) == Status::Issued,
              "control: aged-out windows make room again", ok);
        auto t3 = h.e->issue_ticket(Role::Member, 1000 * kSec, 0);
        const std::string long_a = std::string(kMaxSourceKey, 'a') + "-one";
        const std::string long_b = std::string(kMaxSourceKey, 'a') + "-two";
        (void)h.submit(bad_mac_for(*t3, long_a), 62 * kSec);
        (void)h.submit(bad_mac_for(*t3, long_b), 62 * kSec);
        check(h.status_of(bad_mac_for(*t3, long_a), 62 * kSec) == Status::RateLimited,
              "source keys are bounded (two keys equal in their first kMaxSourceKey bytes share a window)", ok);
        auto t4 = h.e->issue_ticket(Role::Member, 1000 * kSec, 0);
        (void)h.submit(bad_mac_for(*t4, "7.7.7.7"), 100 * kSec);
        (void)h.submit(bad_mac_for(*t4, "7.7.7.7"), 100 * kSec);
        check(h.status_of(bad_mac_for(*t4, "7.7.7.7"), 99 * kSec) == Status::RateLimited,
              "an out-of-order (earlier) clock reading does not reset a source's window", ok);
    }

    // ---- Callbacks never run under the lock (re-entrant calls would deadlock) ----------------------
    {
        Config cfg = Harness::default_config();
        auto crypto = std::make_shared<MockCrypto>();
        std::unique_ptr<Enrollment> e;
        int reentered = 0;
        e = std::make_unique<Enrollment>(
            cfg, crypto,
            [&](IssueRequest req, Enrollment::Completion done) {
                reentered += static_cast<int>(e->pending_issues());
                done(Issued{NodeId{9}, toy_cert("quark:77:9", string_of(req.spki_der))});
            },
            [&](const AuditRecord&) { reentered += static_cast<int>(e->live_tickets(0)) + 1; }, [] { return kUnixNow; });
        auto t = e->issue_ticket(Role::Member, 10 * kSec, 0);
        Status s{};
        e->submit(request_for(*t, "K"), [&](Reply r) { s = r.status; reentered += static_cast<int>(e->live_tickets(1)) + 1; }, 1);
        e->submit(request_for(*t, "K"), [&](Reply) { reentered += static_cast<int>(e->pending_issues()) + 1; }, 1);
        check(s == Status::Issued && reentered >= 4, "IssueFn, audit and reply may call back into Enrollment", ok);
    }

    std::printf("enrollment_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
