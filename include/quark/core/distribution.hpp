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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "quark/core/actor_ref.hpp"    // LocalRouter, ActorRef, AskFuture (006 send API)
#include "quark/core/audit.hpp"        // AuditSink (020 §Audit)
#include "quark/core/authorizer.hpp"   // Authorizer, SecurityDeadLetter, authorize_at_boundary (020 §3)
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"   // Membership seam + MembershipView
#include "quark/core/metadata.hpp"     // type_key_of / actor_id_of (008 content-addressed keys)
#include "quark/core/placement.hpp"    // place() — HRW winner
#include "quark/core/serialize.hpp"    // encode/decode_tagged, Described (016 top-level)
#include "quark/core/transport.hpp"    // Transport seam + MessageFrame
#include "quark/core/wire.hpp"         // negotiate(), WireMode, PeerSchema, tagless codec (016)
#include "quark/detail/hash.hpp"       // hash_combine (inbound registry key)
#include "quark/detail/rate_bucket.hpp"
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
          // F17 safe default: consult the transport's AUTHENTICATED post-handshake schema advert.
          // No advert (plaintext/unknown) ⇒ tagged. A peer advertises the exact local TypeKey and
          // ABI only when it can decode this schema; otherwise no tagless. `set_peer_schema_provider`
          // remains an explicit override for tests/embedded transports.
          peer_schema_([this](NodeId owner, TypeKey tk) -> PeerSchema {
              // Boolean query: no per-send copy of the peer's advertisement (review E1).
              if (!transport_->peer_accepts_type(owner, tk.value, pal::platform_abi_tag))
                  return PeerSchema{~0ull, ~0u};
              return PeerSchema{tk.value, pal::platform_abi_tag};
          }) {
        transport_->on_receive([this](MessageFrame f) { deliver(f); });
    }

    DistributedRouter(const DistributedRouter&) = delete;
    DistributedRouter& operator=(const DistributedRouter&) = delete;

    [[nodiscard]] NodeId self() const noexcept { return self_; }

    // ADR-046: frames dropped locally by the cross-node congestion gate before ever reaching the
    // transport (mirrors GovernanceCore::sheds' observability shape for the local mailbox case).
    [[nodiscard]] std::uint64_t remote_sheds() const noexcept {
        return remote_sheds_.load(std::memory_order_relaxed);
    }

    // Per-peer inbound admission (F15): token bucket to stop one peer from filling the actor's
    // mailbox faster than it drains. Default 4096 frames/s with a 1024-frame burst; 0 disables.
    // Buckets are hashed into a fixed table, so spoofed plaintext ids share limits rather than
    // growing memory. Raise this for a legitimate high-throughput data-plane peer.
    void set_peer_budget(std::uint64_t frames_per_sec, std::uint64_t burst) noexcept {
        std::lock_guard<std::mutex> g(budget_mu_);
        peer_rate_per_sec_ = frames_per_sec;
        peer_burst_ = burst == 0 ? 1 : burst;
    }
    [[nodiscard]] std::uint64_t peer_sheds() const noexcept {
        return peer_sheds_.load(std::memory_order_relaxed);
    }

    using ClockFn = std::int64_t (*)(void* ctx) noexcept;  // mirrors Activation::set_clock (014)

    // --- Boundary authorization (020 §3): gate an inbound frame BEFORE it is deserialized/dispatched.
    // DENY BY DEFAULT: an unset authorizer drops every inbound data frame. A deployment that wants
    // allow-all must install AllowAllAuthorizer explicitly. `sec_dl`/`audit` are optional; a denial
    // with both null is still refused, just unrecorded. `clock`/`clock_ctx` timestamp a denial for
    // the sinks above; a null clock timestamps denials as 0.
    void set_authorizer(const Authorizer& authz, SecurityDeadLetter* sec_dl = nullptr,
                        const AuditSink* audit = nullptr, ClockFn clock = nullptr,
                        void* clock_ctx = nullptr) noexcept {
        authz_ = &authz;
        sec_dl_ = sec_dl;
        audit_ = audit;
        clock_fn_ = clock;
        clock_ctx_ = clock_ctx;
    }

    // --- Principal binding (020 §3 / F2): the Principal in a frame is chosen by the SENDER. The
    // transport authenticates the NODE (mTLS), not the principal. With no policy installed, a
    // non-anonymous wire principal is DROPPED (fail closed); anonymous frames still reach the
    // Authorizer. A policy maps the authenticated peer NodeId + claimed principal to the effective
    // principal (or nullopt to reject). Install one only when the peer is authorized to speak for
    // that principal (e.g., an ingress gateway with a signed user assertion).
    using PrincipalPolicy =
        std::function<std::optional<Principal>(NodeId authenticated_peer, const Principal& claimed)>;
    void set_principal_policy(PrincipalPolicy policy) { principal_policy_ = std::move(policy); }

    // Resolve identity → a distributed ref (identity + placement context). Never blocks, never
    // touches the network — placement happens at send time against the then-current view.
    template <class A>
    [[nodiscard]] DistRef<A> get(std::uint64_t key) noexcept {
        return DistRef<A>{actor_id_of<A>(key), this};
    }

    // Test/harness seam: send to a specific owner, bypassing HRW placement. Production routing
    // uses `get<A>(key).tell(...)`; this pins a ledger node in the live security lab when the
    // cluster has more nodes than the actor's HRW owner would pick.
    template <class A, class M>
    void tell_on(NodeId owner, std::uint64_t key, M&& m) {
        send_remote<A>(owner, actor_id_of<A>(key), std::forward<M>(m));
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
        // F17: publish the message TypeKey this node can decode. A secure transport advertises it
        // (with the platform ABI) to authenticated peers; the sender takes tagless only on an exact
        // key+ABI match. The type key IS the 016 schema fingerprint, so a layout change changes it.
        if (!local_schema_.contains(type_key_of<M>().value))
            local_schema_.types.push_back(type_key_of<M>().value);
        local_schema_.abi_tag = pal::platform_abi_tag;
        transport_->set_local_schema(local_schema_);
    }

    // --- tell (010): place, then LOCAL fast path or REMOTE serialize+transport. -----------------
    template <class A, class M>
    void tell(ActorId id, M&& m) {
        using Msg = std::remove_cvref_t<M>;
        static_assert(Handles<A, Msg>, "tell: message type is not in the actor's protocol (unhandled)");
        static_assert(Described<Msg>,
                      "tell (distributed): a routable message needs QUARK_SERIALIZE (016) — 008 "
                      "Validation requires a Serializer for any remotely-sent type");
        const MembershipView v = membership_->placement_view();
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
        // The transport contract says only frames addressed to THIS node reach us; enforce it here
        // too (a peer could otherwise aim a frame at another NodeId and have it applied locally).
        if (f.to != self_) return;
        // F15: per-peer admission before authz/decode/posting. A slow actor can no longer be
        // buried by one peer's burst; the bucket is bounded and fixed-size.
        if (!admit_peer(f.from, rate_now_ns())) {
            peer_sheds_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Boundary authorization (020 §3) runs FIRST — before the (target,msg) lookup and well before
        // decode — so a denied frame is never deserialized and never touches actor state.
        // Deny by default: a DistributedRouter with no configured Authorizer is a fail-open hole
        // (the old `authz_ != nullptr && ...` accepted every frame). Apps that really want
        // allow-all must install AllowAllAuthorizer explicitly.
        if (authz_ == nullptr) return;
        // F2: bind the claimed principal to the authenticated peer before the ACL sees it.
        Principal effective = f.principal;
        if (!effective.anonymous() || principal_policy_) {
            if (!principal_policy_) return;  // no binder configured: never trust a wire principal
            std::optional<Principal> bound = principal_policy_(f.from, effective);
            if (!bound) return;
            effective = *bound;
        }
        if (!authorize_at_boundary(*authz_, effective, f.target, f.msg_type, sec_dl_, audit_,
                                   now_ns())) {
            return;
        }
        const auto it = inbound_.find(wire_key(f.target.type, f.msg_type));
        if (it == inbound_.end()) return;  // no such (actor,msg) type registered here → drop (007 seam)
        it->second(*local_, f, effective);
    }

    // --- Test / 021 seam: opt into the 016 tagless fast path by returning the peer's REAL schema.
    // The default is TAGGED (mismatched placeholder) — tagless is never assumed. A real connect
    // handshake must advertise the peer's PeerSchema; a rolling-upgrade mismatch then falls back to
    // tagged automatically. `matched_peer_schema(tk)` is the explicit "same binary" opt-in.
    void set_peer_schema_provider(std::function<PeerSchema(NodeId, TypeKey)> fn) {
        peer_schema_ = std::move(fn);
    }

    [[nodiscard]] static PeerSchema matched_peer_schema(TypeKey tk) noexcept {
        return PeerSchema{tk.value, pal::platform_abi_tag};
    }

    [[nodiscard]] NodeId owner_of(ActorId id) const noexcept {
        const std::optional<NodeId> o = place(id, membership_->placement_view());
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

    using InboundFn = void (*)(LocalRouter& local, const MessageFrame& f, const Principal& effective);

    // The per-(A,M) inbound thunk: decode the 016 bytes per the frame's negotiated mode, then hand
    // the typed message to the local router with the wire-propagated deadline/trace and the
    // principal bound by the policy. A malformed tagged stream is dropped.
    template <class A, class M>
    static void inbound_thunk(LocalRouter& local, const MessageFrame& f, const Principal& effective) {
        M msg{};
        if (f.mode == WireMode::Tagless) {
            // A peer controls its own plaintext and can force Tagless; never run the unchecked
            // decoder on wire bytes. A truncated/oversized declared length is dropped here.
            if (!decode_tagless_checked(f.payload.data(), f.payload.size(), msg)) return;
        } else {
            if (!decode_tagged(f.payload.data(), f.payload.size(), msg)) return;  // malformed → drop
        }
        local.template deliver_from_wire<A, M>(f.target, std::move(msg), f.deadline_ns, f.trace_id,
                                               effective);
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
            f.principal = amb->principal;  // 020 §3: inherit unchanged across the hop (see principal.hpp)
        }
        if (mode == WireMode::Tagless) {
            f.payload.resize(tagless_size(m));
            (void)encode_tagless(m, f.payload.data());
        } else {
            const std::size_t sz = detail::tagged_object_size(m);
            f.payload.resize(sz);
            if (!encode_tagged(m, f.payload.data(), sz)) return;  // sized exactly → cannot fail
        }
        // ADR-046 cross-node backpressure: the sender-side admission gate, checked BEFORE a frame is
        // handed to the transport at all — a congested peer is locally shed here, never queued onto
        // the wire. The transport supplies its own clock internally (no dependency on this router's
        // own, separately-unwired, `clock_fn_` seam below).
        if (!transport_->admit_send(owner)) {
            remote_sheds_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        transport_->send(owner, std::move(f));
    }

    [[nodiscard]] std::int64_t now_ns() const noexcept { return clock_fn_ ? clock_fn_(clock_ctx_) : 0; }

    [[nodiscard]] std::int64_t rate_now_ns() const noexcept {
        if (clock_fn_) return clock_fn_(clock_ctx_);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   pal::now().time_since_epoch())
            .count();
    }

    using PeerBudget = detail::RateBucket;
    static constexpr std::size_t kBudgetSlots = 256;

    [[nodiscard]] bool admit_peer(NodeId peer, std::int64_t now) noexcept {
        // Fixed-size hashed buckets: no per-NodeId allocation from a spoofed-id flood.
        const std::size_t idx = static_cast<std::size_t>(
            (peer.value * 0x9E3779B97F4A7C15ULL) >> 56) & (kBudgetSlots - 1);
        std::lock_guard<std::mutex> g(budget_mu_);
        if (peer_rate_per_sec_ == 0) return true;
        return peer_budgets_[idx].admit(now, peer_rate_per_sec_, peer_burst_);
    }

    Membership* membership_;
    NodeId self_;
    LocalRouter* local_;
    Transport* transport_;
    std::atomic<std::uint64_t> remote_sheds_{0};  // ADR-046: frames locally shed by admit_send()
    std::atomic<std::uint64_t> peer_sheds_{0};    // F15: frames shed by the per-peer token bucket
    mutable std::mutex budget_mu_;
    std::array<PeerBudget, kBudgetSlots> peer_budgets_{};
    std::uint64_t peer_rate_per_sec_ = 4096;
    std::uint64_t peer_burst_ = 1024;
    std::function<PeerSchema(NodeId, TypeKey)> peer_schema_;
    SchemaAdvertisement local_schema_;  // F17: what this node advertises to authenticated peers
    PrincipalPolicy principal_policy_;
    std::unordered_map<WireKey, InboundFn, WireKeyHash> inbound_;
    const Authorizer* authz_ = nullptr;
    SecurityDeadLetter* sec_dl_ = nullptr;
    const AuditSink* audit_ = nullptr;
    ClockFn clock_fn_ = nullptr;
    void* clock_ctx_ = nullptr;
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
