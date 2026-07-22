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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pal/net.hpp"
#include "quark/core/cluster.hpp"    // Endpoint, dial_winner, keep_local_dial (021 §2 — the dedup RULE)
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
    TcpTransport(NodeId self, std::uint64_t bind_addr, std::uint16_t bind_port) noexcept
        : self_(self), bind_addr_(bind_addr), bind_port_(bind_port) {}

    ~TcpTransport() override { stop(); }

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Teach the transport where a peer LISTENS, so a lazy dial to it can resolve an address. Fed from
    // 021 Discovery/seeds + SWIM gossip in a full deployment; called directly in a test. Thread-safe
    // before start(); after start() it is marshalled onto the loop.
    void add_peer(Endpoint ep) {
        if (started_.load(std::memory_order_acquire))
            io_.post([this, ep] { peers_[ep.node.value] = ep; });
        else
            peers_[ep.node.value] = ep;
    }

    // Bind + listen + spawn the I/O thread. Returns false if the listener could not be opened.
    bool start() {
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
        for (auto& [id, c] : conns_) pal::close_fd(c->fd);
        for (auto& [fd, c] : pending_in_) pal::close_fd(c->fd);
        pal::close_fd(listener_);
        listener_ = pal::invalid_fd;
        conns_.clear();
        pending_in_.clear();
        by_fd_.clear();
        open_marked_.clear();
        reconnect_suppressed_.clear();
        outq_.clear();
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

    // 021 §"Teardown on death": SWIM declares `peer` dead ⇒ close its connection and stop reconnecting.
    // Buffered outbound frames are dropped (dead-lettered locally, 010). Thread-safe.
    void close_peer(NodeId peer) {
        io_.post([this, peer] {
            reconnect_suppressed_.insert(peer.value);
            if (auto it = conns_.find(peer.value); it != conns_.end()) drop_conn(it->second.get());
            outq_.erase(peer.value);  // dead-letter frames queued for the now-dead peer (010)
        });
    }

    // Marshal an arbitrary callback onto the I/O thread — the SAME thread on_receive() callbacks run
    // on. SwimMembership (021 cluster.hpp) documents that tick() and inbound control-frame handling
    // must share one driving thread (all its mutation is unsynchronized by design, matching the
    // single-threaded LoopbackTransport test double it was proven against); a real caller driving it
    // over this async transport has no other thread-safe way to invoke tick(). Thread-safe (same
    // mutex-guarded post queue `send`/`close_peer` already use).
    void post(std::function<void()> fn) { io_.post(std::move(fn)); }

    // --- diagnostic counters (atomic; read from a test thread after a sync point) ---------------
    [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_.load(); }
    [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(); }
    [[nodiscard]] std::uint64_t drops() const noexcept { return drops_.load(); }
    [[nodiscard]] std::uint64_t dedup_closed() const noexcept { return dedup_closed_.load(); }
    [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_.load(); }
    [[nodiscard]] std::uint64_t connections_open() const noexcept { return conns_open_.load(); }

private:
    // Connecting: TCP connect in flight (dialer) — EPOLLOUT pending. Open: usable, frames flow.
    // Backoff: the socket died and a jittered reconnect timer is armed; the Conn object PERSISTS (with
    // its queued frames) but has no fd — a send queues, a reconnect revives it. There is no "Dead" map
    // entry: a connection we give up on is erased from conns_ entirely.
    enum class St : std::uint8_t { Connecting, Open, Backoff };

    struct Conn {
        NodeId peer{};
        pal::fd_t fd = pal::invalid_fd;
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

        std::vector<std::byte> out;   // concatenated WHOLE frames pending write (FIFO, one sender)
        std::size_t out_sent = 0;     // bytes of `out` already handed to the kernel
        FrameStream in;               // inbound byte-stream reassembler (partial reads / coalescing)
        std::uint32_t backoff_attempt = 0;
        std::uint32_t interest = 0;   // current epoll interest mask (avoid redundant mod_fd)
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
        const std::vector<std::byte> bytes = encode_frame(frame);
        auto& q = outq_[to.value];
        q.insert(q.end(), bytes.begin(), bytes.end());
        frames_sent_.fetch_add(1, std::memory_order_relaxed);
        // Flows now only if this connection has won dedup and is up; otherwise the frame waits in the
        // peer queue until a confirmed connection pumps it (on confirm / on writable / on reconnect).
        if (c->confirmed && c->st == St::Open) pump(c);
    }

    // Move the peer's queued frames onto this (confirmed) connection's socket buffer and flush.
    void pump(Conn* c) {
        auto qit = outq_.find(c->peer.value);
        if (qit != outq_.end() && !qit->second.empty()) {
            c->out.insert(c->out.end(), qit->second.begin(), qit->second.end());
            qit->second.clear();
        }
        flush(c);
    }

    // A connection is dying: return its queued-but-UNSENT whole frames to the FRONT of the peer queue so
    // the next confirmed connection re-sends them. A partially-sent leading frame (out_sent>0) is torn
    // and cannot resume, so that buffer is dead-lettered (010 at-most-once) — the untouched peer queue is
    // unaffected either way.
    void salvage_out(Conn* c) {
        if (!c->out.empty() && c->out_sent == 0) {
            auto& q = outq_[c->peer.value];
            q.insert(q.begin(), c->out.begin(), c->out.end());
        }
        c->out.clear();
        c->out_sent = 0;
    }

    // Get the connection to `to`, lazily DIALING if none exists (021 §2 lazy establishment). Any conn
    // already in the map (Connecting/Open/Backoff) is returned as-is — frames queue on it.
    Conn* ensure_conn(NodeId to) {
        if (auto it = conns_.find(to.value); it != conns_.end()) return it->second.get();
        const auto pit = peers_.find(to.value);
        if (pit == peers_.end()) return nullptr;  // unknown endpoint — caller drops
        return dial(to, pit->second);
    }

    Conn* dial(NodeId to, const Endpoint& ep) {
        auto fd = pal::tcp_connect(ep.addr, ep.port);
        if (!fd) return nullptr;
        auto c = std::make_unique<Conn>();
        c->peer = to;
        c->fd = *fd;
        c->st = St::Connecting;
        c->initiated_by_self = true;
        c->identified = true;  // we know exactly who we dialed
        build_hello(c->hello);
        Conn* raw = c.get();
        conns_[to.value] = std::move(c);
        by_fd_[raw->fd] = raw;
        raw->interest = EPOLLIN | EPOLLOUT;  // EPOLLOUT = connect completion
        io_.add_fd(raw->fd, raw->interest, [this, fd = raw->fd](std::uint32_t ev) { on_conn(fd, ev); });
        return raw;
    }

    // ---- accept path (loop thread) --------------------------------------------------------------
    void on_accept_ready() {
        for (;;) {
            auto fd = pal::accept_one(listener_);
            if (!fd) break;  // would_block (drained) or a transient error — stop this batch
            auto c = std::make_unique<Conn>();
            c->fd = *fd;
            c->st = St::Open;               // TCP already established for an accepted socket
            c->initiated_by_self = false;    // the PEER dialed us
            c->identified = false;           // learn their NodeId from their hello
            build_hello(c->hello);
            Conn* raw = c.get();
            pending_in_[raw->fd] = std::move(c);
            by_fd_[raw->fd] = raw;
            raw->interest = EPOLLIN | EPOLLOUT;  // EPOLLOUT to push our hello out
            io_.add_fd(raw->fd, raw->interest, [this, f = raw->fd](std::uint32_t ev) { on_conn(f, ev); });
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
        // 2) then the FIFO frame buffer (populated by pump() only for a CONFIRMED connection — 021 §2:
        // no application frame flows before the dedup loser is dropped). `c->out` is empty until confirm.
        if (c->confirmed) {
            while (c->out_sent < c->out.size()) {
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
            if (c->out_sent == c->out.size()) {  // fully drained — reclaim and drop EPOLLOUT interest
                c->out.clear();
                c->out_sent = 0;
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
            const bool ok = c->in.feed(p, n, [this, peer = c->peer](MessageFrame f) {
                frames_received_.fetch_add(1, std::memory_order_relaxed);
                (void)peer;
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
            return true;
        }
        Conn* existing = it->second.get();
        if (existing == c) {  // the dialer's own conn confirming the peer it dialed — no rival, it wins
            c->confirmed = true;
            mark_open(c);
            pump(c);
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
            return true;
        }
        // existing wins: close `c`. owned_c (if set) drops here with its fd closed.
        salvage_out(c);  // recover any unsent frames `c` queued before losing (race window)
        pal::close_fd(c->fd);
        by_fd_.erase(c->fd);
        io_.del_fd(c->fd);
        existing->confirmed = true;
        mark_open(existing);
        pump(existing);  // the surviving connection drains the peer queue
        return false;
    }

    // ---- teardown / reconnect -------------------------------------------------------------------
    void handle_dead(Conn* c) {
        const NodeId peer = c->peer;
        const bool was_identified = c->identified;
        salvage_out(c);  // return unsent whole frames to the peer queue BEFORE the fd/state go away
        pal::close_fd(c->fd);
        io_.del_fd(c->fd);
        by_fd_.erase(c->fd);
        if (was_open(c)) unmark_open(c);

        // A pending (never-identified) inbound just disappears.
        if (auto pit = pending_in_.find(c->fd); pit != pending_in_.end()) {
            pending_in_.erase(pit);
            return;
        }
        auto it = (was_identified) ? conns_.find(peer.value) : conns_.end();
        if (it == conns_.end() || it->second.get() != c) return;

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
        }
    }

    void schedule_reconnect(NodeId peer) {
        auto it = conns_.find(peer.value);
        if (it == conns_.end()) return;
        Conn* c = it->second.get();
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
            return;
        }
        const auto pit = peers_.find(peer.value);
        if (pit == peers_.end()) {
            conns_.erase(it);
            outq_.erase(peer.value);
            return;
        }
        auto fd = pal::tcp_connect(pit->second.addr, pit->second.port);
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
    // Detach `existing` from conns_ WITHOUT erasing (the caller re-inserts the survivor), closing its fd.
    void drop_conn_detach(Conn* existing) {
        pal::close_fd(existing->fd);
        io_.del_fd(existing->fd);
        by_fd_.erase(existing->fd);
        if (was_open(existing)) unmark_open(existing);
        // erase the owning entry (it is keyed by peer)
        conns_.erase(existing->peer.value);
    }

    // Fully drop a connection: close fd, deregister, remove from whichever map owns it.
    void drop_conn(Conn* c) {
        pal::close_fd(c->fd);
        io_.del_fd(c->fd);
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
    }
    [[nodiscard]] bool was_open(Conn* c) const { return open_marked_.count(c) != 0; }
    void unmark_open(Conn* c) {
        if (open_marked_.erase(c)) conns_open_.fetch_sub(1, std::memory_order_relaxed);
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

    NodeId self_;
    std::uint64_t bind_addr_;
    std::uint16_t bind_port_;
    pal::fd_t listener_ = pal::invalid_fd;

    pal::IoContext io_;
    std::thread io_thread_;
    std::atomic<bool> started_{false};

    std::function<void(MessageFrame)> receiver_;  // set before start(); read on the loop thread

    // Connection state — all loop-thread-owned. conns_: identified peers (keyed by NodeId). pending_in_:
    // accepted connections whose peer NodeId is not yet known (keyed by fd). by_fd_: raw handler lookup.
    std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;
    std::unordered_map<pal::fd_t, std::unique_ptr<Conn>> pending_in_;
    std::unordered_map<pal::fd_t, Conn*> by_fd_;
    std::unordered_map<std::uint64_t, Endpoint> peers_;  // NodeId → where it listens (for dial/reconnect)
    std::unordered_map<std::uint64_t, std::vector<std::byte>> outq_;  // per-peer pending frames (survives
                                                                      // connection churn — see send_on_loop)
    std::unordered_set<std::uint64_t> reconnect_suppressed_;  // peers SWIM declared dead (close_peer)
    std::unordered_set<Conn*> open_marked_;  // conns counted in conns_open_ (idempotent open/close)

    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> frames_sent_{0};
    std::atomic<std::uint64_t> drops_{0};
    std::atomic<std::uint64_t> dedup_closed_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<std::uint64_t> conns_open_{0};
};

}  // namespace quark::net
