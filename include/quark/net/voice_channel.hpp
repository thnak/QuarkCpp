// VoiceChannel — Shared-Loop, Capability-Placed UDP Datagram Channel (028). NEW sibling seam to
// Transport (010) — NOT a Transport, NOT a decorator of one. Carries best-effort, unordered,
// unreliable UDP voice/media datagrams between player and relay nodes that live in the SAME Quark
// cluster, sharing TcpTransport's already-open pal::IoContext epoll reactor (via the one additive
// TcpTransport::event_loop() accessor) instead of a second thread/loop/fd-set. See ADR-030 and 028
// for the full design record and rationale.
//
// Load-bearing design points (each is a MUST — see 028):
//   - PeerSessionTable vs RouteTable split: per-peer crypto/replay state (send_seq/recv_high) lives in
//     PeerSessionTable, keyed by NodeId, scoped to the session's lifetime — NEVER inside the freely-
//     rebuilt topology snapshot (`routes_`). Mixing the two reintroduces nonce reuse across a relay-set
//     rebuild (ADR-030 S2r).
//   - The relay-side io-loop-thread-only maps (learned_players_/room_members_/last_seen_ns_) are
//     mutated ONLY by code that runs on the io-loop thread: on_readable() directly, and the
//     idle-timeout sweep, which is scheduled exclusively via IoContext::post_after() as a
//     self-rescheduling timer — never a second ambient thread.
//   - Budgeted drain / budgeted fan-out (kMaxDatagramsPerWakeup, kMaxFanoutPerDatagram) is a required
//     invariant, not a tuning suggestion: on_readable() drains at most kMaxDatagramsPerWakeup datagrams
//     per epoll wakeup, and fans a received datagram out to at most kMaxFanoutPerDatagram room members
//     inline; any excess is serviced via a bounded, chained IoContext::post() continuation — never
//     unbounded synchronous work on the shared loop. ADR-030's F2r negative control proved the
//     unbudgeted variant fully starves the shared loop's other fds (SWIM/control-plane) at P>=64.
//   - LIFETIME: every self-rescheduling/chained continuation (idle_sweep, on_readable's budget-overflow
//     repost, fanout_continuation) captures a std::weak_ptr<State>, never a raw `this`. A VoiceChannel
//     can be destroyed while sharing a longer-lived IoContext (the whole point of the shared-loop
//     design — TcpTransport's loop outlives any one VoiceChannel that might be torn down/rebound); a
//     still-pending continuation against an expired weak_ptr silently no-ops instead of touching freed
//     memory (found under TSan as a real use-after-free during ADR-030's proof pass).
//
// Deliberate simplifications for v1 (documented, not silent — see 028 residual risks):
//   - kVoiceReplicationK == 1: RouteTable degenerates to "the live VirtualBins snapshot restricted to
//     the voice-relay-eligible node subset" — O(1)/N-independent via VirtualBins::owner_of, so no
//     separate per-room map is needed on the hot path at all. K>1 is future work.
//   - RoomId is modeled as ActorId{kRoomTypeKey, room.value} so it can ride VirtualBins verbatim
//     (unmodified cluster_topology.hpp) — no new placement machinery.
//   - Security is token-possession-gated AEAD (an HKDF-derivable sub-key placeholder over the existing
//     020 session material), not a richer per-member ACL — see 028 §Security.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pal/net.hpp"
#include "quark/core/aead.hpp"
#include "quark/core/capabilities.hpp"
#include "quark/core/cluster.hpp"           // Endpoint, NodeId
#include "quark/core/cluster_topology.hpp"  // VirtualBins (unmodified) — the placement oracle we reuse
#include "quark/core/ids.hpp"
#include "quark/detail/hash.hpp"

