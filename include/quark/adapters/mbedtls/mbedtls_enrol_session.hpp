// ADR-074 D4 / issue #36 (D4c step 3): the two ends of ONE enrolment, transport-agnostic and driven by bytes (as `EnrolChannel`), so the whole
// exchange can be tested in memory and the TCP integration (step 4) only moves bytes.
//
//   EnrolAnchorSession  the anchor's end of one connection: handshake (EnrolServerChannel) -> exactly ONE framed EnrolRequest -> Enrollment::submit ->
//                       the reply (a collapsed WireStatus, ADR-074) sealed back -> close. Any other input is a protocol violation and ends the session.
//   EnrolJoinerSession  the joiner's end: generates its own identity key (it never leaves this object except inside `JoinedIdentity`), sends the CSR and
//                       the MAC, and ACCEPTS what comes back only after checking it itself: the leaf carries its own public key, chains to the CA the
//                       join code pinned, is a member/client identity of the join code's cluster, is not CA-capable, and is not the anchor's id.
//                       The joiner never takes the anchor's word for what it issued.
//
// Threading. The anchor session is single-threaded for everything the transport calls (`feed`, `drain`); only the Enrollment reply may arrive on another
// thread (the custodian's), which parks it in a shared slot and calls `on_ready` (under the slot's lock: it must be cheap, must not call back into the
// session and must not destroy it). The transport then calls `drain` on its own thread. Destroying the session while an issuance is in flight is safe: the
// slot outlives it and the late reply is dropped.
//
// Not here (step 4): admission (`has_live_ticket`, connection caps), the per-source key, the clock source, running this off the I/O thread, scheduling
// `Enrollment::expire`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <mbedtls/x509_crt.h>

#include "quark/adapters/mbedtls/mbedtls_enrol_channel.hpp"
#include "quark/core/enrol_wire.hpp"
#include "quark/core/enrollment.hpp"
#include "quark/core/secret.hpp"

namespace quark::adapters {

class EnrolAnchorSession {
public:
    enum class State { Handshaking, AwaitingRequest, Issuing, Replied, Failed };
    using Clock = std::function<enrol::Ticks()>;

    // `source` keys the per-source rate limit (see enrol::EnrolRequest). `now` is the anchor's monotonic clock in nanoseconds.
    EnrolAnchorSession(const TlsIdentity& own, enrol::Enrollment& enrollment, std::string source, Clock now, std::function<void()> on_ready = {})
        : channel_(own), enrollment_(enrollment), source_(std::move(source)), now_(std::move(now)), slot_(std::make_shared<Slot>()) {
        slot_->notify = std::move(on_ready);
    }
    ~EnrolAnchorSession() {
        std::lock_guard<std::mutex> g(slot_->mu);
        slot_->notify = nullptr;
        slot_->abandoned = true;
    }
    EnrolAnchorSession(const EnrolAnchorSession&) = delete;
    EnrolAnchorSession& operator=(const EnrolAnchorSession&) = delete;

    // Inbound bytes from the joiner; bytes to send are APPENDED to `out`. Returns the state afterwards. After Replied or Failed it does nothing.
    State feed(std::span<const std::byte> in, std::vector<std::byte>& out) {
        if (state_ == State::Replied || state_ == State::Failed) return state_;
        if (state_ == State::Handshaking) {
            const auto step = channel_.advance(in, out);
            if (step == EnrolChannel::Step::Failed) return fail(channel_.failure_reason());
            if (step != EnrolChannel::Step::Done) return state_;
            state_ = State::AwaitingRequest;
            in = {};  // the handshake consumed it; application data that rode along is read from the channel's buffer below
        }
        std::vector<std::byte> plain;
        const auto r = channel_.open(in, plain, out);
        if (r == EnrolChannel::ReadResult::Failed) return fail(channel_.failure_reason());
        if (!plain.empty()) {
            if (state_ == State::Issuing) return fail("application data after the request");
            if (!reader_.feed(plain)) return fail("enrol message framing violation");
        }
        if (state_ == State::AwaitingRequest) {
            std::vector<std::byte> msg;
            if (reader_.next(msg)) {
                auto req = enrol::wire::decode_request(msg);
                if (!req) return fail("malformed enrol request");
                std::vector<std::byte> extra;
                if (reader_.next(extra)) return fail("more than one request on a connection");
                req->source = source_;
                state_ = State::Issuing;
                std::shared_ptr<Slot> slot = slot_;
                enrollment_.submit(std::move(*req), [slot](enrol::Reply rep) { slot->put(std::move(rep)); }, now_());
            }
        }
        if (r == EnrolChannel::ReadResult::Closed && state_ != State::Replied) return fail("joiner closed before the reply");
        return try_reply(out);
    }

