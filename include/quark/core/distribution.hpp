// Implements 010-Distribution §"Placement" + §"Delivery semantics across the network" — the
// distributed routing layer that sits ON the verified single-node core (006 addressing, 002 engine,
// 016 serialization) and extends `tell` across a cluster through the three seams: `Membership`
// (membership.hpp), placement (placement.hpp), and `Transport` (transport.hpp).
//
// THE ROUTING DECISION (per send, pure function of the current membership view):
//   owner = place(ActorId, membership.view())
//   owner == self (or no cluster)  → LOCAL FAST PATH: the existing 006 LocalRouter delivery, byte for
//                                    byte — no placement wire, NO serialization, 0 hot-path alloc.
//   owner == a remote node         → serialize via 016 (negotiate() → tagless fast path between
//                                    matched peers, else canonical tagged) and hand the frame to the
//                                    Transport. The far node decodes and re-posts it as an ordinary
//                                    local delivery (LocalRouter::deliver_from_wire), so per-actor
//                                    FIFO and the single-executor invariant hold end to end.
//
// ZERO-COST-WHEN-UNUSED (010 normative): a single-node build never constructs a DistributedRouter, so
// none of this touches the local hot path — `LocalRouter`/`ActorRef` are unchanged. `DistRef<A>` is
// the distributed analogue of `ActorRef<A>`; keeping it a DISTINCT type (rather than branching inside
// `LocalRouter::tell`) is what keeps that guarantee literal — the local send never pays a "am I
// distributed?" check. Unifying them behind a 013 config policy is a documented future seam.
//
// SEAMS LEFT EXPLICIT:
//   * 021 — real SWIM behind `Membership`; connection mechanics behind `Transport`.
//   * 025 — weighted / capability-constrained placement specializes `place()` (uniform HRW here).
//   * 026 — the O(1) `VirtualBins` cache in front of `place()`; relay path-pinning for FIFO at scale.
//   * 017/021 — CROSS-NODE ASK reply-routing (a reply frame correlated back to the origin ReplyCell).
//     Not built here: `ask` resolves through the local router (self-owned replies normally; a
//     remote-owned target fails the ask fast, matching the 010 "peer → ask fails & escalates" row).
//   * 019 — the real socket/event-loop transport adapter (the loopback here is a test double).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

#include "quark/core/actor_ref.hpp"    // LocalRouter, ActorRef, AskFuture (006 send API)
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"   // Membership seam + MembershipView
#include "quark/core/metadata.hpp"     // type_key_of / actor_id_of (008 content-addressed keys)
#include "quark/core/placement.hpp"    // place() — HRW winner
#include "quark/core/serialize.hpp"    // encode/decode_tagged, Described (016 top-level)
#include "quark/core/transport.hpp"    // Transport seam + MessageFrame
#include "quark/core/wire.hpp"         // negotiate(), WireMode, PeerSchema, tagless codec (016)
#include "quark/detail/hash.hpp"       // hash_combine (inbound registry key)
#include "pal/pal.hpp"                 // platform_abi_tag

namespace quark {

template <class A>
class DistRef;

// ============================================================================================
// DistributedRouter — placement-aware routing over the three seams. One per node.
// ============================================================================================
class DistributedRouter {
public:
    // `local` delivers self-owned sends (the 006 fast path); `membership` decides ownership;
    // `transport` carries remote frames. The router attaches its inbound dispatch to the transport.
    DistributedRouter(Membership& membership, LocalRouter& local, Transport& transport)
        : membership_(&membership),
          self_(membership.self()),
          local_(&local),
          transport_(&transport),
          peer_schema_([](NodeId, TypeKey tk) noexcept { return default_peer_schema(tk); }) {
        transport_->on_receive([this](MessageFrame f) { deliver(f); });
    }

    DistributedRouter(const DistributedRouter&) = delete;
    DistributedRouter& operator=(const DistributedRouter&) = delete;

    [[nodiscard]] NodeId self() const noexcept { return self_; }

