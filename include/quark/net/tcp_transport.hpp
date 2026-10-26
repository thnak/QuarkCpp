// Implements 010-Distribution §"Transport seam" DEFAULT transport — plain TCP with length-prefixed
// frames, one multiplexed connection per peer, over the 019 PAL event loop (epoll backend). This is the
// concrete adapter transport.hpp's banner deferred ("the real default transport is plain TCP … that
// adapter is 019/021 work"). It satisfies the `Transport` seam, so DistributedRouter (010), SwimMembership
// (021 control plane, via control_data_demux), and SecureTransport (020 decorator) all sit on it UNCHANGED
// — none of them knows a socket appeared under the seam.
//
// WHAT IT REALIZES:
//   * 010: length-prefixed framing (wire_codec.hpp), one-connection-per-peer, FIFO per (sender,receiver)
//     over that single connection (a single sender's frames are appended to one ordered write buffer and
//     drained in order — the property the loopback double gave by construction).
//   * 021 §2 connection mechanics: LAZY dial on first cross-node send; deterministic lower-NodeId-wins
//     dial DEDUP (dial_winner / keep_local_dial from cluster.hpp — the single-source rule, now realized
//     over real racing sockets); jittered exponential RECONNECT; teardown hook (`close_peer`) SWIM calls
//     when it declares a peer dead. Keepalive is NOT a separate heartbeat here — 021 §"Liveness" reuses
//     SWIM's pings (they flow as Control frames over this same connection), so the transport adds none.
//   * 019: every OS call goes through pal/net.hpp (per-OS backend selected at compile time); NO
//     <sys/*>/<netinet/*>/<winsock2.h> here. Written against `pal::fd_t` (not a raw POSIX `int`), so
//     this file is unchanged between the Linux (epoll) and Windows (WSAPoll) backends.
//
// THREADING: one dedicated I/O thread runs the IoContext loop; ALL connection state lives on it. `send()`
// is thread-safe (marshals the frame onto the loop via IoContext::post); the inbound `on_receive` sink is
// invoked ON the I/O thread (its DistributedRouter/engine target is an ordinary cross-thread producer edge,
// 002/006). Diagnostic counters are atomic so a test thread can read them after a sync point.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pal/net.hpp"
#include "pal/pal.hpp"                // pal::now() — ADR-046 gate's own clock
#include "quark/core/cluster.hpp"    // Endpoint, dial_winner, keep_local_dial (021 §2 — the dedup RULE)
#include "quark/core/governance.hpp"  // PeerCongestionTable (ADR-046 backpressure gate)
#include "quark/core/transport.hpp"  // Transport seam + MessageFrame
#include "quark/detail/hash.hpp"     // splitmix64 (deterministic reconnect jitter — no random_device)
#include "quark/net/wire_codec.hpp"  // encode_frame + FrameStream (the "stream" reassembler)

namespace quark::net {

// The per-connection HELLO record, sent as the first bytes on every fresh connection (before any frame).
// It tells the *accepting* side which NodeId dialed it — the input the dial-dedup rule needs (the dialer
// already knows the peer's id). Fixed layout, LE: [u32 magic][u16 version][u64 node]. Not length-prefixed
// (it is fixed-size); frames begin only after both hellos are exchanged.
inline constexpr std::uint32_t hello_magic = 0x51'52'4B'31u;  // 'Q','R','K','1'
inline constexpr std::uint16_t hello_version = 1;
inline constexpr std::size_t hello_size = 4 + 2 + 8;  // 14 bytes

// Reconnect backoff (021 §"Reconnect with jittered exponential backoff, capped").
inline constexpr std::int64_t reconnect_base_ns = 20'000'000;    // 20 ms
inline constexpr std::int64_t reconnect_cap_ns = 2'000'000'000;  // 2 s ceiling

class TcpTransport final : public Transport {
public:
    // `self` is this node's identity; `bind_addr`/`bind_port` are where this node LISTENS (port 0 ⇒ the
    // OS picks an ephemeral port, readable via listen_port() after start()). Construct → register peer
    // endpoints (add_peer) and on_receive → start().
    //
    // `fallback_timeout_ns` is ADR-048's relay-fallback threshold: 0 (default) = `NoRelayFallback`, this
    // transport behaves byte-identically to before that ADR. A positive value = `DirectThenRelay<TimeoutMs>`
    // (resolve the runtime value via `fallback_timeout_ns_of<...>()`, fallback_policy.hpp) — a peer whose
    // direct connection has not reached `St::Open` within this many nanoseconds of its first `St::Connecting`
    // is redialed through its `set_relay()`-configured relay Endpoint instead.
    TcpTransport(NodeId self, std::uint64_t bind_addr, std::uint16_t bind_port,
                 std::int64_t fallback_timeout_ns = 0) noexcept
        : self_(self), bind_addr_(bind_addr), bind_port_(bind_port),
          fallback_timeout_ns_(fallback_timeout_ns) {}