namespace quark::net {

inline constexpr std::size_t kVoiceMtu = 1200;
// room(8) + seq(8) + via(8) + origin(8), sent as AEAD AAD. `via` is the PHYSICAL immediate sender of
// THIS datagram (what the nonce/PeerSession/replay state are scoped to — a relay re-sealing a fanned-
// out frame stamps its OWN id here, matching the physical relay<->listener channel the nonce is
// derived over). `origin` is the LOGICAL speaker (who the audio actually came from) and is preserved
// unchanged across a relay hop, so a listener always learns who is really talking, not "the relay" —
// without this split every relayed frame would misattribute its speaker to the relay.
inline constexpr std::size_t kVoiceHeaderSize = 32;
inline constexpr std::size_t kVoiceTagSize = 16;     // reserved AEAD tag budget (MockCipher uses 8)
inline constexpr std::size_t kVoiceMaxPayload = kVoiceMtu - kVoiceHeaderSize - kVoiceTagSize;
inline constexpr std::uint32_t kVoiceReplicationK = 1;  // v1 default; K>1 is documented future work

// F2r/F3r budgets: bound worst-case synchronous work per epoll wakeup / per received datagram so a
// large room can never starve SWIM/control-plane dispatch sharing the same IoContext.
inline constexpr std::uint32_t kMaxDatagramsPerWakeup = 32;
inline constexpr std::uint32_t kMaxFanoutPerDatagram = 32;

inline constexpr std::int64_t kIdleSweepIntervalNs = 200'000'000;  // 200ms control-plane cadence
inline constexpr std::int64_t kIdleTimeoutNs = 2'000'000'000;      // 2s of silence -> evict

inline constexpr TypeKey kRoomTypeKey{0x564F'4943'4552'4D31ULL};  // "VOICERM1" — RoomId's ActorId tag

struct RoomId {
    std::uint64_t value = 0;
    friend constexpr bool operator==(RoomId, RoomId) = default;
};
using VoicePeer = Endpoint;  // reused verbatim — no new locator type

[[nodiscard]] inline ActorId room_actor_id(RoomId r) noexcept { return ActorId{kRoomTypeKey, r.value}; }

// --- PeerSessionTable (fixes S2r) --------------------------------------------------------------
// Crypto/replay state, keyed by NodeId, scoped to the SecureTransport session's lifetime: created
// once at session establishment (player: when it first resolves a relay; relay: learn-on-first-
// receive), destroyed only at explicit teardown. NEVER touched by a RouteTable rebuild.
struct alignas(64) PeerSession {
    NodeId peer{};
    VoicePeer addr{};
    std::array<std::byte, 32> voice_key{};  // HKDF sub-key placeholder (real key material is 020's)
    alignas(64) std::atomic<std::uint64_t> send_seq{0};    // many-writer cache line (S1)
    alignas(64) std::atomic<std::uint64_t> recv_high{0};   // single-writer (io-loop thread), relaxed
};

class PeerSessionTable {
public:
    // Structural mutation (insert) is rare (control-plane rate); the mutex guards ONLY the map's
    // bookkeeping. Once returned, a PeerSession* is stable for the life of the entry (unique_ptr
    // indirection survives rehashing) — hot-path atomics on it never need the lock.
    PeerSession* find_or_create(NodeId n, VoicePeer addr) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = sessions_.find(n.value);
        if (it != sessions_.end()) return it->second.get();
        auto slot = std::make_unique<PeerSession>();
        slot->peer = n;
        slot->addr = addr;
        PeerSession* raw = slot.get();
        sessions_.emplace(n.value, std::move(slot));
        return raw;
    }
    [[nodiscard]] PeerSession* find(NodeId n) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = sessions_.find(n.value);
        return it == sessions_.end() ? nullptr : it->second.get();
    }
    void erase(NodeId n) {
        std::lock_guard<std::mutex> g(mu_);
        sessions_.erase(n.value);
    }
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return sessions_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::unique_ptr<PeerSession>> sessions_;
};

// Build the voice-relay-eligible VirtualBins snapshot from a CapabilityView — pure function of
// (Flag{"voice-relay"} content), independent of insertion order (claim C1). K==1 default: the bin
// table itself IS the route table, no separate per-room map needed.
[[nodiscard]] inline std::shared_ptr<const VirtualBins> build_voice_route_snapshot(
    const CapabilityView& view) {
    std::vector<NodeId> eligible;
    for (NodeId n : view.nodes())
        if (view.capabilities_of(n).has_flag("voice-relay")) eligible.push_back(n);
    if (eligible.empty()) return nullptr;
    const std::uint64_t buckets = virtual_bin_count(eligible.size());
    return std::make_shared<const VirtualBins>(eligible, buckets);
}