    // Resolve identity → a distributed ref (identity + placement context). Never blocks, never
    // touches the network — placement happens at send time against the then-current view.
    template <class A>
    [[nodiscard]] DistRef<A> get(std::uint64_t key) noexcept {
        return DistRef<A>{actor_id_of<A>(key), this};
    }

    // Register message type `M` (in actor `A`'s protocol) as remotely receivable ON THIS NODE: wires
    // the inbound decode+post thunk so a frame for (A, M) that lands here is decoded and delivered.
    // Every node that may HOST an `A` must register its remotely-sent messages (008 Validation: a
    // remotely-sent type needs a Serializer). Cold, setup-time.
    template <class A, class M>
    void register_remote() {
        static_assert(Handles<A, M>, "register_remote<A,M>: M is not in A's protocol (unhandled)");
        static_assert(Described<M>,
                      "register_remote<A,M>: a remotely-received message needs QUARK_SERIALIZE (016)");
        inbound_[wire_key(type_key_of<A>(), type_key_of<M>())] = &inbound_thunk<A, M>;
    }

    // --- tell (010): place, then LOCAL fast path or REMOTE serialize+transport. -----------------
    template <class A, class M>
    void tell(ActorId id, M&& m) {
        using Msg = std::remove_cvref_t<M>;
        static_assert(Handles<A, Msg>, "tell: message type is not in the actor's protocol (unhandled)");
        static_assert(Described<Msg>,
                      "tell (distributed): a routable message needs QUARK_SERIALIZE (016) — 008 "
                      "Validation requires a Serializer for any remotely-sent type");
        const MembershipView v = membership_->view();
        const std::optional<NodeId> owner = place(id, v);
        if (!owner || *owner == self_) {
            // LOCAL FAST PATH — identical to a single-node send: no serialization, 0 hot-path alloc.
            local_->template tell<A>(id, std::forward<M>(m));
            return;
        }
        send_remote<A, Msg>(*owner, id, m);  // REMOTE — 016 serialize + Transport
    }

    // --- ask (010): resolves through the local router; cross-node reply routing is a 017/021 seam. --
    template <class A, class R, class Q>
    [[nodiscard]] AskFuture<R> ask(ActorId id, Q&& q) {
        return local_->template ask<A, R>(id, std::forward<Q>(q));
    }

    // --- Inbound (Transport receiver): decode a frame and re-post it as a local delivery. ---------
    void deliver(const MessageFrame& f) {
        const auto it = inbound_.find(wire_key(f.target.type, f.msg_type));
        if (it == inbound_.end()) return;  // no such (actor,msg) type registered here → drop (007 seam)
        it->second(*local_, f);
    }

    // --- Test / 021 seam: override the per-peer schema (default assumes a matched same-binary peer,
    // i.e. the 016 tagless fast path). A real connect handshake advertises the peer's PeerSchema;
    // a rolling-upgrade mismatch then makes negotiate() fall back to tagged automatically.
    void set_peer_schema_provider(std::function<PeerSchema(NodeId, TypeKey)> fn) {
        peer_schema_ = std::move(fn);
    }

    [[nodiscard]] NodeId owner_of(ActorId id) const noexcept {
        const std::optional<NodeId> o = place(id, membership_->view());
        return o ? *o : self_;
    }
    [[nodiscard]] bool is_local(ActorId id) const noexcept { return owner_of(id) == self_; }

private:
    // The inbound registry key: (actor TypeKey, message TypeKey). A frame carries both (target.type
    // and msg_type), so the same M in two actors' protocols dispatches to the right thunk.
    struct WireKey {
        std::uint64_t actor = 0;
        std::uint64_t msg = 0;
        friend bool operator==(const WireKey&, const WireKey&) = default;
    };
    struct WireKeyHash {
        std::size_t operator()(const WireKey& k) const noexcept {
            return static_cast<std::size_t>(detail::hash_combine(k.actor, k.msg));
        }
    };
    static WireKey wire_key(TypeKey actor, TypeKey msg) noexcept {
        return WireKey{actor.value, msg.value};
    }

    using InboundFn = void (*)(LocalRouter& local, const MessageFrame& f);