    ~TcpTransport() override { stop(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Teach the transport where a peer LISTENS, so a lazy dial to it can resolve an address. Fed from
    // 021 Discovery/seeds + SWIM gossip in a full deployment; called directly in a test. Thread-safe
    // before start(); after start() it is marshalled onto the loop.
    // Per-transport single-frame body cap (default net::max_frame_body). Call before start().
    void set_max_frame_body(std::uint32_t n) noexcept {
        max_frame_body_ = n == 0 ? 1 : n;
        if (started_.load(std::memory_order_acquire))
            io_.post([this] {
                for (auto& [id, c] : conns_) c->in.set_max_body(max_frame_body_);
            });
    }

    // Bound unidentified inbound sockets (a pre-auth connection flood). 0 disables the cap.
    void set_max_pending_inbound(std::uint32_t n) noexcept { max_pending_in_ = n; }
    // Per-source-IP cap on unidentified inbound sockets (0 disables) and the deadline for a socket to
    // send its hello (<=0 disables). Call before start().
    void set_max_pending_inbound_per_ip(std::uint32_t n) noexcept { max_pending_per_ip_ = n; }
    void set_pending_hello_timeout_ns(std::int64_t ns) noexcept { pending_hello_timeout_ns_ = ns; }

    void add_peer(Endpoint ep) {
        if (started_.load(std::memory_order_acquire))
            io_.post([this, ep] { peers_[ep.node.value] = ep; });
        else
            peers_[ep.node.value] = ep;
    }

    // ADR-048: designate `relay_ep` as `peer`'s fallback dial target — the Endpoint TcpTransport switches
    // to once `peer`'s direct connection has failed to reach `St::Open` within the constructor's
    // `fallback_timeout_ns` (0 = fallback disabled; this call is then inert — `relays_` is still recorded
    // but never consulted). `relay_ep` must be byte-transparent for `peer` specifically (e.g. a TURN-style
    // per-peer relay allocation): whoever answers on it must send `peer`'s own HELLO, since dial-dedup,
    // frame decode, and delivery all continue completely unchanged over it (the "settled architecture" this
    // ADR did not re-litigate). Producing that transparent relay allocation is separate, deferred work (see
    // ADR-048's non-goals + 021's "Relay transport endpoint fallback" section) — this call only teaches
    // TcpTransport which Endpoint to dial once one exists. Thread-safe, same started_-gated io_.post()
    // idiom as add_peer().
    void set_relay(NodeId peer, Endpoint relay_ep) {
        if (started_.load(std::memory_order_acquire))
            io_.post([this, peer, relay_ep] { relays_[peer.value] = relay_ep; });
        else
            relays_[peer.value] = relay_ep;
    }

    // F1: a bare plaintext TCP transport is DEV-ONLY. It refuses to start unless either an
    // authenticating wrapper (SecureTransport) marked it or the application explicitly opted into
    // insecure dev mode. This is a second, structural gate behind `validate_startup_security`:
    // forgetting that call cannot silently expose a plaintext multi-node cluster.
    void set_authenticated_transport(bool authenticated) noexcept override {
        authenticated_transport_ = authenticated;
    }
    void set_allow_insecure_plaintext(bool allow) noexcept { allow_insecure_plaintext_ = allow; }
    [[nodiscard]] bool plaintext_start_refused() const noexcept { return plaintext_start_refused_; }

    // Bind + listen + spawn the I/O thread. Returns false if the listener could not be opened OR if
    // this is an unauthenticated plaintext transport that was not explicitly opted into dev mode.
    bool start() {
        plaintext_start_refused_ = false;
#ifndef QUARK_ALLOW_INSECURE
        if (!authenticated_transport_ && !allow_insecure_plaintext_) {
            plaintext_start_refused_ = true;  // F1: fail closed; see set_allow_insecure_plaintext()
            return false;
        }
#endif
        auto lfd = pal::tcp_listen(bind_addr_, bind_port_);
        if (!lfd) return false;
        listener_ = *lfd;
        if (auto p = pal::local_port(listener_)) bind_port_ = *p;  // resolve the ephemeral port
        io_.add_fd(listener_, EPOLLIN, [this](std::uint32_t) { on_accept_ready(); });
        started_.store(true, std::memory_order_release);
        io_thread_ = std::thread([this] { io_.run(); });
        return true;
    }

    // Stop the loop, join the thread, close every fd. Idempotent.
    void stop() noexcept {
        if (!started_.exchange(false)) return;
        io_.stop();
        if (io_thread_.joinable()) io_thread_.join();
        // Loop is stopped; tear down sockets on this thread (no handler can run concurrently now).
        for (auto& [id, c] : conns_) retire_fd(c->fd);
        for (auto& [fd, c] : pending_in_) retire_fd(c->fd);
        for (auto& [fd, p] : probes_) retire_fd(fd);                    // ADR-048
        for (auto& [peer, fd] : pending_cutover_) retire_fd(fd);        // ADR-048
        retire_fd(listener_);
        listener_ = pal::invalid_fd;
        conns_.clear();
        pending_in_.clear();
        by_fd_.clear();
        open_marked_.clear();
        reconnect_suppressed_.clear();
        outq_.clear();
        relay_routed_.clear();
        pending_cutover_.clear();
        probes_.clear();
    }

    [[nodiscard]] std::uint16_t listen_port() const noexcept { return bind_port_; }
    [[nodiscard]] NodeId self() const noexcept { return self_; }

    // --- Transport seam -------------------------------------------------------------------------
    void send(NodeId to, MessageFrame frame) override {
        if (to == self_) return;  // never dial ourselves; a self-addressed frame is a local concern
        io_.post([this, to, f = std::move(frame)]() mutable { send_on_loop(to, std::move(f)); });
    }

    void on_receive(std::function<void(MessageFrame)> cb) override {
        if (started_.load(std::memory_order_acquire))
            io_.post([this, cb = std::move(cb)]() mutable { receiver_ = std::move(cb); });
        else
            receiver_ = std::move(cb);
    }

    // ADR-046 cross-node backpressure. Runs synchronously on the CALLER's thread (not `io_`'s loop
    // thread) — called directly by DistributedRouter/SwimMembership before send() is ever invoked,
    // which is what lets a congested peer be shed BEFORE a frame is even posted onto the I/O loop.
    // `congestion_` has its own internal locking (governance.hpp), independent of `io_`'s thread.
    [[nodiscard]] bool admit_send(NodeId peer) noexcept override {
        return congestion_.admit(peer, now_ns());
    }
    void mark_congested(NodeId peer, std::int64_t remaining_ns) noexcept override {
        congestion_.mark_congested(peer, now_ns() + remaining_ns);
    }

    // 021 §"Teardown on death": SWIM declares `peer` dead ⇒ close its connection and stop reconnecting.
    // Buffered outbound frames are dropped (dead-lettered locally, 010). Thread-safe.
    void close_peer(NodeId peer) {
        io_.post([this, peer] {
            reconnect_suppressed_.insert(peer.value);
            if (auto it = conns_.find(peer.value); it != conns_.end()) drop_conn(it->second.get());
            outq_.erase(peer.value);  // dead-letter frames queued for the now-dead peer (010)
            discard_relay_fallback_state(peer);  // ADR-048: a dead peer has nothing left to fall back on
        });
    }

    // ADR-040 Phase 6: force-drop the LIVE connection to `peer` (e.g. a failed mTLS renegotiation, C2)
    // WITHOUT declaring the peer dead. Unlike close_peer() above (SWIM's permanent-death teardown, which
    // suppresses reconnect), this leaves reconnect-eligibility untouched: the ordinary handle_dead() path
    // salvages any unsent frames and — since the peer is not in reconnect_suppressed_ and its endpoint is
    // still known — immediately re-arms a reconnect, so a fresh TCP connection (and thus a fresh mTLS
    // handshake carrying a new generation) begins right away. A no-op if there is no live socket for
    // `peer` (e.g. already mid-Backoff, or the peer is unknown). Thread-safe (marshals onto the I/O loop
    // like every other public method here).
    //
    // THIS IS A DELIBERATE, NARROW ADDITION to this file's public surface — see 010-Distribution's
    // "Sibling seams" note (Phase 9 spec update) for why this is sanctioned and what it does NOT do
    // (it never suppresses reconnect, unlike close_peer()).
    void reset_peer_connection(NodeId peer) {
        io_.post([this, peer] {
            const auto it = conns_.find(peer.value);
            if (it == conns_.end()) return;
            Conn* c = it->second.get();
            if (c->fd == pal::invalid_fd) return;  // already a Backoff husk — nothing live to drop
            handle_dead(c);
        });
    }

    // Marshal an arbitrary callback onto the I/O thread — the SAME thread on_receive() callbacks run
    // on. SwimMembership (021 cluster.hpp) documents that tick() and inbound control-frame handling
    // must share one driving thread (all its mutation is unsynchronized by design, matching the
    // single-threaded LoopbackTransport test double it was proven against); a real caller driving it
    // over this async transport has no other thread-safe way to invoke tick(). Thread-safe (same
    // mutex-guarded post queue `send`/`close_peer` already use).
    void post(std::function<void()> fn) { io_.post(std::move(fn)); }

    // Fired on the I/O thread when an ESTABLISHED, identified connection to `peer` dies (peer restart,
    // RST, EOF, network drop) — before any reconnect is armed. `SecureTransport` wires this to
    // `on_peer_disconnected()` (ADR-040) so a per-connection mTLS session never outlives the
    // connection it was negotiated on. Set before start(); called on the loop thread.
    void set_peer_down_hook(std::function<void(NodeId)> fn) { peer_down_hook_ = std::move(fn); }

    // --- diagnostic counters (atomic; read from a test thread after a sync point) ---------------
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_.load(); }
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(); }
    [[nodiscard]] std::uint64_t drops() const noexcept { return drops_.load(); }
    [[nodiscard]] std::uint64_t dedup_closed() const noexcept { return dedup_closed_.load(); }
    [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_.load(); }
    [[nodiscard]] std::uint64_t connections_open() const noexcept { return conns_open_.load(); }

private:
    bool authenticated_transport_ = false;    // F1: set by SecureTransport before the inner starts
    bool allow_insecure_plaintext_ = false;   // F1: explicit dev opt-in only
    bool plaintext_start_refused_ = false;    // F1: diagnostic for a refused start()
    // Connecting: TCP connect in flight (dialer) — EPOLLOUT pending. Open: usable, frames flow.
    // Backoff: the socket died and a jittered reconnect timer is armed; the Conn object PERSISTS (with
    // its queued frames) but has no fd — a send queues, a reconnect revives it. There is no "Dead" map
    // entry: a connection we give up on is erased from conns_ entirely.
    enum class St : std::uint8_t { Connecting, Open, Backoff };

