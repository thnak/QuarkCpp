// Implements 020-Security §2 (transport security seam) — `SecureTransport`, a DECORATOR over the 010
// `Transport` that provides confidentiality + integrity + replay protection per node↔node connection.
// It composes WITH, does not replace, the 010 transport: it wraps an inner `Transport`, seals every
// outbound frame's payload with the AEAD seam (aead.hpp), and opens + replay-checks every inbound frame
// before handing it to the real receiver.
//
// REPLAY PROTECTION (020 §2): a PER-SESSION, strictly-increasing SEQUENCE NUMBER rides in the AEAD
// ASSOCIATED DATA (so it cannot be altered without breaking the tag) and is also the nonce. The receiver
// tracks the highest seq seen per sender and REJECTS any frame whose seq is not strictly greater — a
// captured-and-replayed frame is dropped. This composes with 017's delivery-layer dedup (it does not
// replace it): 017 dedups at-least-once RETRIES by message id; this rejects an ADVERSARIAL replay at
// the wire, before decode.
//
// ADR-040's REQUIRED FIX (same-session send-path ordering): per-peer state lives in a `PeerSession`
// (peer_session.hpp), each with its OWN mutex. `send()` holds that session's lock across seq-draw +
// AEAD-seal + wire-handoff, so concurrent senders on the SAME session can never have their seq order
// diverge from wire-arrival order (the pre-fix shape only locked the seq-draw). `map_mu_` is a
// SEPARATE, structural-only lock (session lookup/insert) — AEAD seal/open never runs while holding it,
// so one peer's crypto never blocks another peer's session (the F2-revised invariant).
//
// THE HONEST EXCEPTION (020): the AEAD here is the seam. The default `MockCipher` is NOT real crypto
// (aead.hpp banner). ADR-040 wires the production handshake (mutual mTLS auth, per-session directional
// key derivation) and a real AES-128-GCM cipher as a thin adapter over mbedTLS
// (include/quark/adapters/mbedtls/), linked only when a secure cluster is configured. The "plaintext
// dev transport" is simply the inner `Transport` used WITHOUT this wrapper — labeled dev-only, rejected
// under Strict+multi-node (security.hpp `validate_security`).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unordered_set>

#include "quark/core/aead.hpp"
#include "quark/core/audit.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metrics.hpp"
#include "quark/core/peer_session.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"
#include "quark/detail/hash.hpp"
#include "pal/pal.hpp"

namespace quark {

namespace detail {
inline void put_u64_le(std::vector<std::byte>& out, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) out.push_back(static_cast<std::byte>((v >> (b * 8)) & 0xFF));
}
[[nodiscard]] inline std::uint64_t get_u64_le(std::span<const std::byte> in) noexcept {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(in[static_cast<std::size_t>(b)]))
             << (b * 8);
    return v;
}
}  // namespace detail

class SecureTransport final : public Transport {
public:
    // Decorate `inner` for THIS node `self`, sealing/opening with `cipher` (the AEAD seam). `cipher`
    // must outlive the transport. Pre-handshake (Phase 3/4 wiring), every peer's PeerSession shares
    // this one non-owned cipher for both directions — an aliasing shared_ptr with a no-op deleter lets
    // PeerSession hold a shared_ptr without taking ownership. Once the mTLS handshake is wired, each
    // PeerSession gets its own directional pair from `HandshakeEngine::Result` instead.
    SecureTransport(Transport& inner, const Aead& cipher, NodeId self) noexcept
        : inner_(&inner),
          self_(self),
          shared_cipher_(&cipher, [](const Aead*) noexcept {}) {}

    // Optional audit sink for tamper/replay drops (cold path).
    void set_audit_sink(AuditSink sink) noexcept { audit_ = sink; }

    // ADR-040 Phase 8: optionally feed handshake/rotation/revocation events into the 009 metrics
    // surface (metrics.hpp's ShardCounters::security_*). There is one SecureTransport per NODE, not
    // one per shard — wire this to whichever ShardCounters block the node attributes node-level events
    // to (typically shard 0); the registry's aggregate-on-scrape sum stays correct as long as exactly
    // one shard's block receives them. Unset (default) costs one null check per event — off the hot
    // message path either way (handshakes/rotations/revocations are all cold-path events).
    void set_metrics(ShardCounters& sc) noexcept { metrics_ = &sc; }