// C5: mirrors 026's `B >= 16*max_nodes` startup-validation precedent — catches "R approaching N" at
// config time instead of letting BoundedPartialView's O(log R)/O(R^2) relay-mesh bound silently
// degrade toward O(log N)/O(N^2) at runtime. threshold==0 -> max(8, ceil(sqrt(total_nodes))).
[[nodiscard]] inline bool voice_relay_ratio_ok(std::size_t relay_count, std::size_t total_nodes,
                                               std::size_t threshold = 0) noexcept {
    std::size_t thresh = threshold;
    if (thresh == 0) {
        std::size_t s = 1;
        while (s * s < total_nodes) ++s;  // ceil(sqrt(total_nodes)), integer, no <cmath> dependency
        thresh = s > 8 ? s : 8;
    }
    return relay_count <= thresh;
}

// ================================================================================================
// VoiceChannel — a thin handle over a heap-allocated, shared_ptr-owned State. Every loop-thread
// continuation captures a std::weak_ptr<State> (never a raw `this`), so a VoiceChannel destroyed
// while continuations are still pending on the shared IoContext is SAFE: the next firing finds the
// weak_ptr expired and no-ops, instead of touching freed memory (the lifetime bug this comment block
// documents, found under TSan while proving S1/S2r — see ADR-030).
// ================================================================================================
class VoiceChannel {
public:
    using DatagramCb = std::function<void(RoomId, NodeId, std::span<const std::byte>)>;

    VoiceChannel(pal::IoContext& loop, NodeId self, const Aead& cipher)
        : st_(std::make_shared<State>(loop, self, cipher)) {}

    VoiceChannel(const VoiceChannel&) = delete;
    VoiceChannel& operator=(const VoiceChannel&) = delete;

    // Best-effort synchronous teardown: removes the fd from the shared loop and marks the state
    // closed so any ALREADY-QUEUED continuation (idle_sweep, on_readable repost, fanout overflow)
    // no-ops on its next firing instead of touching a half-torn-down object. Safe to skip (the
    // weak_ptr mechanism alone is memory-safe); this just stops new epoll events promptly.
    ~VoiceChannel() { st_->close(); }

    bool bind(std::uint64_t addr, std::uint16_t port) { return st_->bind(addr, port); }
    void on_datagram(DatagramCb cb) { st_->on_datagram(std::move(cb)); }
    void on_capability_view_changed(const CapabilityView& view) { st_->on_capability_view_changed(view); }
    PeerSession* ensure_peer_session(NodeId peer, VoicePeer addr) { return st_->ensure_peer_session(peer, addr); }
    void send(RoomId room, std::span<const std::byte> payload) noexcept { st_->send(room, payload); }

    [[nodiscard]] std::uint64_t sent() const noexcept { return st_->sent(); }
    [[nodiscard]] std::uint64_t received() const noexcept { return st_->received(); }
    [[nodiscard]] std::uint64_t drop_wb() const noexcept { return st_->drop_wb(); }
    [[nodiscard]] std::uint64_t drop_stale() const noexcept { return st_->drop_stale(); }
    [[nodiscard]] std::uint64_t drop_oversize() const noexcept { return st_->drop_oversize(); }
    [[nodiscard]] std::uint64_t drop_no_route() const noexcept { return st_->drop_no_route(); }
    [[nodiscard]] std::uint64_t drop_no_session() const noexcept { return st_->drop_no_session(); }
    [[nodiscard]] std::uint64_t post_continuations() const noexcept { return st_->post_continuations(); }
    [[nodiscard]] std::uint64_t thread_violations() const noexcept { return st_->thread_violations(); }
    [[nodiscard]] std::thread::id loop_thread_id() const noexcept { return st_->loop_thread_id(); }
    [[nodiscard]] pal::fd_t fd() const noexcept { return st_->fd(); }
    [[nodiscard]] std::size_t learned_players_count() const noexcept { return st_->learned_players_count(); }
    [[nodiscard]] PeerSessionTable& sessions() noexcept { return st_->sessions(); }