    struct Conn {
        NodeId peer{};
        pal::fd_t fd = pal::invalid_fd;
        std::uint64_t accept_serial = 0;  // identifies an accepted socket across fd reuse
        std::uint64_t peer_addr = 0;    // accepted socket's source (IPv4, host order)
        std::uint16_t peer_port = 0;
        bool has_peer_addr = false;
        St st = St::Connecting;
        bool initiated_by_self = false;  // did WE dial? (the dedup discriminant, 021 §2)
        bool identified = false;         // peer NodeId confirmed via hello (accept side learns it here)
        bool confirmed = false;          // won dedup ⇒ application frames may flow (021 §2: the loser is
                                         // dropped BEFORE any application frame flows). Until then only
                                         // the hello is transmitted; queued frames wait / migrate.

        std::array<std::byte, hello_size> hello{};  // our hello bytes
        std::size_t hello_sent = 0;                 // how many hello bytes are on the wire
        std::vector<std::byte> hello_in_;            // accumulates the peer's hello (may straddle recvs)
        bool peer_hello_read = false;                // have we consumed the peer's 14-byte hello?

        std::vector<std::byte> out;   // the ONE whole frame currently being written (never a splice
                                      // point mid-frame — send_some may return a partial byte count at
                                      // ANY offset, so priority can only rotate BETWEEN whole frames,
                                      // never inside one; see `load_next_frame`/ADR-046 S4)
        std::size_t out_sent = 0;     // bytes of `out` already handed to the kernel
        FrameStream in;               // inbound byte-stream reassembler (partial reads / coalescing)
        std::uint32_t backoff_attempt = 0;
        std::uint32_t interest = 0;   // current epoll interest mask (avoid redundant mod_fd)

        // ADR-048 relay fallback. `first_connecting_ns` is when the CURRENT give-up window started (0 =
        // none armed — either fallback is off, no relay is configured for this peer, or the peer is
        // already relay-routed); it is a wall-clock stamp (`now_ns()`), read only by the opportunistic
        // check in schedule_reconnect(). `fallback_epoch` increments on every direct⇄relay transition and
        // guards a stale check_relay_timeout()/direct-recovery-probe callback from acting on a Conn that
        // has since moved on (recycled, torn down, or already flipped again).
        std::int64_t first_connecting_ns = 0;
        std::uint32_t fallback_epoch = 0;
    };

    // ADR-048: an out-of-band connection that only ever proves "peer is reachable at its own direct
    // Endpoint right now" (connect + HELLO round-trip, never joins the peer's `outq_`/dedup machinery).
    // On success it hands its already-established, already-identified fd to cutover_to_direct(); on any
    // failure it is simply retried after another `fallback_timeout_ns_` (the disclosed single-constant,
    // no-backoff-of-its-own probe cadence, ADR-048 residual risks).
    struct Probe {
        NodeId peer{};
        pal::fd_t fd = pal::invalid_fd;
        std::uint32_t epoch = 0;         // must still match the live Conn's fallback_epoch to be honored
        bool connecting = true;          // awaiting EPOLLOUT connect-completion vs. mid hello exchange
        std::array<std::byte, hello_size> hello{};
        std::size_t hello_sent = 0;
        std::vector<std::byte> hello_in;
    };

    // ADR-046 S4 fix: per-peer queues of WHOLE, still-unstarted frames, split by FrameKind, so a
    // `Congested` control frame (or any SWIM control traffic) is never head-of-line-blocked behind a
    // peer's own Data backlog. Queues of FRAMES, not concatenated bytes — `load_next_frame` only ever
    // rotates `Conn::out` to a new frame once the previous one is fully sent, so control can jump
    // ahead of not-yet-started data, but never preempts a frame whose transmission has already begun
    // (a raw byte splice at an arbitrary `out_sent` offset can land mid-frame and corrupt the stream).
    struct PeerQueue {
        std::deque<std::vector<std::byte>> control;
        std::deque<std::vector<std::byte>> data;
    };

