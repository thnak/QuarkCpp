// Implements 010-Distribution §"Transport seam" — the byte-moving SEAM plus a std-only in-process
// loopback transport so cross-node routing is testable deterministically without a socket.
//
// THE SEAM (`Transport`): fire-and-forget `send(NodeId, MessageFrame)` + an `on_receive` sink the
// distributed router attaches its inbound dispatch to. A `MessageFrame` is the wire unit: destination
// identity + the 016-serialized payload + just enough header to decode and re-post it on the far side
// (target ActorId, the message TypeKey, the negotiated WireMode, and the propagated 018 deadline /
// 009 trace).
//
// WHAT LIVES BEHIND IT (NOT here — a 019-PAL-dependent adapter, explicitly NOT core):
//   * The real default transport is plain TCP with length-prefixed frames, one multiplexed
//     connection per peer, over the per-OS event loop the PAL supplies (epoll/io_uring on Linux,
//     kqueue on BSD/macOS, IOCP on Windows) — written against the PAL readiness/completion interface,
//     never a raw OS API, and never asio/gRPC. That adapter is 019/021 work; it does NOT belong in
//     any core translation unit and is not implemented here.
//   * The connection MECHANICS (lazy dial on first cross-node send, lower-NodeId-wins dial dedup,
//     SWIM-ping-reused keepalive, jittered reconnect) are 021. The secure/authenticated wrapping is
//     020. This seam is deliberately ignorant of all of it.
//
// `LoopbackFabric` + `LoopbackTransport` below are the TEST double: a `send` hands the frame straight
// to the destination node's receiver on the caller's thread, in order — so a single sender's frames
// arrive FIFO by construction (the property the real one-connection-per-peer transport must also
// give, ADR-011). Determinism, not performance, is its job.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/principal.hpp"  // Principal — the ambient identity carried across a node↔node hop
#include "quark/core/wire.hpp"  // WireMode (016 negotiation result carried on the frame)

namespace quark {

// Frame class discriminator (021 §"Control-plane framing"). A `Data` frame is the 010 actor-delivery
// unit — the ONLY kind the 010 data path ever produces (`send_remote` leaves this defaulted). A
// `Control` frame carries a 021 SWIM/lifecycle control message in `payload` (its own std-only codec,
// NOT a 016 actor message; `target`/`msg_type`/`mode` are unused for it). Adding this trailing,
// defaulted field is byte-for-byte behavior-preserving for 010: every existing producer/consumer of a
// data frame leaves it `Data`, the local fast path never constructs a frame at all, and the field sits
// AFTER `payload` so no positional aggregate-init exists to disturb (verified: all call sites assign
// field-by-field). A node running both planes demuxes inbound frames on this field (see
// `control_data_demux` in cluster.hpp) — the data sink sees a `Data`-only stream, unchanged.
enum class FrameKind : std::uint8_t { Data = 0, Control = 1 };

// The wire unit handed to a Transport. Owns its serialized bytes; movable. The header is everything
// the receiving node needs to (a) find the target actor, (b) decode the payload into the right type,
// and (c) reconstruct the propagated deadline/trace — WITHOUT a schema exchange per message (that is
// the connect-time negotiation, 016; the per-frame `mode` records its outcome).
struct MessageFrame {
    NodeId from{};                     // origin node (FIFO-per-sender / reply routing seam)
    NodeId to{};                       // destination node (placement winner)
    ActorId target{};                  // destination actor identity (006 addressing)
    TypeKey msg_type{};                // the message's 016 content key (inbound decode dispatch)
    WireMode mode = WireMode::Tagged;  // negotiated encoding of `payload` (016)
    std::int64_t deadline_ns = 0;      // propagated absolute deadline (018); 0 = none
    std::uint64_t trace_id = 0;        // propagated trace correlation id (009)
    std::vector<std::byte> payload;    // the 016-serialized message body
    FrameKind kind = FrameKind::Data;  // 021 data/control discriminator (defaulted ⇒ 010 path intact)
    Principal principal{};             // 020 ambient identity carried across the boundary; default =
                                       // anonymous. Stamped ATTENUATED from the sender's inbound context
                                       // at the wire edge; re-established as the ambient on receive.
};

// The transport seam (010). Fire-and-forget by contract; ordering/at-most-once are the transport's
// to provide per (sender,receiver) over its one connection (006/010 delivery table). The real
// adapter adds connection lifecycle + backpressure signalling (010 open question) behind this.
class Transport {
public:
    virtual ~Transport() = default;

    // Hand `frame` toward node `to`. Fire-and-forget: no delivery result here (a peer-down failure
    // surfaces as an `ask` error / local dead-letter per the 010 delivery table, not a return code).
    virtual void send(NodeId to, MessageFrame frame) = 0;

    // Register the inbound sink for THIS node's transport endpoint. Frames addressed to this node are
    // handed to `cb` (the distributed router's `deliver`).
    virtual void on_receive(std::function<void(MessageFrame)> cb) = 0;
};

// ============================================================================================
// In-process loopback (test double). A shared switchboard of NodeId → receiver. `send` dispatches
// the frame to the destination's receiver INLINE on the caller's thread — so a single sender's
// frames arrive in send order (FIFO), and the whole cross-node path is deterministic and reentrant-
// safe (the receiver's `engine.post` is an ordinary cross-thread producer edge).
// ============================================================================================
class LoopbackFabric {
public:
    void attach(NodeId n, std::function<void(MessageFrame)> receiver) {
        std::lock_guard<std::mutex> g(mu_);
        receivers_[n] = std::move(receiver);
    }

    void detach(NodeId n) {
        std::lock_guard<std::mutex> g(mu_);
        receivers_.erase(n);
    }

    // Deliver inline. A frame to an unattached node is dropped (peer down / not yet joined) — the
    // 010 table's "tell dead-lettered locally" degrades here to a silent drop at the fabric edge,
    // which the sender already treated as fire-and-forget. Count observed for tests.
    void send(NodeId to, MessageFrame frame) {
        std::function<void(MessageFrame)> cb;
        {
            std::lock_guard<std::mutex> g(mu_);
            ++sends_;
            const auto it = receivers_.find(to);
            if (it == receivers_.end()) {
                ++drops_;
                return;
            }
            cb = it->second;  // copy the target's receiver, then release the lock before dispatch
        }
        cb(std::move(frame));  // inline, in-order delivery (FIFO for a single sender)
    }

    [[nodiscard]] std::uint64_t sends() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return sends_;
    }
    [[nodiscard]] std::uint64_t drops() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return drops_;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<NodeId, std::function<void(MessageFrame)>> receivers_;
    std::uint64_t sends_ = 0;
    std::uint64_t drops_ = 0;
};

// One node's endpoint on a `LoopbackFabric`.
class LoopbackTransport final : public Transport {
public:
    LoopbackTransport(LoopbackFabric& fabric, NodeId self) noexcept
        : fabric_(&fabric), self_(self) {}

    void send(NodeId to, MessageFrame frame) override { fabric_->send(to, std::move(frame)); }

    void on_receive(std::function<void(MessageFrame)> cb) override {
        fabric_->attach(self_, std::move(cb));
    }

private:
    LoopbackFabric* fabric_;
    NodeId self_;
};

}  // namespace quark