    using ClockFn = std::int64_t (*)(void* ctx) noexcept;  // mirrors SwimMembership::ClockFn (021)
    void set_clock(ClockFn fn, void* ctx) noexcept {
        clock_fn_ = fn;
        clock_ctx_ = ctx;
    }

    // ADR-040 Phase 3+: gate this transport on a real mTLS handshake. Once enabled, PeerSessions are
    // no longer auto-vivified on first use (the pre-handshake Phase 2 shape) — a peer must complete a
    // handshake before ANY frame is sent/delivered (S2: "no session ⇒ no delivery"). `identity`/`trust`
    // must outlive this SecureTransport. Call before any send()/inbound traffic for `self`.
    void enable_handshake(HandshakeEngineFactory& factory, ClusterId cluster_id,
                          const IdentityMaterial& identity, const TrustStore& trust) noexcept {
        handshake_factory_ = &factory;
        cluster_id_ = cluster_id;
        identity_ = &identity;
        trust_ = &trust;
    }

    // Phase 6 fallback path (ADR-040 C2): invoked when an in-flight RENEGOTIATION fails — the stale
    // session is dropped and this hook is the only way SecureTransport (which knows nothing about
    // TcpTransport specifically) can force a fresh connection so 021's existing reconnect/backoff
    // machinery redials and a brand-new mTLS handshake carries a fresh generation. Wire this to
    // `[&tcp](NodeId p){ tcp.reset_peer_connection(p); }` when `inner` is a `net::TcpTransport`; leave
    // unset for a transport (e.g. a test double) that has no such concept — a failed renegotiation then
    // simply drops the session and waits for the next application send to re-trigger a fresh handshake.
    void set_reset_hook(std::function<void(NodeId)> fn) noexcept { reset_hook_ = std::move(fn); }