    // ---- send path (loop thread) ----------------------------------------------------------------
    // The outbound queue is PER PEER, not per connection: it outlives dial/dedup/reconnect churn, so a
    // frame is never lost when a connection object is dropped (dedup loser, RST during the race, teardown
    // before the survivor is identified). A confirmed connection PUMPs the peer queue onto its socket.
    void send_on_loop(NodeId to, MessageFrame frame) {
        Conn* c = ensure_conn(to);
        if (!c) {  // no endpoint known for `to` ⇒ cannot dial; fire-and-forget drop (010 dead-letter)
            drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::byte> bytes = encode_frame(frame);
        std::deque<std::vector<std::byte>>& dst =
            (frame.kind == FrameKind::Control) ? outq_[to.value].control : outq_[to.value].data;
        dst.push_back(std::move(bytes));
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        // Flows now only if this connection has won dedup and is up; otherwise the frame waits in the
        // peer queue until a confirmed connection pumps it (on confirm / on writable / on reconnect).
        if (c->confirmed && c->st == St::Open) pump(c);
    }

    // Rotate `c->out` to the NEXT whole, not-yet-started frame — called only once the previous frame
    // has fully drained (`out_sent == out.size()`). Control is preferred over data (ADR-046 S4): this
    // is the ONLY place priority is decided, and it only ever happens BETWEEN frames, never mid-frame.
    void load_next_frame(Conn* c) {
        if (c->out_sent < c->out.size()) return;  // a frame is still in flight — do not touch it
        c->out.clear();
        c->out_sent = 0;
        const auto qit = outq_.find(c->peer.value);
        if (qit == outq_.end()) return;
        PeerQueue& q = qit->second;
        if (!q.control.empty()) {
            c->out = std::move(q.control.front());
            q.control.pop_front();
        } else if (!q.data.empty()) {
            c->out = std::move(q.data.front());
            q.data.pop_front();
        }
    }

    // Kick the (confirmed) connection to pick up whatever is newly queued and flush.
    void pump(Conn* c) { flush(c); }

    // A connection is dying: the ONE frame currently loaded into `c->out`, if not yet started
    // (`out_sent == 0`), is returned to the FRONT of the peer's control queue so the next confirmed
    // connection retries it first — a torn, partially-sent frame (out_sent>0) cannot resume and is
    // dead-lettered (010 at-most-once). Every OTHER not-yet-started frame was never removed from
    // `outq_[peer]` in the first place (`load_next_frame` only pops one at a time), so nothing else
    // needs salvaging here. Re-queuing a salvaged DATA frame on the control queue is safe for
    // SecureTransport's per-lane replay guard (issue #16): the receiver picks the lane from the frame's
    // own `kind`, and the salvaged frame is the OLDEST unsent data frame, so data-lane FIFO still holds.
    void salvage_out(Conn* c) {
        if (!c->out.empty() && c->out_sent == 0) outq_[c->peer.value].control.push_front(std::move(c->out));
        c->out.clear();
        c->out_sent = 0;
    }

    // Get the connection to `to`, lazily DIALING if none exists (021 §2 lazy establishment). Any conn
    // already in the map (Connecting/Open/Backoff) is returned as-is — frames queue on it.
    Conn* ensure_conn(NodeId to) {
        if (auto it = conns_.find(to.value); it != conns_.end()) return it->second.get();
        const auto ep = resolve_dial_endpoint(to);
        if (!ep) return nullptr;  // no direct AND no relay endpoint known — caller drops
        return dial(to, *ep);
    }

    // ADR-048: the SOLE dial-target lookup — `to`'s direct Endpoint (peers_) normally, or its relay
    // Endpoint (relays_) once `relay_routed_` has flipped for it. Shared by ensure_conn()/dial() (fresh
    // dials) and do_reconnect() (the fix for the confirmed relay-leg-churn wedge: without this, a relay
    // connection's own reconnect would silently redial the (unreachable) direct endpoint instead).
    [[nodiscard]] std::optional<Endpoint> resolve_dial_endpoint(NodeId to) const {
        if (relay_routed_.count(to.value)) {
            if (const auto rit = relays_.find(to.value); rit != relays_.end()) return rit->second;
        }
        if (const auto pit = peers_.find(to.value); pit != peers_.end()) return pit->second;
        return std::nullopt;
    }

    Conn* dial(NodeId to, const Endpoint& ep) {
        auto fd = pal::tcp_connect(ep.addr, ep.port);
        if (!fd) return nullptr;
        auto c = std::make_unique<Conn>();
        c->in.set_max_body(max_frame_body_);
        c->peer = to;
        c->fd = *fd;
        c->peer_addr = ep.addr;
        c->peer_port = ep.port;
        c->has_peer_addr = true;
        c->st = St::Connecting;
        c->initiated_by_self = true;
        c->identified = true;  // we know exactly who we dialed
        build_hello(c->hello);
        Conn* raw = c.get();
        conns_[to.value] = std::move(c);
        by_fd_[raw->fd] = raw;
        raw->interest = EPOLLIN | EPOLLOUT;  // EPOLLOUT = connect completion
        io_.add_fd(raw->fd, raw->interest, [this, fd = raw->fd](std::uint32_t ev) { on_conn(fd, ev); });
        arm_fallback_if_needed(raw);  // ADR-048
        return raw;
    }

    // ---- accept path (loop thread) --------------------------------------------------------------
    void on_accept_ready() {
        for (;;) {
            std::uint64_t peer_addr = 0;
            std::uint16_t peer_port = 0;
            auto fd = pal::accept_one(listener_, &peer_addr, &peer_port);
            if (!fd) break;  // would_block (drained) or a transient error — stop this batch
            if (max_pending_in_ != 0 && pending_in_.size() >= max_pending_in_) {
                pal::close_fd(*fd);
                drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (max_pending_per_ip_ != 0 && peer_addr != 0) {
                std::uint32_t same_ip = 0;
                for (const auto& [pfd, pc] : pending_in_)
                    if (pc->has_peer_addr && pc->peer_addr == peer_addr) ++same_ip;
                if (same_ip >= max_pending_per_ip_) {
                    pal::close_fd(*fd);
                    drops_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }
            auto c = std::make_unique<Conn>();
            c->in.set_max_body(max_frame_body_);
            c->accept_serial = ++accept_serial_;
            c->fd = *fd;
            c->peer_addr = peer_addr;
            c->peer_port = peer_port;
            c->has_peer_addr = true;
            c->st = St::Open;               // TCP already established for an accepted socket
            c->initiated_by_self = false;    // the PEER dialed us
            c->identified = false;           // learn their NodeId from their hello
            build_hello(c->hello);
            Conn* raw = c.get();
            pending_in_[raw->fd] = std::move(c);
            by_fd_[raw->fd] = raw;
            raw->interest = EPOLLIN | EPOLLOUT;  // EPOLLOUT to push our hello out
            io_.add_fd(raw->fd, raw->interest, [this, f = raw->fd](std::uint32_t ev) { on_conn(f, ev); });
            if (pending_hello_timeout_ns_ > 0) {
                // A socket that never identifies must not pin a pending slot forever (slowloris).
                // The serial guards against fd reuse after the original socket closed.
                io_.post_after(pending_hello_timeout_ns_, [this, f = raw->fd, s = raw->accept_serial] {
                    const auto pit = pending_in_.find(f);
                    if (pit == pending_in_.end() || pit->second->accept_serial != s) return;
                    drops_.fetch_add(1, std::memory_order_relaxed);
                    handle_dead(pit->second.get());
                });
            }
        }
    }

    // ---- per-connection readiness (loop thread) -------------------------------------------------
    void on_conn(pal::fd_t fd, std::uint32_t ev) {
        const auto it = by_fd_.find(fd);
        if (it == by_fd_.end()) return;
        Conn* c = it->second;
        if (ev & (EPOLLERR | EPOLLHUP)) {
            handle_dead(c);
            return;
        }
        if (c->st == St::Connecting && (ev & EPOLLOUT)) {
            if (!pal::connect_result(fd)) {  // connect failed
                handle_dead(c);
                return;
            }
            mark_open(c);  // flips Connecting → Open and counts the connection exactly once
        }
        if (ev & EPOLLOUT) {
            if (c->confirmed) pump(c);  // drain the peer queue + continue any partial write
            else flush(c);              // unconfirmed: only the hello may progress
        }
        if (ev & EPOLLIN) do_read(c);
    }

    // Drain as much of `out` (hello first) to the kernel as it will take; adjust EPOLLOUT interest.
    void flush(Conn* c) {
        // 1) hello must precede every frame on a fresh stream.
        while (c->hello_sent < hello_size) {
            auto r = pal::send_some(c->fd, c->hello.data() + c->hello_sent, hello_size - c->hello_sent);
            if (!r) {
                if (r.error() == pal::would_block()) {
                    update_interest(c);
                    return;
                }
                handle_dead(c);
                return;
            }
            if (*r == 0) { update_interest(c); return; }
            c->hello_sent += *r;
        }
        // 2) then whichever frame is current, rotating to the NEXT one (control preferred — ADR-046
        // S4) only once each fully drains — populated by pump() only for a CONFIRMED connection (021
        // §2: no application frame flows before the dedup loser is dropped). `c->out` is empty until
        // confirm. The rotation happens entirely BETWEEN frames (`load_next_frame`'s own guard), so a
        // `send_some` partial write landing mid-frame never risks a priority-driven splice corrupting it.
        if (c->confirmed) {
            for (;;) {
                if (c->out_sent >= c->out.size()) {
                    // ADR-048 fenced cutover: only ever swap `c->fd` here, at a whole-frame boundary
                    // (out_sent==out.size(), the same guard load_next_frame uses) — never mid-frame. A
                    // `continue` re-enters the loop against the NEW fd so any already-queued outq_ frame
                    // sends over the recovered direct connection without further special-casing.
                    if (auto pit = pending_cutover_.find(c->peer.value); pit != pending_cutover_.end()) {
                        const pal::fd_t new_fd = pit->second;
                        pending_cutover_.erase(pit);
                        cutover_to_direct(c, new_fd);
                        continue;
                    }
                    load_next_frame(c);
                    if (c->out.empty()) break;  // nothing else queued right now
                }
                auto r = pal::send_some(c->fd, c->out.data() + c->out_sent, c->out.size() - c->out_sent);
                if (!r) {
                    if (r.error() == pal::would_block()) {
                        update_interest(c);
                        return;
                    }
                    handle_dead(c);
                    return;
                }
                if (*r == 0) break;
                c->out_sent += *r;
            }
        }
        update_interest(c);
    }

    void do_read(Conn* c) {
        std::array<std::byte, 64 * 1024> buf;
        for (;;) {
            auto r = pal::recv_some(c->fd, buf.data(), buf.size());
            if (!r) {
                if (r.error() == pal::would_block()) return;
                handle_dead(c);
                return;
            }
            if (*r == 0) {  // orderly peer close (EOF)
                handle_dead(c);
                return;
            }
            const std::byte* p = buf.data();
            std::size_t n = *r;
            // Consume the peer's fixed hello first (it may straddle recv boundaries).
            if (!c->peer_hello_read) {
                const std::size_t need = hello_size - c->hello_in_.size();
                const std::size_t take = n < need ? n : need;
                c->hello_in_.insert(c->hello_in_.end(), p, p + take);
                p += take;
                n -= take;
                if (c->hello_in_.size() < hello_size) return;  // partial hello — wait for more
                NodeId learned{};
                if (!parse_hello(c->hello_in_.data(), learned)) {  // bad magic/version ⇒ drop
                    handle_dead(c);
                    return;
                }
                if (c->initiated_by_self && learned != c->peer) {  // dialed X, someone else answered
                    handle_dead(c);
                    return;
                }
                c->peer_hello_read = true;
                if (!register_identified(c, learned)) return;  // c was the dedup LOSER and is gone
                // register_identified returning true keeps `c` as the registered connection for the
                // peer; the remaining bytes below belong to its stream.
            }
            // Remaining bytes are length-prefixed frames — the "stream" reassembler emits each whole one.
            const PeerAddress src{c->peer_addr, c->peer_port, c->has_peer_addr};
            const bool ok = c->in.feed(p, n, [this, src](MessageFrame f) {
                frames_received_.fetch_add(1, std::memory_order_relaxed);
                f.source_address = src;  // transient: never encoded, used for pre-auth admission
                if (receiver_) receiver_(std::move(f));
            });
            if (!ok) {  // protocol error on the stream (oversized/malformed) ⇒ untrustworthy, drop
                if (auto it = conns_.find(c->peer.value); it != conns_.end() && it->second.get() == c)
                    handle_dead(c);
                return;
            }
        }
    }

    // Associate a just-identified connection with its peer, applying the lower-NodeId dial-dedup RULE
    // (021 §2). Returns true if `c` SURVIVES (or is now the registered connection), false if `c` was the
    // loser and has been closed (the caller must stop touching it).
    bool register_identified(Conn* c, NodeId peer) {
        c->peer = peer;
        c->identified = true;
        // Move an accepted conn out of pending_in_ (keyed by fd) if that is where it lives.
        std::unique_ptr<Conn> owned_c;
        if (auto pit = pending_in_.find(c->fd); pit != pending_in_.end()) {
            owned_c = std::move(pit->second);
            pending_in_.erase(pit);
        }
        auto it = conns_.find(peer.value);
        if (it == conns_.end() || it->second->st == St::Backoff) {
            // No live existing (or only a fd-less Backoff husk): a fresh live inbound supersedes it.
            if (it != conns_.end()) drop_conn(it->second.get());  // cancels the husk (do_reconnect no-ops)
            if (owned_c) conns_[peer.value] = std::move(owned_c);
            c->confirmed = true;  // sole connection to this peer — frames may flow
            mark_open(c);
            pump(c);
            on_confirmed(c);
            return true;
        }
        Conn* existing = it->second.get();
        if (existing == c) {  // the dialer's own conn confirming the peer it dialed — no rival, it wins
            c->confirmed = true;
            mark_open(c);
            pump(c);
            on_confirmed(c);
            return true;
        }
        // Two connections between the same pair — keep the one the dedup rule elects, close the other.
        // The peer's queued frames live in outq_ (survivor pumps them once confirmed); salvage_out below
        // reclaims any frames the loser had queued but not yet sent. NOTE (at-most-once, 010): a frame the
        // loser had ALREADY put on the wire in the sub-ms simultaneous-dial race window is dropped when
        // its socket closes — within at-most-once semantics. A future refinement (documented seam) elects
        // the canonical connection deterministically from node ids so data only ever flows over it (needs
        // a two-connection-slot rework + gossiped-endpoint dial-back); once settled, delivery is lossless.
        const bool want_self_initiated = keep_local_dial(self_, peer);
        Conn* survivor = (c->initiated_by_self == want_self_initiated) ? c : existing;
        dedup_closed_.fetch_add(1, std::memory_order_relaxed);
        if (survivor == c) {
            // `c` (the new conn) wins: take its ownership into conns_[peer], drop the old existing.
            salvage_out(existing);       // recover any unsent frames the loser queued (race window)
            drop_conn_detach(existing);  // remove existing from conns_, close it
            if (owned_c) conns_[peer.value] = std::move(owned_c);
            c->confirmed = true;
            mark_open(c);
            pump(c);  // the surviving connection drains the peer queue (incl. salvaged frames)
            on_confirmed(c);
            return true;
        }
        // existing wins: close `c`. owned_c (if set) drops here with its fd closed.
        salvage_out(c);  // recover any unsent frames `c` queued before losing (race window)
        retire_fd(c->fd);
        by_fd_.erase(c->fd);
        existing->confirmed = true;
        mark_open(existing);
        pump(existing);  // the surviving connection drains the peer queue
        on_confirmed(existing);
        return false;
    }

    // ADR-048: a connection that the PEER dialed (accepted, or won dedup as the accept side) is direct
    // reachability made manifest — clear any stale `relay_routed_` flag for it (e.g. the peer's NAT/
    // firewall state changed and it reached us before our own recovery probe did) and drop a pending
    // cutover fd that would otherwise leak, now that this Conn is already, genuinely, directly Open.
    void on_confirmed(Conn* c) {
        if (c->initiated_by_self) return;  // our own dial (direct OR relay-routed) proves nothing new
        relay_routed_.erase(c->peer.value);
        if (auto pit = pending_cutover_.find(c->peer.value); pit != pending_cutover_.end()) {
            pal::close_fd(pit->second);
            pending_cutover_.erase(pit);
        }
    }

    // ---- teardown / reconnect -------------------------------------------------------------------
    void handle_dead(Conn* c) {
        const NodeId peer = c->peer;
        const bool was_identified = c->identified;
        const bool was_established = was_open(c);  // captured before unmark_open() clears it below
        salvage_out(c);  // return unsent whole frames to the peer queue BEFORE the fd/state go away
        retire_fd(c->fd);
        by_fd_.erase(c->fd);
        if (was_open(c)) unmark_open(c);

        // A pending (never-identified) inbound just disappears.
        if (auto pit = pending_in_.find(c->fd); pit != pending_in_.end()) {
            pending_in_.erase(pit);
            return;
        }
        auto it = (was_identified) ? conns_.find(peer.value) : conns_.end();
        if (it == conns_.end() || it->second.get() != c) return;

        if (was_established && was_identified && peer_down_hook_) peer_down_hook_(peer);
        const bool suppressed = reconnect_suppressed_.count(peer.value) != 0;
        const bool have_endpoint = peers_.count(peer.value) != 0;
        // Reconnect any peer whose listen endpoint we know (021 §"Reconnect …, capped"). If both ends
        // know each other (add_peer on both), both may redial; dial-dedup then collapses to one. A SWIM
        // death declaration (close_peer) suppresses reconnect — the peer is gone, not flapping.
        if (!suppressed && have_endpoint) {
            schedule_reconnect(peer);
        } else {
            conns_.erase(it);        // give up: forget the connection …
            outq_.erase(peer.value);  // … and dead-letter its queued frames (010)
            discard_relay_fallback_state(peer);  // ADR-048
        }
    }

    void schedule_reconnect(NodeId peer) {
        auto it = conns_.find(peer.value);
        if (it == conns_.end()) return;
        Conn* c = it->second.get();
        // ADR-048 opportunistic early check: re-entering Backoff is a good moment to notice the give-up
        // deadline has ALREADY passed without waiting for the one-shot timer armed in arm_fallback_if_
        // needed() to fire. Only meaningful for a still-direct peer (relay-routed Backoff cycles reconnect
        // through the relay Endpoint as normal, via do_reconnect()'s resolve_dial_endpoint() call).
        if (fallback_timeout_ns_ > 0 && !relay_routed_.count(peer.value) && c->first_connecting_ns != 0 &&
            now_ns() - c->first_connecting_ns >= fallback_timeout_ns_) {
            check_relay_timeout(peer, c->fallback_epoch);  // may replace/erase conns_[peer] — stop here
            return;
        }
        c->st = St::Backoff;
        c->fd = pal::invalid_fd;
        c->hello_sent = 0;
        c->peer_hello_read = false;
        c->confirmed = false;  // the new socket must re-exchange hello + re-confirm before frames flow
        c->hello_in_.clear();
        c->in.clear();
        // c->out was already salvaged to outq_ by handle_dead; the frames survive the reconnect there.
        const std::uint32_t attempt = c->backoff_attempt++;
        const std::int64_t delay = backoff_delay(peer, attempt);
        io_.post_after(delay, [this, peer] { do_reconnect(peer); });
    }

    void do_reconnect(NodeId peer) {
        auto it = conns_.find(peer.value);
        if (it == conns_.end()) return;
        // The Backoff husk may have been superseded by a live inbound (register_identified replaced it)
        // between the timer arming and firing — only reconnect if it is STILL an awaiting-reconnect husk.
        if (it->second->st != St::Backoff) return;
        if (reconnect_suppressed_.count(peer.value)) {
            conns_.erase(it);
            outq_.erase(peer.value);
            discard_relay_fallback_state(peer);  // ADR-048
            return;
        }
        // ADR-048: resolve_dial_endpoint(), not a raw peers_ lookup — a relay-routed peer's reconnect
        // must keep redialing the RELAY Endpoint (the fix for the confirmed relay-leg-churn wedge; see
        // ADR-048's cross-examination summary).
        const auto ep = resolve_dial_endpoint(peer);
        if (!ep) {
            conns_.erase(it);
            outq_.erase(peer.value);
            discard_relay_fallback_state(peer);  // ADR-048
            return;
        }
        auto fd = pal::tcp_connect(ep->addr, ep->port);
        if (!fd) {  // dial failed outright — back off and try again
            schedule_reconnect(peer);
            return;
        }
        Conn* c = it->second.get();
        c->fd = *fd;
        c->st = St::Connecting;
        c->initiated_by_self = true;
        c->identified = true;
        build_hello(c->hello);
        by_fd_[c->fd] = c;
        c->interest = EPOLLIN | EPOLLOUT;
        io_.add_fd(c->fd, c->interest, [this, f = c->fd](std::uint32_t ev) { on_conn(f, ev); });
        reconnects_.fetch_add(1, std::memory_order_relaxed);
        arm_fallback_if_needed(c);  // ADR-048: no-op while already relay-routed
    }

    // Deterministic jittered exponential backoff (021): no random_device / wall clock. base*2^attempt
    // capped, plus jitter in [0,base) folded from (self,peer,attempt) via splitmix64.
    [[nodiscard]] std::int64_t backoff_delay(NodeId peer, std::uint32_t attempt) const noexcept {
        const std::uint32_t shift = attempt > 20 ? 20 : attempt;
        std::int64_t base = reconnect_base_ns << shift;
        if (base > reconnect_cap_ns || base <= 0) base = reconnect_cap_ns;
        const std::uint64_t mix = quark::detail::splitmix64(
            quark::detail::hash_combine(self_.value ^ peer.value, attempt));
        const std::int64_t jitter = static_cast<std::int64_t>(mix % static_cast<std::uint64_t>(base));
        return base + jitter;
    }

    // ---- small helpers --------------------------------------------------------------------------
    // Deregister from the reactor BEFORE closing. The kernel may hand this fd number to a concurrent
    // open/accept/connect between the two calls; IoContext keys its handler table by fd and
    // epoll_ctl(DEL) targets the registration, so closing first would let del_fd() erase (or act on)
    // the NEW descriptor's handler instead of ours. 010/021 fd-lifetime invariant — never open-code
    // this pair again.
    void retire_fd(pal::fd_t fd) noexcept {
        if (fd == pal::invalid_fd) return;
        io_.del_fd(fd);
        pal::close_fd(fd);
    }

    // Detach `existing` from conns_ WITHOUT erasing (the caller re-inserts the survivor), closing its fd.
    void drop_conn_detach(Conn* existing) {
        retire_fd(existing->fd);
        by_fd_.erase(existing->fd);
        if (was_open(existing)) unmark_open(existing);
        // erase the owning entry (it is keyed by peer)
        conns_.erase(existing->peer.value);
    }

    // Fully drop a connection: close fd, deregister, remove from whichever map owns it.
    void drop_conn(Conn* c) {
        retire_fd(c->fd);
        by_fd_.erase(c->fd);
        if (was_open(c)) unmark_open(c);
        if (auto pit = pending_in_.find(c->fd); pit != pending_in_.end()) {
            pending_in_.erase(pit);
            return;
        }
        if (c->identified) conns_.erase(c->peer.value);
    }

    void update_interest(Conn* c) {
        if (c->fd == pal::invalid_fd) return;  // a Backoff husk has no socket to arm
        std::uint32_t want = EPOLLIN;
        if (c->st == St::Connecting) want |= EPOLLOUT;  // connect completion
        // Arm EPOLLOUT for pending hello, or for pending frames ONLY once confirmed — otherwise an
        // unconfirmed conn with queued frames would spin on writable readiness it will not act on.
        if (c->hello_sent < hello_size || (c->confirmed && c->out_sent < c->out.size())) want |= EPOLLOUT;
        if (want != c->interest) {
            c->interest = want;
            io_.mod_fd(c->fd, want);
        }
    }

    void mark_open(Conn* c) {
        if (!open_marked_.count(c)) {
            open_marked_.insert(c);
            conns_open_.fetch_add(1, std::memory_order_relaxed);
        }
        if (c->st == St::Connecting) c->st = St::Open;
        c->first_connecting_ns = 0;  // ADR-048: succeeded — no direct give-up window pending
    }
    [[nodiscard]] bool was_open(Conn* c) const { return open_marked_.count(c) != 0; }
    void unmark_open(Conn* c) {
        if (open_marked_.erase(c)) conns_open_.fetch_sub(1, std::memory_order_relaxed);
    }

    // ---- ADR-048: relay fallback ------------------------------------------------------------------
    // Arm the one-shot give-up timer for `c`'s CURRENT fallback epoch, IF: relay fallback is configured
    // (fallback_timeout_ns_ > 0), `c`'s peer has a relay Endpoint on record (set_relay), the peer is not
    // already relay-routed, and no timer is already outstanding for this epoch (`first_connecting_ns == 0`
    // — the one-shot guard: repeated Backoff→do_reconnect cycles within the SAME epoch must not each
    // re-arm a fresh TimeoutMs window, or the give-up deadline would never actually arrive under a fast
    // backoff cadence). Called from both dial() (fresh dials) and do_reconnect() (retries).
    void arm_fallback_if_needed(Conn* c) {
        if (fallback_timeout_ns_ <= 0) return;                     // NoRelayFallback — feature off
        if (relay_routed_.count(c->peer.value)) return;            // already on the relay leg
        if (!relays_.count(c->peer.value)) return;                 // nothing configured to fall back to
        if (c->first_connecting_ns != 0) return;                   // already armed this epoch
        c->first_connecting_ns = now_ns();
        const NodeId peer = c->peer;
        const std::uint32_t epoch = c->fallback_epoch;
        io_.post_after(fallback_timeout_ns_, [this, peer, epoch] { check_relay_timeout(peer, epoch); });
    }

    // The one-shot give-up backstop: fires `fallback_timeout_ns_` after the epoch's first St::Connecting,
    // regardless of how many Backoff/reconnect cycles happened in between — this is what catches a
    // black-holed connect() that never even errors out of Connecting (ADR-048's C3).
    void check_relay_timeout(NodeId peer, std::uint32_t epoch) {
        const auto it = conns_.find(peer.value);
        if (it == conns_.end()) return;                            // peer torn down — nothing to give up on
        Conn* c = it->second.get();
        if (c->fallback_epoch != epoch) return;                    // stale: this Conn moved on already
        if (c->st == St::Open) return;                             // direct succeeded before the timer fired
        if (relay_routed_.count(peer.value)) return;                // already flipped (opportunistic-check race)
        const auto rit = relays_.find(peer.value);
        if (rit == relays_.end()) return;                           // relay unconfigured meanwhile — nothing to do

        // Give up on the direct attempt: drop the socket/husk (any unsent frame it held was already
        // salvaged into outq_ back when it entered Backoff, or never existed for an unconfirmed Connecting
        // Conn — outq_[peer] itself is untouched) and immediately redial through the relay Endpoint.
        relay_routed_.insert(peer.value);
        const std::uint32_t next_epoch = c->fallback_epoch + 1;  // read before drop_conn destroys `c`
        drop_conn(c);
        Conn* nc = ensure_conn(peer);  // resolve_dial_endpoint() now returns the relay Endpoint for `peer`
        if (nc) {
            nc->fallback_epoch = next_epoch;
            nc->first_connecting_ns = 0;   // no direct give-up window while relay-routed
            arm_direct_probe(peer, next_epoch);
        }
    }

    // Out-of-band direct-reachability probing (recovery). Re-arms itself at `fallback_timeout_ns_`
    // cadence — ADR-048's disclosed residual: one constant does double duty as both the give-up threshold
    // and the probe interval, with no backoff/jitter of its own.
    void arm_direct_probe(NodeId peer, std::uint32_t epoch) {
        io_.post_after(fallback_timeout_ns_, [this, peer, epoch] { try_direct_probe(peer, epoch); });
    }

    void try_direct_probe(NodeId peer, std::uint32_t epoch) {
        if (!relay_routed_.count(peer.value)) return;  // already recovered (or torn down) — stop probing
        const auto it = conns_.find(peer.value);
        if (it == conns_.end() || it->second->fallback_epoch != epoch) return;  // stale generation
        const auto pit = peers_.find(peer.value);
        if (pit == peers_.end()) {  // no direct endpoint on record — nothing to probe yet
            arm_direct_probe(peer, epoch);
            return;
        }
        auto fd = pal::tcp_connect(pit->second.addr, pit->second.port);
        if (!fd) {  // still unreachable — try again next interval
            arm_direct_probe(peer, epoch);
            return;
        }
        auto p = std::make_unique<Probe>();
        p->peer = peer;
        p->fd = *fd;
        p->epoch = epoch;
        build_hello(p->hello);
        const pal::fd_t raw = p->fd;
        probes_[raw] = std::move(p);
        io_.add_fd(raw, EPOLLIN | EPOLLOUT, [this, raw](std::uint32_t ev) { on_probe(raw, ev); });
    }

    void on_probe(pal::fd_t fd, std::uint32_t ev) {
        const auto it = probes_.find(fd);
        if (it == probes_.end()) return;
        Probe* p = it->second.get();
        if (ev & (EPOLLERR | EPOLLHUP)) {
            fail_probe(fd);
            return;
        }
        if (p->connecting) {
            if (!(ev & EPOLLOUT)) return;
            if (!pal::connect_result(fd)) {
                fail_probe(fd);
                return;
            }
            p->connecting = false;
        }
        if (ev & EPOLLOUT) {
            while (p->hello_sent < hello_size) {
                auto r = pal::send_some(fd, p->hello.data() + p->hello_sent, hello_size - p->hello_sent);
                if (!r) {
                    if (r.error() == pal::would_block()) {
                        io_.mod_fd(fd, EPOLLIN | EPOLLOUT);
                        return;
                    }
                    fail_probe(fd);
                    return;
                }
                if (*r == 0) {
                    io_.mod_fd(fd, EPOLLIN | EPOLLOUT);
                    return;
                }
                p->hello_sent += *r;
            }
            io_.mod_fd(fd, EPOLLIN);  // our hello is fully out — only reading theirs from here
        }
        if (ev & EPOLLIN) {
            std::array<std::byte, hello_size> buf{};
            for (;;) {
                const std::size_t need = hello_size - p->hello_in.size();
                if (need == 0) break;
                auto r = pal::recv_some(fd, buf.data(), need);
                if (!r) {
                    if (r.error() == pal::would_block()) return;
                    fail_probe(fd);
                    return;
                }
                if (*r == 0) {  // peer closed before completing its hello
                    fail_probe(fd);
                    return;
                }
                p->hello_in.insert(p->hello_in.end(), buf.data(), buf.data() + *r);
            }
            NodeId learned{};
            if (!parse_hello(p->hello_in.data(), learned) || learned != p->peer) {
                fail_probe(fd);  // wrong/garbled answer — not the peer we're trying to recover
                return;
            }
            succeed_probe(fd);
        }
    }

    void fail_probe(pal::fd_t fd) {
        const auto it = probes_.find(fd);
        if (it == probes_.end()) return;
        const NodeId peer = it->second->peer;
        const std::uint32_t epoch = it->second->epoch;
        retire_fd(fd);
        probes_.erase(it);
        if (relay_routed_.count(peer.value)) arm_direct_probe(peer, epoch);  // keep trying at the same cadence
    }

    void succeed_probe(pal::fd_t fd) {
        const auto it = probes_.find(fd);
        if (it == probes_.end()) return;
        std::unique_ptr<Probe> p = std::move(it->second);
        probes_.erase(it);
        io_.del_fd(fd);  // stop the probe's own handler — cutover_to_direct() re-adds it under Conn's
        const auto cit = conns_.find(p->peer.value);
        if (cit == conns_.end() || cit->second->fallback_epoch != p->epoch ||
            !relay_routed_.count(p->peer.value)) {
            pal::close_fd(fd);  // stale: the Conn moved on (or already recovered) while this probe ran
            return;
        }
        cutover_direct_or_defer(cit->second.get(), fd);
    }

    // If a frame is mid-send over the (still relay-routed) live connection, defer the swap until it fully
    // leaves the wire (017's no-mid-frame-splice requirement) — flush()'s loop applies it at the next
    // frame boundary. Otherwise there is nothing to wait for; cut over immediately.
    void cutover_direct_or_defer(Conn* c, pal::fd_t new_fd) {
        if (c->out_sent > 0 && c->out_sent < c->out.size()) {
            pending_cutover_[c->peer.value] = new_fd;
            return;
        }
        cutover_to_direct(c, new_fd);
        flush(c);  // resume draining outq_ (if anything queued) on the freshly swapped fd
    }

    // The fenced swap itself: retire the relay fd, adopt the probe's already-connected, already-HELLO-
    // confirmed fd in its place. Never calls flush()/pump() itself (both call sites above do, exactly
    // once, in the right place) — avoids re-entering flush()'s own send loop when invoked from within it.
    void cutover_to_direct(Conn* c, pal::fd_t new_fd) {
        by_fd_.erase(c->fd);
        io_.del_fd(c->fd);  // 019 fix: del_fd BEFORE add_fd — epoll EEXISTs (silently) on a live re-add
        pal::close_fd(c->fd);
        c->fd = new_fd;
        c->in.clear();             // fresh byte stream; the new fd's hello was already consumed by the probe
        c->hello_sent = hello_size;  // our hello was already sent during probing
        c->peer_hello_read = true;   // the peer's hello was already validated during probing
        c->interest = 0;             // force update_interest() to (re)establish the fd's interest mask
        by_fd_[new_fd] = c;
        io_.add_fd(new_fd, EPOLLIN, [this, fd = new_fd](std::uint32_t ev) { on_conn(fd, ev); });
        relay_routed_.erase(c->peer.value);
        c->fallback_epoch++;       // a fresh generation: any outstanding probe/timer for the old one is stale
        c->first_connecting_ns = 0;
        update_interest(c);
    }

    void cancel_probes_for(NodeId peer) {
        for (auto it = probes_.begin(); it != probes_.end();) {
            if (it->second->peer.value == peer.value) {
                retire_fd(it->first);
                it = probes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Drop every piece of ADR-048 bookkeeping for a peer that is being torn down entirely (SWIM death via
    // close_peer(), or handle_dead()'s own give-up path) — there is nothing left to fall back on or
    // recover to. Leaves `relays_[peer]` itself untouched (a future add_peer()/close_peer() cycle for the
    // same NodeId may reuse it — mirrors peers_'s own lifetime, never cleared by close_peer() either).
    void discard_relay_fallback_state(NodeId peer) {
        relay_routed_.erase(peer.value);
        if (auto pit = pending_cutover_.find(peer.value); pit != pending_cutover_.end()) {
            pal::close_fd(pit->second);
            pending_cutover_.erase(pit);
        }
        cancel_probes_for(peer);
    }

    void build_hello(std::array<std::byte, hello_size>& h) const noexcept {
        auto put = [&](std::size_t off, std::uint64_t v, int bytes) {
            for (int i = 0; i < bytes; ++i)
                h[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
        };
        put(0, hello_magic, 4);
        put(4, hello_version, 2);
        put(6, self_.value, 8);
    }
    [[nodiscard]] static bool parse_hello(const std::byte* p, NodeId& out) noexcept {
        auto get = [&](std::size_t off, int bytes) {
            std::uint64_t v = 0;
            for (int i = 0; i < bytes; ++i)
                v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[off + static_cast<std::size_t>(i)]))
                     << (8 * i);
            return v;
        };
        if (static_cast<std::uint32_t>(get(0, 4)) != hello_magic) return false;
        if (static_cast<std::uint16_t>(get(4, 2)) != hello_version) return false;
        out = NodeId{get(6, 8)};
        return true;
    }

    // ADR-046 gate's own clock — independent of the caller's thread/context, mirrors the
    // `real_steady_ns` pattern cluster.hpp/secure_transport.hpp already use.
    [[nodiscard]] static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch())
            .count();
    }

    NodeId self_;
    std::uint64_t bind_addr_;
    std::uint16_t bind_port_;
    std::uint32_t max_frame_body_ = max_frame_body;
    std::uint32_t max_pending_in_ = 1024;
    std::uint32_t max_pending_per_ip_ = 64;
    std::int64_t pending_hello_timeout_ns_ = 5'000'000'000;  // 5s to identify
    std::uint64_t accept_serial_ = 0;
    pal::fd_t listener_ = pal::invalid_fd;

    pal::IoContext io_;
    std::thread io_thread_;
    std::atomic<bool> started_{false};

    std::function<void(MessageFrame)> receiver_;  // set before start(); read on the loop thread
    std::function<void(NodeId)> peer_down_hook_;  // set before start(); called on the loop thread

    // Connection state — all loop-thread-owned. conns_: identified peers (keyed by NodeId). pending_in_:
    // accepted connections whose peer NodeId is not yet known (keyed by fd). by_fd_: raw handler lookup.
    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;
    std::unordered_map<pal::fd_t, std::unique_ptr<Conn>> pending_in_;
    std::unordered_map<pal::fd_t, Conn*> by_fd_;
    std::unordered_map<std::uint64_t, Endpoint> peers_;  // NodeId → where it listens (for dial/reconnect)
    std::unordered_map<std::uint64_t, PeerQueue> outq_;  // per-peer pending frames (survives connection
                                                          // churn — see send_on_loop), split control/data
    std::unordered_set<std::uint64_t> reconnect_suppressed_;  // peers SWIM declared dead (close_peer)
    std::unordered_set<Conn*> open_marked_;  // conns counted in conns_open_ (idempotent open/close)
    PeerCongestionTable congestion_;  // ADR-046 — checked on the CALLER's thread, before send() posts

    // ADR-048 relay fallback state — all loop-thread-owned, like everything else above.
    std::int64_t fallback_timeout_ns_ = 0;  // 0 = NoRelayFallback (feature off, byte-identical to before)
    std::unordered_map<std::uint64_t, Endpoint> relays_;  // peer → its designated relay Endpoint (set_relay)
    std::unordered_set<std::uint64_t> relay_routed_;      // peers currently dialed via their relay Endpoint
    std::unordered_map<std::uint64_t, pal::fd_t> pending_cutover_;  // peer → probed direct fd awaiting a
                                                                     // frame boundary (fenced cutover)
    std::unordered_map<pal::fd_t, std::unique_ptr<Probe>> probes_;  // out-of-band direct-reachability probes

    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> dedup_closed_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<std::uint64_t> conns_open_{0};
};

}  // namespace quark::net