    // Seal the reply if the Enrollment has produced one. Call after `on_ready`. Bytes to send are APPENDED to `out`.
    State drain(std::vector<std::byte>& out) {
        if (state_ != State::Issuing) return state_;
        return try_reply(out);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::string_view failure_reason() const noexcept { return failure_; }
    // The detailed outcome (for the anchor's own logs; the joiner only ever sees the collapsed WireStatus).
    [[nodiscard]] std::optional<enrol::Status> outcome() const noexcept { return outcome_; }

private:
    struct Slot {
        std::mutex mu;
        std::optional<enrol::Reply> reply;
        std::function<void()> notify;
        bool abandoned = false;
        void put(enrol::Reply r) {
            std::lock_guard<std::mutex> g(mu);
            if (abandoned) return;
            reply = std::move(r);
            if (notify) notify();
        }
    };

    State try_reply(std::vector<std::byte>& out) {
        if (state_ != State::Issuing) return state_;
        std::optional<enrol::Reply> rep;
        {
            std::lock_guard<std::mutex> g(slot_->mu);
            rep = std::move(slot_->reply);
            slot_->reply.reset();
        }
        if (!rep) return state_;
        outcome_ = rep->status;
        enrol::wire::WireReply wr{enrol::wire_status(rep->status), {}};
        if (wr.status == enrol::WireStatus::Issued) wr.cert_chain_der = std::move(rep->cert_chain_der);
        auto enc = enrol::wire::encode_reply(wr);
        if (!enc) enc = enrol::wire::encode_reply(enrol::wire::WireReply{enrol::WireStatus::IssuanceFailed, {}});
        if (!enc || !channel_.seal(enrol::wire::frame_message(*enc), out)) return fail("cannot seal the reply");
        channel_.close(out);
        state_ = State::Replied;
        return state_;
    }
    State fail(std::string_view why) {
        failure_ = std::string(why);
        state_ = State::Failed;
        return state_;
    }

    EnrolServerChannel channel_;
    enrol::Enrollment& enrollment_;
    std::string source_;
    Clock now_;
    std::shared_ptr<Slot> slot_;
    enrol::wire::MessageReader reader_;
    State state_ = State::Handshaking;
    std::string failure_;
    std::optional<enrol::Status> outcome_;
};

// What a successful enrolment yields: everything needed to build a TlsIdentity and a trust store.
struct JoinedIdentity {
    std::vector<std::byte> cert_chain_der;  // leaf first, as issued
    std::vector<std::byte> key_der;         // the joiner's own key (SEC1), generated here
    std::vector<std::byte> ca_der;          // the CA the join code pinned
    NodeId node{};
    ClusterId cluster{};
    enrol::Role role = enrol::Role::Member;
};

class EnrolJoinerSession {
public:
    enum class State { Handshaking, AwaitingReply, Joined, Rejected, Failed };

    explicit EnrolJoinerSession(const enrol::wire::JoinCode& code)
        : code_(code), channel_(AnchorPin{code.anchor_node, code.cluster, code.ca_root_sha256}) {
        auto key = generate_identity_key();
        if (!key) {
            fail("cannot generate the identity key");
            return;
        }
        key_ = std::move(*key);
        auto csr = make_enrol_csr(key_);
        if (!csr) {
            fail("cannot build the CSR");
            return;
        }
        csr_ = std::move(*csr);
        auto tag = compute_enrol_mac(code.ticket_secret, code.cluster, code.ticket_id, csr_);
        if (!tag) {
            fail("cannot compute the enrolment MAC");
            return;
        }
        tag_ = *tag;
    }
    ~EnrolJoinerSession() { wipe(); }
    EnrolJoinerSession(const EnrolJoinerSession&) = delete;
    EnrolJoinerSession& operator=(const EnrolJoinerSession&) = delete;

    // First call: appends the ClientHello to `out`.
    State start(std::vector<std::byte>& out) {
        if (state_ != State::Handshaking || started_) return state_;
        started_ = true;
        return advance_handshake({}, out);
    }

