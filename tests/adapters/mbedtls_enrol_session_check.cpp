// Conformance check for ADR-074 D4 / issue #36 (D4c step 3) over REAL crypto: one whole enrolment in memory, between `EnrolJoinerSession` and
// `EnrolAnchorSession` (mbedtls_enrol_session.hpp), the anchor backed by a real `Enrollment` + `MbedtlsEnrolCrypto` and a simulated custodian.
//
//   S1  a member and a client ticket each enrol end to end, at several chunk sizes; the joiner ends with a certificate for ITS OWN key, of the ticket's
//       role, chaining to the pinned CA, and an identity the C5 startup check accepts; the same join code a second time is Refused
//   S2  refusals reach the joiner collapsed: a wrong ticket secret and an unknown ticket are both `Refused`; a wrong secret does not burn the ticket
//   S3  an issuance that completes LATER, from another thread, is delivered by drain() after on_ready; destroying the anchor session first is safe
//   S4  the joiner does not take the anchor's word: against an anchor whose crypto approves anything, a certificate for another key, another cluster, a
//       CA-capable leaf, or one from a CA the join code did not pin makes the joiner FAIL (no identity)
//   S5  the anchor refuses protocol violations: garbage request, two requests, an absurd length prefix, a joiner closing before any request
//   S6  a joiner with a wrong CA pin fails in the handshake and the anchor consumes nothing (the ticket survives)
// Only built when QUARK_WITH_MBEDTLS=ON.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include "adapters/enrol_test_pki.hpp"

using namespace quark;
using namespace quark::adapters;
using quark::adapters::detail::Pk;
using quark::adapters::detail::SharedRng;

