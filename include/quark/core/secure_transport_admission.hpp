// Implements 020-Security §1a (application-driven admission, Draft amendment) and ADR-069 — the
// admission GATE that `SecureTransport` (secure_transport.hpp) composes. Split out so the transport file
// carries only call sites; everything stateful about admission lives here.
//
// WHAT THE GATE GIVES THE TRANSPORT
//   * `ask(peer)`: one pure read of the authority's CURRENT immutable snapshot, returning the verdict AND
//     the generation it was read under (a single atomic load, so the pair is consistent). Nothing is ever
//     cached across generations: every inbound frame, outbound frame and handshake completion asks again.
//   * a FENCE (`Scope` / `barrier`): every operation that acts on an answer (deliver, send, handshake
//     completion) runs inside a `Scope` (shared lock on `fence_`). `AdmissionAuthority::publish` swaps the
//     snapshot FIRST, then the transport's listener calls `barrier()` (an empty exclusive lock), which waits
//     out every operation that read an older generation. So when publish() returns, no message admitted
//     under an older generation is still being delivered, and no handshake that read an older generation
//     is still installing a session - guarantees (a)-(c) of 020 §1a. A `Scope` taken re-entrantly on the
//     same thread (a receive callback that sends, which on a synchronous inner transport re-enters
//     deliver) does not lock again, so it cannot deadlock against a waiting writer.
//   * counters and the test-only fault toggles that let each guarantee have a control that FAILS when the
//     guarantee is removed (ADR-069 claims C2-C4).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>

#include "quark/core/admission.hpp"

namespace quark {

// Observability for the admission path (SecureTransport::admission_stats()).
struct AdmissionStats {
    std::uint64_t publishes_seen = 0;           // generations the transport has re-answered for
    std::uint64_t generation = 0;               // newest generation the transport swept under
    std::uint64_t inbound_discarded = 0;        // frames from a Deny peer discarded (never delivered)
    std::uint64_t lane_refused = 0;             // non-control frames from/to a LogExchangeOnly peer dropped
    std::uint64_t send_dropped = 0;             // outbound frames to a Deny peer dropped
    std::uint64_t handshakes_refused = 0;       // opening Authenticate frames from a Deny peer refused
    std::uint64_t handshakes_denied_at_completion = 0;  // handshake finished, re-answer said Deny
    std::uint64_t handshakes_reanswered = 0;    // completions whose start generation was older than current
    std::uint64_t handshakes_aborted = 0;       // in-flight handshakes dropped by a publish
    std::uint64_t sessions_closed = 0;          // live sessions closed because a generation denied them
    std::uint64_t reentrant_publishes = 0;      // publish from inside a receive callback (no barrier)
};

// Test-only: remove ONE guarantee so its control can show the failure. Never set in production.
struct AdmissionTestFaults {
    bool skip_handshake_recheck = false;  // completion trusts the start answer; publish aborts nothing
    bool skip_live_sweep = false;         // publish does not close live sessions
    bool skip_inbound_gate = false;       // deliver ignores the verdict (no discard, no lane rule)
};

namespace detail {

class AdmissionGate {
public:
    struct Answer {
        AdmissionVerdict verdict = AdmissionVerdict::Admit;  // no authority attached == today's behaviour
        std::uint64_t generation = 0;
    };

    AdmissionGate() = default;
    AdmissionGate(const AdmissionGate&) = delete;
    AdmissionGate& operator=(const AdmissionGate&) = delete;
    ~AdmissionGate() { detach(); }

    void attach(AdmissionAuthority& a, AdmissionAuthority::Listener l) {
        detach();
        auth_ = &a;
        listener_id_ = a.add_listener(std::move(l));
    }
    void detach() noexcept {
        if (auth_) auth_->remove_listener(listener_id_);
        auth_ = nullptr;
    }
    [[nodiscard]] bool enabled() const noexcept { return auth_ != nullptr; }

    [[nodiscard]] Answer ask(NodeId peer) const noexcept {
        if (!auth_) return {};
        const auto s = auth_->current();
        return {s->lookup(peer), s->generation()};
    }
    [[nodiscard]] std::uint64_t generation() const noexcept { return auth_ ? auth_->generation() : 0; }

    // Shared lock for the lifetime of the scope unless this thread already holds it for this gate.
    class Scope {
    public:
        explicit Scope(AdmissionGate& g) {
            if (!g.auth_ || in_scope() == &g) return;
            g.fence_.lock_shared();
            gate_ = &g;
            outer_ = in_scope();
            in_scope() = &g;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() {
            if (!gate_) return;
            in_scope() = outer_;
            gate_->fence_.unlock_shared();
        }

    private:
        AdmissionGate* gate_ = nullptr;
        const AdmissionGate* outer_ = nullptr;
    };

    // Wait out every Scope that read an older generation. From inside a Scope on this thread an exclusive
    // lock would deadlock: skip it and count it (the only weakening of the guarantee, ADR-069 §Residual).
    void barrier() {
        if (in_scope() == this) {
            ++reentrant_publishes;
            return;
        }
        std::unique_lock<std::shared_mutex> g(fence_);
    }

    AdmissionTestFaults faults;  // set before traffic starts (plain bools; test-only)
    std::atomic<std::uint64_t> publishes_seen{0}, generation_swept{0}, inbound_discarded{0}, lane_refused{0},
        send_dropped{0}, handshakes_refused{0}, handshakes_denied_at_completion{0}, handshakes_reanswered{0},
        handshakes_aborted{0}, sessions_closed{0}, reentrant_publishes{0};

    [[nodiscard]] AdmissionStats stats() const noexcept {
        const auto r = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
        return AdmissionStats{r(publishes_seen),
                              r(generation_swept),
                              r(inbound_discarded),
                              r(lane_refused),
                              r(send_dropped),
                              r(handshakes_refused),
                              r(handshakes_denied_at_completion),
                              r(handshakes_reanswered),
                              r(handshakes_aborted),
                              r(sessions_closed),
                              r(reentrant_publishes)};
    }

private:
    static const AdmissionGate*& in_scope() noexcept {
        static thread_local const AdmissionGate* p = nullptr;
        return p;
    }
    AdmissionAuthority* auth_ = nullptr;
    std::uint64_t listener_id_ = 0;
    std::shared_mutex fence_;
};

}  // namespace detail
}  // namespace quark