    // Inbound bytes from the anchor; bytes to send are APPENDED to `out`.
    State feed(std::span<const std::byte> in, std::vector<std::byte>& out) {
        if (state_ == State::Joined || state_ == State::Rejected || state_ == State::Failed) return state_;
        if (state_ == State::Handshaking) {
            if (!started_) return fail("feed before start");
            return advance_handshake(in, out);
        }
        return read_reply(in, out);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::string_view failure_reason() const noexcept { return failure_; }
    // Rejected only: the collapsed reason the anchor gave.
    [[nodiscard]] enrol::WireStatus rejection() const noexcept { return rejection_; }
    // Joined only: moves the identity (including the private key) out; the session keeps nothing.
    [[nodiscard]] std::optional<JoinedIdentity> take_identity() {
        if (state_ != State::Joined || !identity_) return std::nullopt;
        auto id = std::move(identity_);
        identity_.reset();
        wipe();
        return id;
    }

private:
    State advance_handshake(std::span<const std::byte> in, std::vector<std::byte>& out) {
        const auto step = channel_.advance(in, out);
        if (step == EnrolChannel::Step::Failed) return fail(channel_.failure_reason());
        if (step != EnrolChannel::Step::Done) return state_;
        enrol::EnrolRequest rq;
        rq.ticket_id = code_.ticket_id;
        rq.tag = tag_;
        rq.csr_der = csr_;
        auto enc = enrol::wire::encode_request(rq);
        if (!enc || !channel_.seal(enrol::wire::frame_message(*enc), out)) return fail("cannot seal the request");
        state_ = State::AwaitingReply;
        return read_reply({}, out);  // a reply cannot already be here, but bytes the handshake read ahead may be
    }

    State read_reply(std::span<const std::byte> in, std::vector<std::byte>& out) {
        std::vector<std::byte> plain;
        const auto r = channel_.open(in, plain, out);
        if (r == EnrolChannel::ReadResult::Failed) return fail(channel_.failure_reason());
        if (!plain.empty() && !reader_.feed(plain)) return fail("enrol message framing violation");
        std::vector<std::byte> msg;
        if (reader_.next(msg)) {
            auto rep = enrol::wire::decode_reply(msg);
            if (!rep) return fail("malformed enrol reply");
            if (rep->status != enrol::WireStatus::Issued) {
                rejection_ = rep->status;
                state_ = State::Rejected;
                wipe();
                return state_;
            }
            return accept(rep->cert_chain_der);
        }
        if (r == EnrolChannel::ReadResult::Closed) return fail("anchor closed before the reply");
        return state_;
    }

    // The joiner's own judgement of what the anchor issued.
    State accept(const std::vector<std::byte>& chain_der) {
        detail::Crt chain;
        if (!detail::parse_der_concat(chain.v, chain_der)) return fail("issued chain does not parse");
        detail::Pk key;
        if (!detail::parse_private_key(key.v, key_)) return fail("own key does not parse");
        detail::Spki own, theirs;
        if (!detail::spki_of(key.v, own) || !detail::spki_of(chain.v.pk, theirs) || !detail::same_bytes(own.bytes(), theirs.bytes()))
            return fail("issued certificate is not for the joiner's key");
        const auto& ca_der = channel_.pinned_ca_der();
        if (ca_der.empty()) return fail("no pinned CA");
        detail::Crt trust;
        if (mbedtls_x509_crt_parse_der(&trust.v, detail::uchars(ca_der), ca_der.size()) != 0) return fail("pinned CA does not parse");
        std::uint32_t flags = 0;
        if (mbedtls_x509_crt_verify(&chain.v, &trust.v, nullptr, nullptr, &flags, nullptr, nullptr) != 0 || flags != 0)
            return fail("issued certificate does not chain to the pinned CA");
        NodeId node{};
        ClusterId cluster{};
        if (!detail::parse_cn_identity(chain.v, node, cluster)) return fail("issued certificate CN is not a quark identity");
        if (!(cluster == code_.cluster)) return fail("issued certificate is for another cluster");
        if (node.value == 0 || (node.value & ~kClientNodeBit) == (code_.anchor_node.value & ~kClientNodeBit)) return fail("issued NodeId is invalid or the anchor's own");
        if (detail::ca_capable(chain.v)) return fail("issued certificate is CA-capable");
        JoinedIdentity id;
        id.cert_chain_der = chain_der;
        id.key_der = key_;
        id.ca_der = ca_der;
        id.node = node;
        id.cluster = cluster;
        id.role = is_client_node(node) ? enrol::Role::Client : enrol::Role::Member;
        identity_ = std::move(id);
        state_ = State::Joined;
        return state_;
    }

    State fail(std::string_view why) {
        failure_ = std::string(why);
        state_ = State::Failed;
        wipe();
        return state_;
    }
    void wipe() {
        if (!key_.empty()) quark::detail::secure_zero(key_.data(), key_.size());
        key_.clear();
    }

    enrol::wire::JoinCode code_;
    EnrolJoinerChannel channel_;
    std::vector<std::byte> key_, csr_;
    enrol::MacTag tag_{};
    enrol::wire::MessageReader reader_;
    State state_ = State::Handshaking;
    bool started_ = false;
    std::string failure_;
    enrol::WireStatus rejection_ = enrol::WireStatus::Refused;
    std::optional<JoinedIdentity> identity_;
};

}  // namespace quark::adapters