    void test_add_room_member(RoomId room, NodeId member) { st_->test_add_room_member(room, member); }
    void test_learn_player(NodeId n, VoicePeer addr) { st_->test_learn_player(n, addr); }
    [[nodiscard]] std::vector<std::byte> test_seal_frame(RoomId room, NodeId from, std::uint64_t seq,
                                                          std::span<const std::byte> payload) const {
        return st_->test_seal_frame(room, from, seq, payload);
    }
    void test_feed_datagram(std::span<const std::byte> raw, std::uint64_t from_addr, std::uint16_t from_port) {
        st_->test_feed_datagram(raw, from_addr, from_port);
    }

private:
    // ============================================================================================
    // State — everything a loop-thread continuation might touch. Managed exclusively via
    // shared_ptr/weak_ptr from here down; NEVER captured by raw pointer in a lambda handed to
    // IoContext::post/post_after.
    // ============================================================================================
    class State : public std::enable_shared_from_this<State> {
    public:
        State(pal::IoContext& loop, NodeId self, const Aead& cipher) : io_(loop), self_(self), cipher_(&cipher) {}

        // Blocking marshal-onto-loop helper: posts `fn` (taking no args) onto `io`, waits (bounded) for
        // it to run. The sync primitives (mutex/cv/done) are shared_ptr-owned and captured BY VALUE in
        // the posted lambda — NOT by reference to this call's stack frame — so there is no destroy-
        // while-notifying race even in the pathological case where the waiter's wait_for() times out (or
        // the classic unlock-then-notify race TSan otherwise flags: the notifier may still be inside
        // notify_one() when the waiter's stack-local cv would otherwise already be destructing).
        template <class Fn>
        static void blocking_post(pal::IoContext& io, Fn&& fn) {
            auto m = std::make_shared<std::mutex>();
            auto cv = std::make_shared<std::condition_variable>();
            auto done = std::make_shared<std::atomic<bool>>(false);
            io.post([fn = std::forward<Fn>(fn), m, cv, done]() mutable {
                fn();
                { std::lock_guard<std::mutex> lg(*m); done->store(true); }
                cv->notify_one();
            });
            std::unique_lock<std::mutex> lk(*m);
            cv->wait_for(lk, std::chrono::seconds(2), [&] { return done->load(); });
        }

        bool bind(std::uint64_t addr, std::uint16_t port) {
            auto fd = pal::udp_socket();
            if (!fd) return false;
            if (auto r = pal::udp_bind(*fd, addr, port); !r) {
                pal::close_fd(*fd);
                return false;
            }
            const pal::fd_t f = *fd;
            std::weak_ptr<State> weak = weak_from_this();
            blocking_post(io_, [weak, f] {
                auto self = weak.lock();
                if (!self) return;
                self->loop_thread_id_ = std::this_thread::get_id();
                self->fd_.store(f, std::memory_order_release);
                // NOTE: this fd handler captures a RAW `State*`, not a weak_ptr — deliberately, and
                // ONLY because pal::IoContext::run() COPIES the registered handler on EVERY dispatch
                // (`ReadyHandler h = it->second;`, unmodified PAL code, reused verbatim) before
                // invoking it: a weak_ptr capture is not std::function-SBO-eligible in libstdc++ (its
                // refcounted copy ctor disqualifies the "location invariant" fast path), so it would
                // heap-allocate ONE TIME PER RECEIVED DATAGRAM — the exact regression this comment
                // documents (found empirically: F1's zero-alloc claim broke after an earlier weak_ptr-
                // everywhere lifetime fix; root-caused to this specific copy-on-dispatch site). Safety
                // for the raw pointer is instead established by close() BLOCKING until del_fd() has
                // actually removed this handler from the registry before returning — see close().
                State* raw = self.get();
                self->io_.add_fd(f, EPOLLIN, [raw](std::uint32_t) { raw->on_readable(); });
                self->arm_idle_sweep();
            });
            return true;
        }

