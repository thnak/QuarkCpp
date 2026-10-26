// Implements ADR-048 §Amendment (issue #14) — relay shape C: the SEALED-HOP relay. Shape B forwards
// transit frames below the security layer, so the origin<->relay hop carries the inner frame's header
// (from/to/kind/target/msg_type) in the clear. Shape C removes that: the inner frame is encoded into the
// payload of an outer frame addressed to the relay and sent through the HOP `SecureTransport`, so a wire
// observer sees only {origin, relay} and an opaque sealed blob; the relay opens it (it holds a session
// with the origin), checks it, and re-wraps it for the destination over ITS session with the destination.
//
// Stack on a spoke:  e2e SecureTransport(A<->B) -> SealedRelayTransport -> hop SecureTransport(A<->R) -> Tcp
// Stack on a relay:  SealedRelayTransport (forwarding on) -> hop SecureTransport -> Tcp
//
// Identity: the relay learns the origin from its AUTHENTICATED hop session (outer.from), not a HELLO
// claim, and refuses an inner frame whose `from` differs (no laundering). A node accepts a transit
// delivery (outer.from != inner.from) only from a relay it was configured/discovered to use, so an
// authenticated-but-unrelated peer cannot inject frames claiming another node's identity. Admission is
// the relay's `admit(origin, dest)` (fail-closed). Ordering: a peer uses the relay only if it has no
// direct route configured, exactly as shape B (set_relay_via).
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "quark/core/transport.hpp"
#include "quark/net/wire_codec.hpp"

namespace quark::net {

// Reserved TypeKey marking a relay envelope (outer frame's msg_type). Chosen outside any user range.
inline constexpr std::uint64_t kRelayEnvelopeType = 0x52454C4159454E56ULL;  // "RELAYENV"

class SealedRelayTransport final : public Transport {
public:
    SealedRelayTransport(Transport& hop, NodeId self) noexcept : hop_(&hop), self_(self) {}

    // Spoke side: reach `peer` through this ordered relay list (first entry is used; the list is also
    // the trusted-relay set for inbound transit deliveries). Call before traffic.
    void set_relay_via(NodeId peer, std::vector<NodeId> relays) {
        std::lock_guard<std::mutex> g(mu_);
        for (NodeId r : relays) trusted_.insert(r.value);
        via_[peer.value] = std::move(relays);
    }
    // Relay side: forward envelopes for peers `admit(origin, dest)` allows. Fail-closed when unset.
    void set_forwarding(std::function<bool(NodeId origin, NodeId dest)> admit) {
        std::lock_guard<std::mutex> g(mu_);
        admit_ = std::move(admit);
        forwarding_ = true;
    }
    [[nodiscard]] std::uint64_t relayed() const noexcept { return relayed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

    void send(NodeId to, MessageFrame frame) override {
        NodeId relay{};
        {
            std::lock_guard<std::mutex> g(mu_);
            const auto it = via_.find(to.value);
            if (it != via_.end() && !it->second.empty()) relay = it->second.front();
        }
        if (relay.value == 0) {
            hop_->send(to, std::move(frame));
            return;
        }
        hop_->send(relay, wrap(relay, frame));
    }

    void on_receive(std::function<void(MessageFrame)> cb) override {
        cb_ = std::move(cb);
        hop_->on_receive([this](MessageFrame f) { on_hop_frame(std::move(f)); });
    }

    // Everything else is the hop's: security posture, schema, congestion all describe the real wire.
    [[nodiscard]] bool is_plaintext() const noexcept override { return hop_->is_plaintext(); }
    void set_authenticated_transport(bool a) noexcept override { hop_->set_authenticated_transport(a); }
    [[nodiscard]] bool admit_send(NodeId peer) noexcept override { return hop_->admit_send(peer); }
    void mark_congested(NodeId peer, std::int64_t remaining_ns) noexcept override {
        hop_->mark_congested(peer, remaining_ns);
    }

    // The inbound seam: what the hop's receiver calls for every frame the hop delivers. Public so a test
    // can present a frame as if a given (authenticated) hop peer had sent it.
    void deliver_from_hop(MessageFrame f) { on_hop_frame(std::move(f)); }

private:
    [[nodiscard]] MessageFrame wrap(NodeId to, const MessageFrame& inner) const {
        MessageFrame outer{};
        outer.from = self_;
        outer.to = to;
        outer.msg_type = TypeKey{kRelayEnvelopeType};
        // Keep the Control lane for control traffic; handshake/schema/data ride the data lane (the hop
        // SecureTransport owns Authenticate for ITS OWN handshake, so inner ones must not use it).
        outer.kind = inner.kind == FrameKind::Control ? FrameKind::Control : FrameKind::Data;
        encode_frame_body(inner, outer.payload);
        return outer;
    }

    void on_hop_frame(MessageFrame f) {
        if (f.msg_type.value != kRelayEnvelopeType || f.kind == FrameKind::Authenticate ||
            f.kind == FrameKind::Schema) {
            if (cb_) cb_(std::move(f));  // not an envelope: an ordinary hop-local frame for us
            return;
        }
        auto inner = decode_frame_body(f.payload);
        if (!inner) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const NodeId origin = f.from;  // authenticated by the hop session
        if (inner->to == self_) {
            bool trusted = inner->from == origin;  // direct (no transit): origin is the claimed sender
            if (!trusted) {
                std::lock_guard<std::mutex> g(mu_);
                trusted = trusted_.count(origin.value) != 0;  // transit: only from a configured relay
            }
            if (!trusted) {
                refused_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cb_) cb_(std::move(*inner));
            return;
        }
        std::function<bool(NodeId, NodeId)> admit;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (forwarding_) admit = admit_;
        }
        if (!admit || inner->from != origin || !admit(origin, inner->to)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        relayed_.fetch_add(1, std::memory_order_relaxed);
        const NodeId dest = inner->to;
        hop_->send(dest, wrap(dest, *inner));  // re-sealed under the relay<->dest hop session
    }

    Transport* hop_;
    NodeId self_;
    std::function<void(MessageFrame)> cb_;
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::vector<NodeId>> via_;
    std::unordered_set<std::uint64_t> trusted_;
    std::function<bool(NodeId, NodeId)> admit_;
    bool forwarding_ = false;
    std::atomic<std::uint64_t> relayed_{0};
    std::atomic<std::uint64_t> refused_{0};
};

}  // namespace quark::net