    // The per-(A,M) inbound thunk: decode the 016 bytes per the frame's negotiated mode, then hand
    // the typed message to the local router with the wire-propagated deadline/trace. A malformed
    // tagged stream is dropped (defense-in-depth; tagless trusts the connect gate, 016).
    template <class A, class M>
    static void inbound_thunk(LocalRouter& local, const MessageFrame& f) {
        M msg{};
        if (f.mode == WireMode::Tagless) {
            (void)decode_tagless(f.payload.data(), msg);  // UNCHECKED — matched-peer gate (016)
        } else {
            if (!decode_tagged(f.payload.data(), f.payload.size(), msg)) return;  // malformed → drop
        }
        local.template deliver_from_wire<A, M>(f.target, std::move(msg), f.deadline_ns, f.trace_id);
    }

    // Serialize `m` (016) and hand the frame to the transport. NOT the zero-alloc local hot path —
    // cross-node serialization inherently allocates a byte buffer; the local fast path pays none of
    // this. The propagated deadline (018) / trace (009) ride the frame.
    template <class A, class M>
    void send_remote(NodeId owner, ActorId id, const M& m) {
        const TypeKey mk = type_key_of<M>();
        const WireMode mode = negotiate<M>(peer_schema_(owner, mk));
        MessageFrame f;
        f.from = self_;
        f.to = owner;
        f.target = id;
        f.msg_type = mk;
        f.mode = mode;
        if (const MessageContext* amb = detail::tl_current_ctx) {
            f.trace_id = amb->trace_id;
            f.deadline_ns = amb->deadline_ns;
        }
        if (mode == WireMode::Tagless) {
            f.payload.resize(tagless_size(m));
            (void)encode_tagless(m, f.payload.data());
        } else {
            const std::size_t sz = detail::tagged_object_size(m);
            f.payload.resize(sz);
            if (!encode_tagged(m, f.payload.data(), sz)) return;  // sized exactly → cannot fail
        }
        transport_->send(owner, std::move(f));
    }

    // A peer we know nothing specific about is assumed to run the same binary: matched fingerprint
    // (for a Described type, type_key == 016 fingerprint) + matched ABI ⇒ negotiate() picks tagless.
    static PeerSchema default_peer_schema(TypeKey tk) noexcept {
        return PeerSchema{tk.value, pal::platform_abi_tag};
    }

    Membership* membership_;
    NodeId self_;
    LocalRouter* local_;
    Transport* transport_;
    std::function<PeerSchema(NodeId, TypeKey)> peer_schema_;
    std::unordered_map<WireKey, InboundFn, WireKeyHash> inbound_;
};

// ============================================================================================
// DistRef<A> — the distributed analogue of ActorRef<A>: identity + a DistributedRouter delivery
// context. Cheap to copy, always typed (unhandled send = compile error via Handles<A,M>).
// ============================================================================================
template <class A>
class DistRef {
public:
    using actor_type = A;

    DistRef() noexcept = default;
    DistRef(ActorId id, DistributedRouter* router) noexcept : id_(id), router_(router) {}

    [[nodiscard]] ActorId id() const noexcept { return id_; }
    [[nodiscard]] bool valid() const noexcept { return router_ != nullptr; }

    // tell — placement-aware fire-and-forget (local fast path or remote transport, decided per send).
    template <class M>
    void tell(M&& m) const {
        router_->template tell<A>(id_, std::forward<M>(m));
    }

    // ask<R>(q) — request/reply (local today; cross-node reply routing is a 017/021 seam).
    template <class R, class Q>
    [[nodiscard]] AskFuture<R> ask(Q&& q) const {
        return router_->template ask<A, R>(id_, std::forward<Q>(q));
    }

    friend bool operator==(const DistRef& a, const DistRef& b) noexcept { return a.id_ == b.id_; }

private:
    ActorId id_{};
    DistributedRouter* router_ = nullptr;
};

}  // namespace quark

template <class A>
struct std::hash<quark::DistRef<A>> {
    std::size_t operator()(const quark::DistRef<A>& r) const noexcept { return r.id().hash(); }
};