        // Synchronously removes the fd from the loop's handler registry BEFORE returning — this is
        // what makes the raw `State*` captured by the fd handler above safe: by the time close()
        // returns, pal::IoContext::run() can never again look up (let alone copy-and-invoke) this
        // fd's handler, so it is safe for VoiceChannel's destructor to let State's members start
        // tearing down immediately afterward. Idempotent; a no-op if never bound.
        //
        // BOUNDED wait, not unbounded: if the shared loop was ALREADY stopped (run() returned) before
        // this VoiceChannel is destroyed — e.g. a test/shutdown sequence that stops the transport's
        // loop first — the posted del_fd task would never be drained, so an unconditional wait would
        // deadlock forever. A real deployment tears down per-session VoiceChannels WHILE the node's
        // transport loop is still running (the loop outlives any one voice session, by construction of
        // the shared-loop design) — this timeout is a defensive fallback for that corner case, not the
        // expected path with a live loop.
        void close() {
            closed_.store(true, std::memory_order_release);
            const pal::fd_t f = fd_.load(std::memory_order_acquire);
            if (f == pal::invalid_fd) return;
            pal::IoContext& io = io_;  // capture the reference itself, not `this` — no lifetime dependency
            blocking_post(io, [f, &io] { io.del_fd(f); });
        }

        void on_datagram(DatagramCb cb) { on_datagram_ = std::move(cb); }

        void on_capability_view_changed(const CapabilityView& view) {
            auto fresh = build_voice_route_snapshot(view);
            std::lock_guard<std::mutex> g(routes_mu_);
            routes_ = std::move(fresh);
        }

        PeerSession* ensure_peer_session(NodeId peer, VoicePeer addr) {
            return sessions_.find_or_create(peer, addr);
        }