    // The connection this peer's session was negotiated on has died. An mTLS session is
    // per-connection: keep it and the peer (which necessarily lost its own half) silently drops every
    // frame we seal, forever — and the server-role side can never re-initiate. Dropping it here makes
    // the client-role side re-handshake on its next send (S2), exactly as on a cold start. Wire this to
    // `[&secure](NodeId p){ secure.on_peer_disconnected(p); }` via `net::TcpTransport::set_peer_down_hook`.
    void on_peer_disconnected(NodeId peer) {
        {
            std::unique_lock<std::shared_mutex> g(map_mu_);
            sessions_.erase(peer.value);
        }
        {
            std::lock_guard<std::mutex> g(pending_mu_);
            pending_.erase(peer.value);
        }
        sessions_dropped_on_disconnect_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sessions_dropped_on_disconnect() const noexcept {
        return sessions_dropped_on_disconnect_.load(std::memory_order_relaxed);
    }

    // Phase 6 rotation policy (ADR-040 C5/C15). Defaults are conservative; override per-deployment.
    static constexpr std::uint64_t kDefaultMaxFramesPerKey = 1'000'000;                  // C5
    static constexpr std::int64_t kDefaultRenegotiateIntervalNs = 3600LL * 1'000'000'000;  // 1h idle poll (C15)
    void set_rotation_policy(std::uint64_t max_frames_per_key, std::int64_t renegotiate_interval_ns) noexcept {
        max_frames_per_key_ = max_frames_per_key;
        renegotiate_interval_ns_ = renegotiate_interval_ns;
    }

    // Wired by Phase 5's RevocationRegistry: fingerprints checked at handshake time (S3), rejecting a
    // revoked-but-unexpired cert before any session is created — distinct from sweep_revocations
    // (Phase 5), which enforces revocation against sessions that are ALREADY open.
    void set_revoked(std::shared_ptr<const std::unordered_set<Fingerprint>> revoked) noexcept {
        revoked_snapshot_ = std::move(revoked);
    }

    // Phase 5: enforce revocation against sessions that are ALREADY OPEN — distinct from set_revoked()
    // above, which is consulted only at handshake time for NEW sessions (S3). Also refreshes the
    // handshake-time snapshot (so a pending/future handshake for a now-revoked identity is rejected
    // too), so callers only need to invoke this one method per sweep tick with the RevocationRegistry's
    // latest snapshot (wired via SwimMembership::set_sweep_hook). Marking a session revoked() does NOT
    // force-close the underlying TCP socket (S5 is satisfied because the session's own send()/deliver()
    // now drop unconditionally) — the peer's own SWIM keepalive traffic rides this same session, so it
    // silently stops passing keepalives and falls through SWIM's existing suspicion→dead→close cycle.
    void sweep_revocations(std::shared_ptr<const std::unordered_set<Fingerprint>> revoked) {
        revoked_snapshot_ = revoked;
        if (!revoked || revoked->empty()) return;
        std::shared_lock<std::shared_mutex> g(map_mu_);
        for (auto& [id, session] : sessions_) {
            if (session->revoked()) continue;
            if (revoked->contains(session->peer_fingerprint())) {
                session->revoke();
                revocations_enforced_.fetch_add(1, std::memory_order_relaxed);
                if (metrics_) metrics_->security_revocations_enforced.inc();
            }
        }
    }

    // Seal the payload and forward. AAD = {from,to,target,msg_type,seq} so a frame replayed to another
    // target / with a mutated header / mutated seq fails the tag. `seq` is the per-peer session counter.
    //
    // ADR-040 required fix: the session's OWN lock spans seq-draw + seal + wire-handoff, so concurrent
    // senders on this SAME session can never have wire-arrival order diverge from seq order. Different
    // peers' sessions never contend with each other (map_mu_, taken only to find/insert, is released
    // before this lock is taken).
    void send(NodeId to, MessageFrame frame) override {
        std::shared_ptr<PeerSession> session;
        if (handshake_factory_) {
            session = try_get_session(to);
            if (!session) {  // S2: no session ⇒ no delivery — kick off a handshake, drop this frame
                ensure_handshake(to);
                handshake_pending_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else {
            session = get_or_create_session(to);
        }
        std::lock_guard<std::mutex> g(session->session_mu());
        if (session->revoked()) {  // S5 (wired in full by Phase 5): a revoked session sends nothing
            ++revoked_dropped_;
            return;
        }
        const std::uint64_t seq = session->next_send_seq();
        const std::uint64_t nonce = nonce_for(self_, to, seq);
        std::vector<std::byte> aad = build_aad(self_, to, frame, seq);

        std::vector<std::byte> envelope;
        detail::put_u64_le(envelope, seq);  // seq travels in the clear (it is the nonce) but is AAD-bound
        session->send_cipher().seal(nonce, std::span<const std::byte>(aad),
                                    std::span<const std::byte>(frame.payload), envelope);
        session->note_sealed();
        frame.payload = std::move(envelope);
        sealed_.fetch_add(1, std::memory_order_relaxed);
        inner_->send(to, std::move(frame));  // cheap enqueue on the inner transport — safe to hold the lock
    }

    // Phase 6: per-sweep-tick rotation policy — three independent triggers, each generalized from the
    // ADR record's proven shapes:
    //   (1) our own identity has rotated PAST this session's grace window (never fires before the window
    //       elapses — C1);
    //   (2) frames sealed under the current key exceed the volume threshold (C5);
    //   (3) the session has gone `renegotiate_interval_ns_` without ANY renegotiate — a timer-driven
    //       poller that rekeys even a fully idle session with zero application traffic (C15).
    // Only the client-role side (local_is_client) actually initiates a renegotiation; the server-role
    // side is driven passively by the peer's ClientHello, exactly like an initial handshake — no new
    // glare-breaking rule is needed (021 spec rec).
    void sweep_rotation(std::int64_t now_ns) {
        if (!handshake_factory_) return;  // rotation needs the handshake seam; no-op if handshakes are off
        const IdentityMaterial::Snapshot snap = identity_->snapshot(now_ns);
        std::vector<NodeId> due;
        {
            std::shared_lock<std::shared_mutex> g(map_mu_);
            for (auto& [id, session] : sessions_) {
                if (session->revoked()) continue;
                const std::uint64_t session_gen = session->generation();
                const bool own_identity_stale =
                    session_gen != snap.generation &&
                    !(snap.previous && session_gen == snap.previous_generation);
                const bool frames_over_budget = session->frames_sealed_on_key() >= max_frames_per_key_;
                const bool idle_timer_due =
                    now_ns - session->renegotiated_at_ns() >= renegotiate_interval_ns_;
                if (own_identity_stale || frames_over_budget || idle_timer_due) due.push_back(session->peer());
            }
        }  // map_mu_ released before driving any handshake — never hold it across inner_->send() (F2-revised)
        for (NodeId peer : due) try_renegotiate(peer);
    }

    // Register the real receiver; interpose the open + replay check.
    void on_receive(std::function<void(MessageFrame)> cb) override {
        receiver_ = std::move(cb);
        inner_->on_receive([this](MessageFrame frame) { this->deliver(std::move(frame)); });
    }

    // ADR-046 cross-node backpressure: SecureTransport is a pure decorator here — it seals/unseals
    // frames but owns no outbound backlog of its own (that lives one layer down, in `inner_`'s own
    // per-peer queue). Forward both calls unchanged rather than duplicating a second congestion
    // table that would track nothing real.
    [[nodiscard]] bool admit_send(NodeId peer) noexcept override { return inner_->admit_send(peer); }
    void mark_congested(NodeId peer, std::int64_t remaining_ns) noexcept override {
        inner_->mark_congested(peer, remaining_ns);
    }

    // --- Test/diagnostic counters -------------------------------------------------------------
    [[nodiscard]] std::uint64_t sealed() const noexcept { return sealed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t opened() const noexcept { return opened_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t replays_rejected() const noexcept {
        return replays_rejected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t tamper_rejected() const noexcept {
        return tamper_rejected_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t revoked_dropped() const noexcept {
        return revoked_dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t revocations_enforced() const noexcept {
        return revocations_enforced_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t renegotiations_attempted() const noexcept {
        return renegotiations_attempted_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t renegotiations_completed() const noexcept {
        return renegotiations_completed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t renegotiations_failed() const noexcept {
        return renegotiations_failed_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t session_count() const noexcept {
        std::shared_lock<std::shared_mutex> g(map_mu_);
        return sessions_.size();
    }
    [[nodiscard]] std::uint64_t handshake_pending_dropped() const noexcept {
        return handshake_pending_dropped_.load(std::memory_order_relaxed);
    }
    // A parked (in-flight) client-role handshake with no progress for this long is abandoned and
    // retried from scratch on the next send/inbound Authenticate frame, instead of parking forever —
    // closes the "a lost opening frame wedges this peer permanently" gap (no timeout previously).
    void set_handshake_timeout(std::int64_t ns) noexcept { handshake_timeout_ns_ = ns; }
    [[nodiscard]] std::uint64_t handshakes_timed_out() const noexcept {
        return handshakes_timed_out_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t pending_handshake_count() const noexcept {
        std::lock_guard<std::mutex> g(pending_mu_);
        return pending_.size();
    }

private:
    [[nodiscard]] std::int64_t now_ns() const noexcept { return clock_fn_(clock_ctx_); }

    // Structural-only: find or lazily create the PeerSession for `peer`. Never held while AEAD seal/
    // open runs (F2-revised) — callers copy the shared_ptr out and lock the SESSION's own mutex instead.
    // Only used in the pre-handshake (Phase 2) shape — once enable_handshake() is called, sessions are
    // created ONLY by a completed handshake (try_get_session below; S2).
    [[nodiscard]] std::shared_ptr<PeerSession> get_or_create_session(NodeId peer) {
        {
            std::shared_lock<std::shared_mutex> g(map_mu_);
            const auto it = sessions_.find(peer.value);
            if (it != sessions_.end()) return it->second;
        }
        std::unique_lock<std::shared_mutex> g(map_mu_);
        auto [it, inserted] = sessions_.try_emplace(peer.value, nullptr);
        if (inserted) {
            it->second = std::make_shared<PeerSession>(peer, shared_cipher_, shared_cipher_,
                                                        /*generation=*/0, Fingerprint{}, now_ns());
        }
        return it->second;
    }

    [[nodiscard]] std::shared_ptr<PeerSession> try_get_session(NodeId peer) const {
        std::shared_lock<std::shared_mutex> g(map_mu_);
        const auto it = sessions_.find(peer.value);
        return it != sessions_.end() ? it->second : nullptr;
    }

    // --- ADR-040 Phase 3: mTLS handshake driving. Glare-free role assignment mirrors cluster.hpp's
    // keep_local_dial rule (both ends independently compute the same predicate from NodeId ordering —
    // no wire negotiation needed); reimplemented locally so this transport-layer file does not take a
    // dependency on the higher-level cluster/membership header. ------------------------------------
    [[nodiscard]] static bool local_is_client(NodeId self, NodeId peer) noexcept {
        return self.value < peer.value;
    }

    void send_authenticate_frame(NodeId peer, std::vector<std::byte> bytes) {
        MessageFrame f{};
        f.from = self_;
        f.to = peer;
        f.kind = FrameKind::Authenticate;
        f.payload = std::move(bytes);
        inner_->send(peer, std::move(f));
    }

    // Returns true iff this was an in-place RENEGOTIATION (an existing session was swapped), false iff
    // it installed a fresh session (including the identity-mismatch reject, which installs nothing).
    // The caller (drive_handshake) uses this to route the ADR-040 Phase 8 metric correctly.
    bool install_session_from_result(NodeId expected_peer, HandshakeEngine::Result result) {
        // Cross-check identity BEFORE touching sessions_ — a mismatch (spoofed/wrong peer, wrong
        // cluster) leaves nothing behind to clean up (closes S6's leak-on-rejection by construction, not
        // by a follow-up eviction).
        if (!(result.peer_node_id == expected_peer) || !(result.peer_cluster_id == cluster_id_)) {
            audit_(AuditRecord{AuditKind::AuthnFailure, errc::validation, expected_peer.value,
                               "secure transport: handshake identity mismatch (NodeId/ClusterId)", 0});
            return false;
        }
        // S2 guarantees sessions_ is populated ONLY by a completed handshake — so a session that already
        // exists for this peer unambiguously means THIS completed handshake is a Phase 6 RENEGOTIATION,
        // not an initial one, regardless of who initiated it (client-role sweep or server-role response
        // to the peer's ClientHello). Swap in place under the session's OWN lock instead of replacing the
        // map entry — a concurrent send()/deliver() holding the pre-existing shared_ptr transitions onto
        // the fresh cipher pair seamlessly; there is never a window where two different PeerSession
        // objects both claim to be "the" session for this peer.
        if (std::shared_ptr<PeerSession> existing = try_get_session(expected_peer)) {
            existing->install_renegotiated(std::move(result.send_cipher), std::move(result.recv_cipher),
                                           identity_->generation(), result.peer_fingerprint, now_ns());
            renegotiations_completed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        auto session = std::make_shared<PeerSession>(expected_peer, std::move(result.send_cipher),
                                                      std::move(result.recv_cipher),
                                                      identity_->generation(), result.peer_fingerprint,
                                                      now_ns());
        std::unique_lock<std::shared_mutex> g(map_mu_);
        sessions_[expected_peer.value] = std::move(session);
        return false;
    }

    // ADR-040 Phase 6, C2: client-role-only renegotiation trigger (same glare-free rule as an initial
    // handshake — only the lower NodeId renegotiates; the peer's server-role engine responds passively
    // via the ordinary handle_authenticate_frame path below, which now finds an EXISTING session and
    // takes install_session_from_result's in-place-swap branch instead of creating a new one). A no-op
    // if a handshake (initial OR another renegotiate) is already in flight for this peer.
    void try_renegotiate(NodeId peer) {
        if (!local_is_client(self_, peer)) return;
        std::unique_ptr<HandshakeEngine> engine;
        {
            std::lock_guard<std::mutex> g(pending_mu_);
            if (const auto it = pending_.find(peer.value); it != pending_.end()) {
                if (now_ns() - it->second.last_progress_ns < handshake_timeout_ns_) return;
                pending_.erase(it);  // stalled (e.g. a lost opening frame) — abandon it, retry fresh
                handshakes_timed_out_.fetch_add(1, std::memory_order_relaxed);
            }
            engine = handshake_factory_->create(/*is_client=*/true, self_, peer, cluster_id_,
                                                identity_->snapshot(now_ns()), trust_->snapshot(now_ns()),
                                                revoked_snapshot_);
        }
        renegotiations_attempted_.fetch_add(1, std::memory_order_relaxed);
        if (metrics_) metrics_->security_handshakes_attempted.inc();
        drive_handshake(peer, std::move(engine), std::span<const std::byte>{});
    }

    // Advances `engine` with inbound `in`, sends any resulting bytes, and stores/finalizes the outcome.
    // SAFE TO RECURSE for the SAME peer under a synchronous inner transport (e.g. LoopbackTransport in
    // tests): pending_ is only ever touched in short, independent critical sections here — NEVER held
    // across advance() or inner_->send() — so a reentrant inbound Authenticate frame for this same peer
    // (arriving synchronously from send_authenticate_frame below) can never deadlock on pending_mu_.
    void drive_handshake(NodeId peer, std::unique_ptr<HandshakeEngine> engine,
                         std::span<const std::byte> in) {
        std::vector<std::byte> out;
        const HandshakeEngine::Step step = engine->advance(in, out);
        if (step == HandshakeEngine::Step::Done) {
            const bool was_renegotiate = install_session_from_result(peer, engine->take_result());
            if (metrics_) (was_renegotiate ? metrics_->security_rotations_completed
                                           : metrics_->security_handshakes_succeeded)
                              .inc();
            if (!out.empty()) send_authenticate_frame(peer, std::move(out));  // send recurses
            return;
        }
        if (step == HandshakeEngine::Step::Failed) {
            audit_(AuditRecord{AuditKind::AuthnFailure, errc::validation, peer.value,
                               engine->failure_reason(), 0});
            if (metrics_) metrics_->security_handshakes_failed.inc();
            // S2 again supplies the discriminant: a session already existing when a handshake FAILS
            // means this failure came from a Phase 6 RENEGOTIATION attempt (an initial handshake, by
            // construction, happens before any session exists). C2's fallback: drop the now-suspect
            // session and force a fresh connection via reset_hook_ — 021's existing reconnect/backoff
            // redials and a brand-new handshake carries a fresh generation, rather than leaving a
            // session whose peer no longer validates silently in place.
            if (try_get_session(peer)) {
                {
                    std::unique_lock<std::shared_mutex> g(map_mu_);
                    sessions_.erase(peer.value);
                }
                renegotiations_failed_.fetch_add(1, std::memory_order_relaxed);
                if (reset_hook_) reset_hook_(peer);
            }
            return;
        }
        {  // WantWrite / WantRead: store BEFORE sending, so a synchronous reentrant reply finds it.
            std::lock_guard<std::mutex> g(pending_mu_);
            pending_[peer.value] = PendingHandshake{std::move(engine), now_ns()};
        }
        if (!out.empty()) send_authenticate_frame(peer, std::move(out));
    }

    void ensure_handshake(NodeId peer) {
        if (!local_is_client(self_, peer)) return;  // server side waits for the peer's opening message
        std::unique_ptr<HandshakeEngine> engine;
        {
            std::lock_guard<std::mutex> g(pending_mu_);
            if (const auto it = pending_.find(peer.value); it != pending_.end()) {
                if (now_ns() - it->second.last_progress_ns < handshake_timeout_ns_) return;  // already in flight
                pending_.erase(it);  // stalled (e.g. a lost opening frame) — abandon it, retry fresh
                handshakes_timed_out_.fetch_add(1, std::memory_order_relaxed);
            }
            engine = handshake_factory_->create(/*is_client=*/true, self_, peer, cluster_id_,
                                                identity_->snapshot(now_ns()), trust_->snapshot(now_ns()),
                                                revoked_snapshot_);
        }
        if (metrics_) metrics_->security_handshakes_attempted.inc();
        drive_handshake(peer, std::move(engine), std::span<const std::byte>{});
    }

    void handle_authenticate_frame(MessageFrame frame) {
        if (!handshake_factory_) return;  // handshake disabled: ignore a stray Authenticate frame
        const NodeId peer = frame.from;
        std::unique_ptr<HandshakeEngine> engine;
        {
            std::lock_guard<std::mutex> g(pending_mu_);
            const auto it = pending_.find(peer.value);
            if (it != pending_.end()) {
                // A parked engine past the progress deadline is stale (e.g. it sent its opening
                // frame, that frame was lost, and the peer is only now retrying with a FRESH engine)
                // — discard it so the peer's retry meets a fresh server-role engine below, instead of
                // this stale one silently swallowing the retry's frame with a state machine that no
                // longer matches what the peer just sent.
                if (now_ns() - it->second.last_progress_ns < handshake_timeout_ns_)
                    engine = std::move(it->second.engine);
                else
                    handshakes_timed_out_.fetch_add(1, std::memory_order_relaxed);
                pending_.erase(it);
            }
        }
        if (!engine) {  // no engine in flight ⇒ we're the server side seeing the opening message
            engine = handshake_factory_->create(/*is_client=*/false, self_, peer, cluster_id_,
                                                identity_->snapshot(now_ns()), trust_->snapshot(now_ns()),
                                                revoked_snapshot_);
            if (metrics_) metrics_->security_handshakes_attempted.inc();
        }
        drive_handshake(peer, std::move(engine), std::span<const std::byte>(frame.payload));
    }

    void deliver(MessageFrame frame) {
        if (frame.kind == FrameKind::Authenticate) {
            handle_authenticate_frame(std::move(frame));
            return;
        }
        if (frame.payload.size() < 8) {  // malformed: no seq prefix
            tamper_rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t seq = detail::get_u64_le(std::span<const std::byte>(frame.payload.data(), 8));
        const std::span<const std::byte> sealed(frame.payload.data() + 8, frame.payload.size() - 8);
        const std::uint64_t nonce = nonce_for(frame.from, self_, seq);

        // Reconstruct AAD from the RECEIVED header (the exact bytes the sender bound). A mutated header
        // or seq yields a different AAD ⇒ the tag fails below.
        std::vector<std::byte> aad = build_aad(frame.from, self_, frame, seq);

        std::shared_ptr<PeerSession> session;
        if (handshake_factory_) {
            session = try_get_session(frame.from);
            if (!session) {  // S2: no session ⇒ no delivery
                handshake_pending_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else {
            session = get_or_create_session(frame.from);
        }
        std::vector<std::byte> plaintext;
        {
            std::lock_guard<std::mutex> g(session->session_mu());
            if (session->revoked()) {
                revoked_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!session->recv_cipher().open(nonce, std::span<const std::byte>(aad), sealed, plaintext)) {
                tamper_rejected_.fetch_add(1, std::memory_order_relaxed);
                audit_(AuditRecord{AuditKind::AuthnFailure, errc::serialization, frame.from.value,
                                   "secure transport: AEAD open failed (tamper/wrong key)", 0});
                return;
            }
            // Per-session replay guard: strictly-increasing seq per sender. A replayed frame has seq <=
            // the high-water mark ⇒ reject (composes with 017 dedup; stops an adversarial wire replay).
            if (!session->accept_recv_seq(seq)) {
                replays_rejected_.fetch_add(1, std::memory_order_relaxed);
                audit_(AuditRecord{AuditKind::AuthnFailure, errc::unavailable, frame.from.value,
                                   "secure transport: replayed sequence number rejected", 0});
                return;
            }
        }
        opened_.fetch_add(1, std::memory_order_relaxed);
        frame.payload = std::move(plaintext);
        if (receiver_) receiver_(std::move(frame));
    }

    // A session-scoped nonce: fold the ordered (src,dst) pair with the seq. Both peers derive the same
    // value from the frame's from/to, so open matches seal. Unique per (session, seq) — seq is strictly
    // increasing, so a nonce never repeats within a session.
    [[nodiscard]] static std::uint64_t nonce_for(NodeId src, NodeId dst, std::uint64_t seq) noexcept {
        return detail::splitmix64(detail::hash_combine(src.value, dst.value)) ^ seq;
    }

    // AAD = the routing header the receiver must be able to trust: from, to, target actor, msg type, and
    // the replay seq. Authenticated, not encrypted (routing needs it in the clear).
    [[nodiscard]] static std::vector<std::byte> build_aad(NodeId from, NodeId to,
                                                          const MessageFrame& f, std::uint64_t seq) {
        std::vector<std::byte> aad;
        detail::put_u64_le(aad, from.value);
        detail::put_u64_le(aad, to.value);
        detail::put_u64_le(aad, f.target.type.value);
        detail::put_u64_le(aad, f.target.key);
        detail::put_u64_le(aad, f.msg_type.value);
        detail::put_u64_le(aad, seq);
        return aad;
    }

    static std::int64_t real_steady_ns(void*) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch())
            .count();
    }

    Transport* inner_;
    NodeId self_;
    std::shared_ptr<const Aead> shared_cipher_;  // aliasing shared_ptr over cipher_ (non-owning)
    std::function<void(MessageFrame)> receiver_;
    AuditSink audit_{};
    ShardCounters* metrics_ = nullptr;  // ADR-040 Phase 8; see set_metrics() banner

    ClockFn clock_fn_ = &real_steady_ns;
    void* clock_ctx_ = nullptr;

    // ADR-040 Phase 3: null (handshake disabled, Phase 2 auto-vivify shape) unless enable_handshake()
    // was called. identity_/trust_ are non-owned; caller-guaranteed to outlive this SecureTransport.
    HandshakeEngineFactory* handshake_factory_ = nullptr;
    ClusterId cluster_id_{};
    const IdentityMaterial* identity_ = nullptr;
    const TrustStore* trust_ = nullptr;
    std::shared_ptr<const std::unordered_set<Fingerprint>> revoked_snapshot_;

    // ADR-040 Phase 6: rotation/renegotiate policy + the reset-hook fallback (set_reset_hook banner above).
    std::function<void(NodeId)> reset_hook_;
    std::uint64_t max_frames_per_key_ = kDefaultMaxFramesPerKey;
    std::int64_t renegotiate_interval_ns_ = kDefaultRenegotiateIntervalNs;

    // Structural-only lock: session lookup/insert. Never held while AEAD seal/open runs — see the
    // required-fix banner above.
    mutable std::shared_mutex map_mu_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PeerSession>> sessions_;

    // Handshakes in flight, keyed by peer. Touched only in short, independent critical sections (see
    // drive_handshake's banner) — never held across advance()/inner_->send().
    mutable std::mutex pending_mu_;
    struct PendingHandshake {
        std::unique_ptr<HandshakeEngine> engine;
        std::int64_t last_progress_ns = 0;
    };
    std::unordered_map<std::uint64_t, PendingHandshake> pending_;
    // A parked handshake with no progress for this long is abandoned and retried (set_handshake_timeout
    // banner above) — closes the "a lost opening frame wedges this peer permanently" gap.
    std::int64_t handshake_timeout_ns_ = 5'000'000'000;  // 5s

    std::atomic<std::uint64_t> sealed_{0};
    std::atomic<std::uint64_t> opened_{0};
    std::atomic<std::uint64_t> replays_rejected_{0};
    std::atomic<std::uint64_t> tamper_rejected_{0};
    std::atomic<std::uint64_t> revoked_dropped_{0};
    std::atomic<std::uint64_t> handshake_pending_dropped_{0};
    std::atomic<std::uint64_t> handshakes_timed_out_{0};
    std::atomic<std::uint64_t> sessions_dropped_on_disconnect_{0};
    std::atomic<std::uint64_t> revocations_enforced_{0};
    std::atomic<std::uint64_t> renegotiations_attempted_{0};
    std::atomic<std::uint64_t> renegotiations_completed_{0};
    std::atomic<std::uint64_t> renegotiations_failed_{0};
};

}  // namespace quark