namespace {

using namespace enrol_test;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

// --- Pump ----------------------------------------------------------------------------------------------------------

void deliver_to_anchor(EnrolAnchorSession& a, const Bytes& data, std::size_t chunk, Bytes& out) {
    for (std::size_t at = 0; at < data.size(); at += chunk) {
        const std::size_t n = std::min(chunk, data.size() - at);
        a.feed(std::span<const std::byte>(data).subspan(at, n), out);
    }
}
void deliver_to_joiner(EnrolJoinerSession& j, const Bytes& data, std::size_t chunk, Bytes& out) {
    for (std::size_t at = 0; at < data.size(); at += chunk) {
        const std::size_t n = std::min(chunk, data.size() - at);
        j.feed(std::span<const std::byte>(data).subspan(at, n), out);
    }
}
bool joiner_done(const EnrolJoinerSession& j) {
    using S = EnrolJoinerSession::State;
    return j.state() == S::Joined || j.state() == S::Rejected || j.state() == S::Failed;
}

// Runs both ends to a terminal joiner state (or a stall). Returns the joiner's final state.
EnrolJoinerSession::State run(EnrolJoinerSession& j, EnrolAnchorSession& a, std::size_t chunk) {
    Bytes to_a, to_j;
    j.start(to_a);
    for (int i = 0; i < 400 && !joiner_done(j); ++i) {
        Bytes in_a = std::move(to_a);
        to_a.clear();
        if (!in_a.empty()) deliver_to_anchor(a, in_a, chunk, to_j);
        a.drain(to_j);
        Bytes in_j = std::move(to_j);
        to_j.clear();
        if (!in_j.empty()) deliver_to_joiner(j, in_j, chunk, to_a);
        if (in_a.empty() && in_j.empty() && to_a.empty()) break;  // stalled
    }
    return j.state();
}

enrol::Ticket new_ticket(World& w, enrol::Role role) {
    auto t = w.enr->issue_ticket(role, 60'000'000'000, w.clock.load());
    if (!t) {
        std::fprintf(stderr, "ticket issue failed\n");
        std::exit(2);
    }
    return *t;
}

using ParkedIssue = std::pair<enrol::IssueRequest, enrol::Enrollment::Completion>;

}  // namespace

int main() {
    using JS = EnrolJoinerSession::State;
    using AS = EnrolAnchorSession::State;

    // ------------------------------------------------------------------------------------------------- S1
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{13}, std::size_t{4096}}) {
        for (const auto role : {enrol::Role::Member, enrol::Role::Client}) {
            World w;
            const auto t = new_ticket(w, role);
            EnrolJoinerSession j(w.code(t));
            auto a = w.anchor();
            const JS end = run(j, *a, chunk);
            check(end == JS::Joined, "S1 the joiner ends Joined");
            check(a->state() == AS::Replied && a->outcome() == enrol::Status::Issued, "S1 the anchor ended Replied/Issued");
            auto id = j.take_identity();
            check(id.has_value(), "S1 an identity is taken");
            if (!id) continue;
            check(id->role == role && is_client_node(id->node) == (role == enrol::Role::Client), "S1 the role is the ticket's");
            check(id->cluster.value == kCluster && (id->node.value & ~kClientNodeBit) == kNode, "S1 cluster and NodeId are the custodian's");
            check(id->ca_der == w.root.cert, "S1 the CA is the pinned one");
            TlsIdentity own{id->cert_chain_der, id->key_der, {}};
            check(static_cast<bool>(validate_identity_not_root(own, w.trust->snapshot(0))), "S1 the joined identity passes the C5 startup check");
            check(!j.take_identity().has_value(), "S1 the identity can be taken once only");

            // The same code again: the ticket is spent. Positive control above, refusal here.
            EnrolJoinerSession j2(w.code(t));
            auto a2 = w.anchor();
            check(run(j2, *a2, chunk) == JS::Rejected && j2.rejection() == enrol::WireStatus::Refused, "S1 a second use of the join code is Refused");
            check(a2->outcome() == enrol::Status::TicketUsed, "S1 and the anchor's audit status is the detailed TicketUsed");
        }
    }

    // ------------------------------------------------------------------------------------------------- S2
    {
        World w;
        const auto t = new_ticket(w, enrol::Role::Member);
        auto bad = w.code(t);
        bad.ticket_secret[0] ^= std::byte{1};
        EnrolJoinerSession jb(bad);
        auto ab = w.anchor();
        check(run(jb, *ab, 64) == JS::Rejected && jb.rejection() == enrol::WireStatus::Refused, "S2 a wrong secret is Refused");
        check(ab->outcome() == enrol::Status::BadMac, "S2 (audit: BadMac)");
        check(!jb.take_identity().has_value(), "S2 a rejected joiner has no identity");
        auto unknown = w.code(t);
        unknown.ticket_id[0] ^= std::byte{1};
        EnrolJoinerSession ju(unknown);
        auto au = w.anchor();
        check(run(ju, *au, 64) == JS::Rejected && ju.rejection() == enrol::WireStatus::Refused, "S2 an unknown ticket is Refused with the SAME wire status");
        check(au->outcome() == enrol::Status::UnknownTicket, "S2 (audit: UnknownTicket)");
        EnrolJoinerSession jg(w.code(t));
        auto ag = w.anchor();
        check(run(jg, *ag, 64) == JS::Joined, "S2 CONTROL: the genuine joiner still succeeds after a wrong-secret attempt");
    }

    // ------------------------------------------------------------------------------------------------- S3
    {
        World w;
        std::mutex mu;
        std::vector<ParkedIssue> parked;
        w.custodian_override = [&](enrol::IssueRequest req, enrol::Enrollment::Completion done) {
            std::lock_guard<std::mutex> g(mu);
            parked.emplace_back(std::move(req), std::move(done));
        };
        const auto t = new_ticket(w, enrol::Role::Member);
        EnrolJoinerSession j(w.code(t));
        std::atomic<int> ready{0};
        auto a = w.anchor([&] { ++ready; });
        run(j, *a, 4096);
        check(j.state() == JS::AwaitingReply, "S3 the joiner waits while the custodian has not answered");
        check(a->state() == AS::Issuing && ready == 0, "S3 the anchor is Issuing and not yet notified");
        std::thread th([&] {
            ParkedIssue p = [&] {
                std::lock_guard<std::mutex> g(mu);
                ParkedIssue first = std::move(parked.front());
                parked.erase(parked.begin());
                return first;
            }();
            p.second(w.sign(p.first));
        });
        th.join();
        check(ready == 1, "S3 on_ready fired once, from the completing thread");
        Bytes to_j, to_a;
        a->drain(to_j);
        check(a->state() == AS::Replied && !to_j.empty(), "S3 drain seals the reply");
        deliver_to_joiner(j, to_j, 4096, to_a);
        check(j.state() == JS::Joined, "S3 the joiner completes with the late reply");

        // Destroying the anchor session while an issuance is in flight is safe; the late completion is dropped.
        const auto t2 = new_ticket(w, enrol::Role::Member);
        EnrolJoinerSession j2(w.code(t2));
        std::atomic<int> ready2{0};
        auto a2 = w.anchor([&] { ++ready2; });
        run(j2, *a2, 4096);
        check(a2->state() == AS::Issuing, "S3 the second anchor session is Issuing");
        a2.reset();
        ParkedIssue p2 = [&] {
            std::lock_guard<std::mutex> g(mu);
            ParkedIssue last = std::move(parked.back());
            parked.pop_back();
            return last;
        }();
        p2.second(w.sign(p2.first));
        check(ready2 == 0, "S3 a destroyed session is never notified");
    }

    // ------------------------------------------------------------------------------------------------- S4
    {
        {
            World w(true);  // CONTROL: the lenient anchor with an honest custodian still enrols, so a failure below is the joiner's own check
            const auto t = new_ticket(w, enrol::Role::Member);
            EnrolJoinerSession j(w.code(t));
            auto a = w.anchor();
            check(run(j, *a, 4096) == JS::Joined, "S4 CONTROL: the lenient anchor with an honest custodian enrols");
        }
        const std::pair<World::Fault, const char*> faults[] = {{World::Fault::OtherKey, "S4 the joiner refuses a certificate for another key"},
                                                               {World::Fault::OtherCluster, "S4 the joiner refuses a certificate of another cluster"},
                                                               {World::Fault::CaLeaf, "S4 the joiner refuses a CA-capable leaf"},
                                                               {World::Fault::Rogue, "S4 the joiner refuses a certificate from a CA it did not pin"}};
        for (const auto& [fault, what] : faults) {
            World w(true);
            w.fault = fault;
            const auto t = new_ticket(w, enrol::Role::Member);
            EnrolJoinerSession j(w.code(t));
            auto a = w.anchor();
            const JS end = run(j, *a, 4096);
            check(a->outcome() == enrol::Status::Issued, "S4 (the lenient anchor did issue it)");
            check(end == JS::Failed && !j.take_identity().has_value(), what);
        }
    }

    // ------------------------------------------------------------------------------------------------- S5
    {
        // Drive the anchor with a hand-made joiner: handshake with a bare EnrolJoinerChannel, then send what the test wants.
        auto handshaken = [](World& w, std::unique_ptr<EnrolAnchorSession>& a, std::unique_ptr<EnrolJoinerChannel>& jc) {
            a = w.anchor();
            jc = std::make_unique<EnrolJoinerChannel>(AnchorPin{NodeId{kAnchor}, ClusterId{kCluster}, w.pin});
            Bytes to_a, to_j;
            auto step = jc->advance({}, to_a);
            for (int i = 0; i < 20; ++i) {
                Bytes in_a = std::move(to_a);
                to_a.clear();
                if (!in_a.empty()) a->feed(in_a, to_j);
                Bytes in_j = std::move(to_j);
                to_j.clear();
                if (step != EnrolChannel::Step::Done && !in_j.empty()) step = jc->advance(in_j, to_a);
                if (in_a.empty() && in_j.empty() && to_a.empty()) break;
            }
            return step == EnrolChannel::Step::Done && a->state() == AS::AwaitingRequest;
        };
        auto send = [](EnrolAnchorSession& a, EnrolJoinerChannel& jc, const Bytes& framed) {
            Bytes rec, out;
            if (!jc.seal(framed, rec)) return;
            a.feed(rec, out);
        };
        World w;
        auto genuine = [&](const enrol::Ticket& t) {
            const auto key = generate_identity_key();
            const auto csr = make_enrol_csr(*key);
            enrol::EnrolRequest rq;
            rq.ticket_id = t.id;
            rq.csr_der = *csr;
            rq.tag = *compute_enrol_mac(t.secret, ClusterId{kCluster}, t.id, *csr);
            return enrol::wire::frame_message(*enrol::wire::encode_request(rq));
        };
        {
            const auto t = new_ticket(w, enrol::Role::Member);
            std::unique_ptr<EnrolAnchorSession> a;
            std::unique_ptr<EnrolJoinerChannel> jc;
            check(handshaken(w, a, jc), "S5 setup: a handshaken connection");
            send(*a, *jc, genuine(t));
            check(a->state() == AS::Replied, "S5 CONTROL: a genuine request is answered");
        }
        {
            std::unique_ptr<EnrolAnchorSession> a;
            std::unique_ptr<EnrolJoinerChannel> jc;
            check(handshaken(w, a, jc), "S5 setup");
            send(*a, *jc, enrol::wire::frame_message(Bytes(40, std::byte{0x7A})));
            check(a->state() == AS::Failed, "S5 garbage as a request fails the session");
        }
        {
            const auto t = new_ticket(w, enrol::Role::Member);
            std::unique_ptr<EnrolAnchorSession> a;
            std::unique_ptr<EnrolJoinerChannel> jc;
            check(handshaken(w, a, jc), "S5 setup");
            Bytes two = genuine(t);
            const Bytes again = genuine(t);
            two.insert(two.end(), again.begin(), again.end());
            send(*a, *jc, two);
            check(a->state() == AS::Failed, "S5 two requests in one flight fail the session");
            check(w.enr->live_tickets(w.clock.load()) == 1, "S5 and the ticket was never presented (still live)");
        }
        {
            std::unique_ptr<EnrolAnchorSession> a;
            std::unique_ptr<EnrolJoinerChannel> jc;
            check(handshaken(w, a, jc), "S5 setup");
            Bytes huge;
            enrol::wire::detail::put_le(huge, enrol::wire::kMaxMessageBytes + 1, 4);
            send(*a, *jc, huge);
            check(a->state() == AS::Failed, "S5 an absurd length prefix fails the session");
        }
        {
            std::unique_ptr<EnrolAnchorSession> a;
            std::unique_ptr<EnrolJoinerChannel> jc;
            check(handshaken(w, a, jc), "S5 setup");
            Bytes close_rec, out;
            jc->close(close_rec);
            a->feed(close_rec, out);
            check(a->state() == AS::Failed, "S5 a joiner closing before any request fails the session");
        }
    }

    // ------------------------------------------------------------------------------------------------- S6
    {
        World w;
        const auto t = new_ticket(w, enrol::Role::Member);
        auto code = w.code(t);
        code.ca_root_sha256[0] ^= std::byte{1};
        EnrolJoinerSession j(code);
        auto a = w.anchor();
        check(run(j, *a, 4096) == JS::Failed, "S6 a joiner with a wrong CA pin fails");
        check(!a->outcome().has_value() && a->state() != AS::Replied, "S6 the anchor never saw a request");
        check(w.enr->live_tickets(w.clock.load()) == 1, "S6 the ticket survives");
        EnrolJoinerSession good(w.code(t));
        auto a2 = w.anchor();
        check(run(good, *a2, 4096) == JS::Joined, "S6 CONTROL: the right pin joins");
    }

    std::printf(g_ok ? "mbedtls_enrol_session_check: all checks passed\n" : "mbedtls_enrol_session_check: FAILED\n");
    return g_ok ? 0 : 1;
}