        void send(RoomId room, std::span<const std::byte> payload) noexcept {
            if (payload.size() > kVoiceMaxPayload) {
                drop_oversize_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::shared_ptr<const VirtualBins> snap;
            {
                std::lock_guard<std::mutex> g(routes_mu_);
                snap = routes_;
            }
            if (!snap) {
                drop_no_route_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto owner = snap->owner_of(room_actor_id(room));
            if (!owner) {
                drop_no_route_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            PeerSession* sess = sessions_.find(*owner);
            if (!sess) {
                drop_no_session_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const pal::fd_t fd = fd_.load(std::memory_order_acquire);
            if (fd == pal::invalid_fd) return;
            send_sealed(fd, *sess, room, self_, payload);  // originating a fresh frame: origin == self
        }

        [[nodiscard]] std::uint64_t sent() const noexcept { return sent_.load(); }
        [[nodiscard]] std::uint64_t received() const noexcept { return received_.load(); }
        [[nodiscard]] std::uint64_t drop_wb() const noexcept { return drop_wb_.load(); }
        [[nodiscard]] std::uint64_t drop_stale() const noexcept { return drop_stale_.load(); }
        [[nodiscard]] std::uint64_t drop_oversize() const noexcept { return drop_oversize_.load(); }
        [[nodiscard]] std::uint64_t drop_no_route() const noexcept { return drop_no_route_.load(); }
        [[nodiscard]] std::uint64_t drop_no_session() const noexcept { return drop_no_session_.load(); }
        [[nodiscard]] std::uint64_t post_continuations() const noexcept { return post_continuations_.load(); }
        [[nodiscard]] std::uint64_t thread_violations() const noexcept { return thread_violations_.load(); }
        [[nodiscard]] std::thread::id loop_thread_id() const noexcept { return loop_thread_id_; }
        [[nodiscard]] pal::fd_t fd() const noexcept { return fd_.load(std::memory_order_acquire); }
        [[nodiscard]] std::size_t learned_players_count() const noexcept { return learned_players_.size(); }
        [[nodiscard]] PeerSessionTable& sessions() noexcept { return sessions_; }

        // Both self-marshal onto the io-loop thread (blocking) — these touch the SAME io-loop-thread-
        // only maps (S3r) that on_readable()/idle_sweep() own, so a test calling them concurrently with
        // real traffic (not just at setup, before any traffic flows) must not race those maps. Found
        // empirically under TSan: an earlier version mutated room_members_ directly from the calling
        // (test) thread, racing on_readable() running concurrently on the loop thread — a bug in the
        // TEST HOOK's own discipline, not in VoiceChannel's production code path (on_readable/
        // idle_sweep never touch these maps except via the loop-thread-only entry points).
        void test_add_room_member(RoomId room, NodeId member) {
            blocking_post(io_, [this, room, member] { room_members_[room.value].push_back(member); });
        }
        void test_learn_player(NodeId n, VoicePeer addr) {
            blocking_post(io_, [this, n, addr] { learned_players_find_or_create(n, addr); });
        }

        [[nodiscard]] std::vector<std::byte> test_seal_frame(RoomId room, NodeId from, std::uint64_t seq,
                                                              std::span<const std::byte> payload) const {
            // Simulates a frame arriving DIRECTLY from `from` (not via a relay hop), so via == origin —
            // the same relationship send()'s own call site uses for a freshly-originated frame.
            std::array<std::byte, kVoiceMtu> scratch;
            std::size_t off = 0;
            put_u64_le(scratch, off, room.value);
            put_u64_le(scratch, off, seq);
            put_u64_le(scratch, off, from.value);  // via
            put_u64_le(scratch, off, from.value);  // origin
            const std::span<const std::byte> aad(scratch.data(), kVoiceHeaderSize);
            const std::uint64_t nonce = quark::detail::hash_combine(from.value ^ self_.value, seq);
            const std::size_t sealed_len = cipher_->seal_into(
                nonce, aad, payload, std::span<std::byte>(scratch.data() + off, scratch.size() - off));
            return std::vector<std::byte>(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(off + sealed_len));
        }

        void test_feed_datagram(std::span<const std::byte> raw, std::uint64_t from_addr, std::uint16_t from_port) {
            std::vector<std::byte> copy(raw.begin(), raw.end());
            std::weak_ptr<State> weak = weak_from_this();
            blocking_post(io_, [weak, copy = std::move(copy), from_addr, from_port]() mutable {
                if (auto self = weak.lock()) self->process_datagram(copy, from_addr, from_port);
            });
        }

    private:
        void arm_idle_sweep() {
            std::weak_ptr<State> weak = weak_from_this();
            io_.post_after(kIdleSweepIntervalNs, [weak] {
                if (auto self = weak.lock()) self->idle_sweep();
            });
        }

        // `origin` is stamped into the wire header UNCHANGED from whatever the caller supplies — send()
        // passes self_ (a freshly-originated frame), the fan-out paths pass the origin they parsed off
        // the incoming frame (preserving the true speaker through a relay hop). `via` (this physical
        // sender, always self_) and the nonce derivation are untouched by that choice — the nonce stays
        // scoped to the physical (self_, sess.peer) channel, matching S2r's proven per-PeerSession
        // replay-state scoping, regardless of who `origin` says is logically speaking.
        void send_sealed(pal::fd_t fd, PeerSession& sess, RoomId room, NodeId origin,
                         std::span<const std::byte> payload) noexcept {
            thread_local std::array<std::byte, kVoiceMtu> scratch;  // per-thread, reused, no alloc
            const std::uint64_t seq = sess.send_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            std::size_t off = 0;
            put_u64_le(scratch, off, room.value);
            put_u64_le(scratch, off, seq);
            put_u64_le(scratch, off, self_.value);  // via: the physical sender of this datagram
            put_u64_le(scratch, off, origin.value);  // origin: the logical speaker, preserved as-is
            const std::span<const std::byte> aad(scratch.data(), kVoiceHeaderSize);
            const std::uint64_t nonce = quark::detail::hash_combine(self_.value ^ sess.peer.value, seq);
            const std::size_t sealed_len = cipher_->seal_into(
                nonce, aad, payload, std::span<std::byte>(scratch.data() + off, scratch.size() - off));
            auto r = pal::udp_send_to(fd, sess.addr.addr, sess.addr.port,
                                      std::span<const std::byte>(scratch.data(), off + sealed_len));
            if (!r)
                drop_wb_.fetch_add(1, std::memory_order_relaxed);
            else
                sent_.fetch_add(1, std::memory_order_relaxed);
        }

        void assert_on_loop_thread() noexcept {
            if (std::this_thread::get_id() != loop_thread_id_)
                thread_violations_.fetch_add(1, std::memory_order_relaxed);
        }

        void on_readable() {
            if (closed_.load(std::memory_order_acquire)) return;
            assert_on_loop_thread();
            for (std::uint32_t drained = 0; drained < kMaxDatagramsPerWakeup; ++drained) {
                std::array<std::byte, kVoiceMtu> in;
                std::uint64_t from_addr = 0;
                std::uint16_t from_port = 0;
                auto r = pal::udp_recv_from(fd_.load(std::memory_order_relaxed), in.data(), in.size(),
                                            from_addr, from_port);
                if (!r) {
                    if (r.error() == pal::would_block()) return;  // drained — normal loop exit
                    continue;                                     // transient error: drop this datagram
                }
                process_datagram(std::span<const std::byte>(in.data(), *r), from_addr, from_port);
            }
            // Budget exhausted but more may be pending: yield to the reactor (let SWIM/other fds run
            // this iteration) via a bounded, chained continuation instead of draining unboundedly.
            post_continuations_.fetch_add(1, std::memory_order_relaxed);
            std::weak_ptr<State> weak = weak_from_this();
            io_.post([weak] { if (auto self = weak.lock()) self->on_readable(); });
        }

        void process_datagram(std::span<const std::byte> in, std::uint64_t from_addr,
                              std::uint16_t from_port) {
            if (closed_.load(std::memory_order_acquire)) return;
            assert_on_loop_thread();
            if (in.size() < kVoiceHeaderSize + 1) {
                drop_stale_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::size_t off = 0;
            const RoomId room{get_u64_le(in, off)};
            const std::uint64_t seq = get_u64_le(in, off);
            const NodeId via{get_u64_le(in, off)};      // physical immediate sender — session/nonce key
            const NodeId origin{get_u64_le(in, off)};   // logical speaker — preserved through a relay hop

            PeerSession* sess = learned_players_find_or_create(via, VoicePeer{via, from_addr, from_port});

            const std::span<const std::byte> aad(in.data(), kVoiceHeaderSize);
            const std::span<const std::byte> sealed(in.data() + off, in.size() - off);
            const std::uint64_t nonce = quark::detail::hash_combine(via.value ^ self_.value, seq);
            std::array<std::byte, kVoiceMaxPayload> plain;
            std::size_t plain_len = 0;
            if (!cipher_->open_into(nonce, aad, sealed, plain, plain_len)) {
                drop_stale_.fetch_add(1, std::memory_order_relaxed);  // tamper / wrong key
                return;
            }
            const std::uint64_t high = sess->recv_high.load(std::memory_order_relaxed);
            if (seq <= high) {
                drop_stale_.fetch_add(1, std::memory_order_relaxed);  // stale OR replay (S2r)
                return;
            }
            sess->recv_high.store(seq, std::memory_order_relaxed);
            received_.fetch_add(1, std::memory_order_relaxed);
            last_seen_ns_[via.value] = pal::mono_ns();

            auto& members = room_members_[room.value];
            if (std::find(members.begin(), members.end(), via) == members.end()) members.push_back(via);

            if (on_datagram_) on_datagram_(room, origin, std::span<const std::byte>(plain.data(), plain_len));

            // Relay fan-out, budgeted (F2r/F3r): the COMMON case (room size <= kMaxFanoutPerDatagram)
            // fans out entirely INLINE off the `plain` stack array — zero heap allocation (F1). Only
            // when a room exceeds the budget does the overflow tail need to survive past this call's
            // stack frame into a chained io_.post() continuation, which is where (and ONLY where) a
            // heap copy is made — a stated, documented trade-off, not a hot-path cost.
            const std::size_t total = members.size();
            const std::size_t inline_end = std::min(total, static_cast<std::size_t>(kMaxFanoutPerDatagram));
            for (std::size_t i = 0; i < inline_end; ++i) {
                const NodeId member = members[i];
                if (member == via) continue;
                PeerSession* dst = learned_players_find(member);
                if (!dst) continue;
                send_sealed(fd_.load(std::memory_order_relaxed), *dst, room, origin,
                           std::span<const std::byte>(plain.data(), plain_len));
            }
            if (inline_end < total) {
                post_continuations_.fetch_add(1, std::memory_order_relaxed);
                std::vector<std::byte> carried(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(plain_len));
                std::weak_ptr<State> weak = weak_from_this();
                io_.post([weak, room, via, origin, carried = std::move(carried), inline_end]() mutable {
                    if (auto self = weak.lock()) self->fanout_continuation(room, via, origin, std::move(carried), inline_end);
                });
            }
        }

        void fanout_continuation(RoomId room, NodeId via, NodeId origin, std::vector<std::byte> plain,
                                 std::size_t start) {
            if (closed_.load(std::memory_order_acquire)) return;
            assert_on_loop_thread();
            auto mit = room_members_.find(room.value);
            if (mit == room_members_.end()) return;
            const auto& members = mit->second;
            const std::size_t end = std::min(members.size(), start + kMaxFanoutPerDatagram);
            for (std::size_t i = start; i < end; ++i) {
                const NodeId member = members[i];
                if (member == via) continue;
                PeerSession* dst = learned_players_find(member);
                if (!dst) continue;
                send_sealed(fd_.load(std::memory_order_relaxed), *dst, room, origin, plain);
            }
            if (end < members.size()) {
                post_continuations_.fetch_add(1, std::memory_order_relaxed);
                std::weak_ptr<State> weak = weak_from_this();
                io_.post([weak, room, via, origin, plain = std::move(plain), end]() mutable {
                    if (auto self = weak.lock()) self->fanout_continuation(room, via, origin, std::move(plain), end);
                });
            }
        }

        PeerSession* learned_players_find_or_create(NodeId n, VoicePeer addr) {
            auto it = learned_players_.find(n.value);
            if (it != learned_players_.end()) return it->second.get();
            auto slot = std::make_unique<PeerSession>();
            slot->peer = n;
            slot->addr = addr;
            PeerSession* raw = slot.get();
            learned_players_.emplace(n.value, std::move(slot));
            return raw;
        }
        [[nodiscard]] PeerSession* learned_players_find(NodeId n) {
            auto it = learned_players_.find(n.value);
            return it == learned_players_.end() ? nullptr : it->second.get();
        }

        void idle_sweep() {
            if (closed_.load(std::memory_order_acquire)) return;
            assert_on_loop_thread();
            const std::int64_t now = pal::mono_ns();
            for (auto it = last_seen_ns_.begin(); it != last_seen_ns_.end();) {
                if (now - it->second > kIdleTimeoutNs) {
                    const std::uint64_t nid = it->first;
                    learned_players_.erase(nid);
                    for (auto& [room, members] : room_members_)
                        members.erase(std::remove(members.begin(), members.end(), NodeId{nid}), members.end());
                    it = last_seen_ns_.erase(it);
                } else {
                    ++it;
                }
            }
            sweeps_.fetch_add(1, std::memory_order_relaxed);
            arm_idle_sweep();
        }

        static void put_u64_le(std::array<std::byte, kVoiceMtu>& buf, std::size_t& off, std::uint64_t v) {
            for (int i = 0; i < 8; ++i) buf[off + static_cast<std::size_t>(i)] =
                static_cast<std::byte>((v >> (8 * i)) & 0xFF);
            off += 8;
        }
        static std::uint64_t get_u64_le(std::span<const std::byte> buf, std::size_t& off) {
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[off + static_cast<std::size_t>(i)]))
                     << (8 * i);
            off += 8;
            return v;
        }

        pal::IoContext& io_;
        NodeId self_;
        const Aead* cipher_;
        std::atomic<pal::fd_t> fd_{pal::invalid_fd};
        std::thread::id loop_thread_id_{};
        std::atomic<bool> closed_{false};

        std::mutex routes_mu_;
        std::shared_ptr<const VirtualBins> routes_;  // topology-only snapshot (C1/C2), rebuilt freely

        PeerSessionTable sessions_;  // player-side + shared crypto/replay state (S2r)

        // Relay-side, io-loop-thread-only (S3r) — no atomics, no lock, single mutator by construction.
        std::unordered_map<std::uint64_t, std::unique_ptr<PeerSession>> learned_players_;
        std::unordered_map<std::uint64_t, std::vector<NodeId>> room_members_;
        std::unordered_map<std::uint64_t, std::int64_t> last_seen_ns_;

        DatagramCb on_datagram_;

        std::atomic<std::uint64_t> sent_{0}, received_{0}, drop_wb_{0}, drop_stale_{0}, drop_oversize_{0};
        std::atomic<std::uint64_t> drop_no_route_{0}, drop_no_session_{0}, post_continuations_{0}, sweeps_{0};
        std::atomic<std::uint64_t> thread_violations_{0};
    };

    std::shared_ptr<State> st_;
};

}  // namespace quark::net
