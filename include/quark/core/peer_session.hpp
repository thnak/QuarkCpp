// Implements ADR-040's REQUIRED pre-merge hardening item ("same-session send-path ordering") plus
// the C4 role-aware directional-key split and S5 revocation-enforcement state: `PeerSession`, the
// per-peer session object `SecureTransport` (secure_transport.hpp) now holds instead of two bare
// send_seq_/recv_high_ maps under one shared mutex.
//
// THE REQUIRED FIX: `session_mu_` is a PER-SESSION lock, never shared across peers. Every hot-path
// caller (SecureTransport::send/deliver) must hold session_mu() for the WHOLE seq-draw + AEAD-seal/
// open + wire-handoff critical section — not just the seq-draw, which was the pre-ADR-040 shape that
// let concurrent same-session senders' wire-arrival order diverge from their seq-assignment order,
// causing a genuinely fresh frame to be spuriously rejected as a replay (imported from ADR-040 Design
// 1's proven C14 methodology). Because the lock is per-session (not the structural map lock in
// secure_transport.hpp), AEAD seal/open for one peer never blocks another peer's session — this is
// simultaneously the required fix AND the F2-revised invariant ("crypto never runs under a lock
// shared with any other peer's session").
//
// Directional ciphers (C4): a real mTLS exporter derives distinct client-write/server-write traffic
// secrets, so send_cipher_ and recv_cipher_ are genuinely different keys, not one shared cipher.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "quark/core/aead.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark {

class PeerSession {
public:
    PeerSession(NodeId peer, std::shared_ptr<const Aead> send_cipher,
                std::shared_ptr<const Aead> recv_cipher, std::uint64_t generation,
                Fingerprint peer_fingerprint, std::int64_t now_ns = 0) noexcept
        : peer_(peer),
          send_cipher_(std::move(send_cipher)),
          recv_cipher_(std::move(recv_cipher)),
          generation_(generation),
          peer_fingerprint_(peer_fingerprint),
          renegotiated_at_ns_(now_ns) {}

    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    // --- Hot path: caller MUST already hold session_mu() for all of these. --------------------
    [[nodiscard]] std::mutex& session_mu() noexcept { return session_mu_; }
    [[nodiscard]] const Aead& send_cipher() const noexcept { return *send_cipher_; }
    [[nodiscard]] const Aead& recv_cipher() const noexcept { return *recv_cipher_; }

    // Draws the next outbound sequence number. Combined with the caller holding session_mu() across
    // seal()+wire-handoff, this is what makes seq order and wire-arrival order agree (the required fix).
    [[nodiscard]] std::uint64_t next_send_seq() noexcept { return ++send_seq_; }

    // Per-session strictly-increasing replay guard: accepts iff seq > current high-water mark, and
    // advances the mark on acceptance. Same semantics as the pre-ADR-040 recv_high_ map entry.
    [[nodiscard]] bool accept_recv_seq(std::uint64_t seq) noexcept {
        if (seq <= recv_high_) return false;
        recv_high_ = seq;
        return true;
    }

    // C5 bookkeeping: count frames sealed under the CURRENT send key, so sweep_rotation (secure_
    // transport.hpp) can trigger a renegotiate once a policy threshold (kMaxFramesPerKey) is reached.
    void note_sealed() noexcept { ++frames_sealed_on_key_; }
    [[nodiscard]] std::uint64_t frames_sealed_on_key() const noexcept { return frames_sealed_on_key_; }

    // --- Cold path: sweep/diagnostics/handshake-completion. Each call is independently thread-safe. -
    [[nodiscard]] NodeId peer() const noexcept { return peer_; }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        std::lock_guard<std::mutex> g(session_mu_);
        return generation_;
    }
    [[nodiscard]] Fingerprint peer_fingerprint() const noexcept {
        std::lock_guard<std::mutex> g(session_mu_);
        return peer_fingerprint_;
    }

    // Phase 6 (ADR-040 C15/C5): when this session was last (re)negotiated — the origin for both the
    // "idle-timer poll" trigger (a session untouched for renegotiate_interval_ns gets rekeyed even with
    // zero application traffic) and diagnostics. Set at construction and on every install_renegotiated.
    [[nodiscard]] std::int64_t renegotiated_at_ns() const noexcept {
        std::lock_guard<std::mutex> g(session_mu_);
        return renegotiated_at_ns_;
    }

    // Installs a freshly (re)negotiated key pair — a fresh handshake means a fresh nonce space, so
    // send_seq_/recv_high_/frames_sealed_on_key_ all reset to 0 under the same lock the hot path uses,
    // making the swap atomic with respect to any in-flight send()/deliver() on this session.
    void install_renegotiated(std::shared_ptr<const Aead> send_cipher,
                               std::shared_ptr<const Aead> recv_cipher, std::uint64_t generation,
                               Fingerprint peer_fingerprint, std::int64_t now_ns) noexcept {
        std::lock_guard<std::mutex> g(session_mu_);
        send_cipher_ = std::move(send_cipher);
        recv_cipher_ = std::move(recv_cipher);
        generation_ = generation;
        peer_fingerprint_ = peer_fingerprint;
        send_seq_ = 0;
        recv_high_ = 0;
        frames_sealed_on_key_ = 0;
        renegotiated_at_ns_ = now_ns;
    }

    // S5: mark this session revoked. Lock-free (atomic) since sweep_revocations scans many sessions
    // and the hot send()/deliver() path checks this first, before taking session_mu().
    void revoke() noexcept { revoked_.store(true, std::memory_order_release); }
    [[nodiscard]] bool revoked() const noexcept { return revoked_.load(std::memory_order_acquire); }

private:
    NodeId peer_;
    mutable std::mutex session_mu_;
    std::shared_ptr<const Aead> send_cipher_;
    std::shared_ptr<const Aead> recv_cipher_;
    std::uint64_t generation_;
    Fingerprint peer_fingerprint_;
    std::uint64_t send_seq_ = 0;
    std::uint64_t recv_high_ = 0;
    std::uint64_t frames_sealed_on_key_ = 0;
    std::atomic<bool> revoked_{false};
    std::int64_t renegotiated_at_ns_ = 0;
};

}  // namespace quark
