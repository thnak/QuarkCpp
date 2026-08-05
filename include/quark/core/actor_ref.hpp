// Implements 006-Messaging-and-Addressing §Actor references / §Send verbs + ADR-007 (always-typed
// refs, async-only ask, ReplyCell win-arbitration). This is the send API sitting on top of the
// 001/002/003 foundation.
//
//   quark::ActorRef<Order> o = router.get<Order>(key);   // identity + placement; never blocks
//   o.tell(Ship{...});                                    // fire-and-forget, 0 hot-path alloc
//   auto r = quark::block_on(o.ask<Confirmation>(Query{...}));   // off-lane request/reply
//
// KEY DECISIONS (ADR-007, normative):
//   * `ActorRef<A>` is ALWAYS typed — no untyped/dynamic ref on the send path, so "unhandled send =
//     compile error" holds universally (the `Handles<A,M>` protocol-membership concept).
//   * `ask` is ASYNC-ONLY: it returns an awaitable one-shot future (no `ask_sync`). Off-lane
//     bootstrap uses `block_on`, which asserts it is NOT on a worker lane (returns `on_worker`).
//   * the reply routes through a shard-pooled, monotonic-generation ReplyCell (detail/reply_cell),
//     never the caller frame — the win-arbitration handshake makes it resolve exactly once.
//
// REPLY MECHANISM (scoped seam): the ADR-007 dispatch table is locked this session and dispatches
// only void/`task<>` handlers, so the reply channel travels INSIDE the ask envelope
// (`Ask<Q,R>` carries a `Responder<R>`) rather than as a handler return value. An author writes
// `void handle(const Ask<Q,R>& m) { m.respond(compute(m.query)); }`. Promoting the sugar
// `R handle(const Q&)` is a dispatch-layer extension deferred to 005/ADR-007.
#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "quark/core/descriptor.hpp"
#include "quark/core/dispatch.hpp"
#include "quark/core/engine.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/message_context.hpp"  // tl_current_ctx — ambient Principal (020 §3)
#include "quark/core/metadata.hpp"  // 008: the durable content-addressed type_key_of<A> / actor_id_of<A>
#include "quark/core/principal.hpp"
#include "quark/core/reply_stream.hpp"
#include "quark/detail/envelope_pool.hpp"  // ADR-044: EnvelopePool, envelope_of, DescriptorEnvelope
#include "quark/detail/message_pool.hpp"
#include "quark/detail/reply_cell.hpp"

namespace quark {

using detail::Responder;

// --- Type identity (008) ------------------------------------------------------------------
// `type_key_of<A>()` / `actor_id_of<A>(key)` now come from metadata.hpp — the stable, content-
// addressed 008 TypeKey (016 fingerprint for described types; a canonical-name + protocol fold for
// actors), which is ALSO the durable/wire key. This retires the earlier process-local sentinel
// address 006 carried as a placeholder (the reported 008 seam is now closed).

// --- ask envelope -------------------------------------------------------------------------
// The wire message an `ask<R>(Q)` posts. Move-only (its Responder is move-only + single-armed), so
// exactly one Responder — the one landed in the pooled payload — governs the ReplyCell.
template <class Q, class R>
struct Ask {
    Q query;
    Responder<R> respond;
};

// --- ask future ---------------------------------------------------------------------------
// The awaitable one-shot returned by `ask`. co_await-able (the awaiter contract) AND block_on-able
// (off-lane). The AWAITER is the sole cell-release point (ADR-007 §5): take() then return the cell.
template <class R>
class [[nodiscard]] AskFuture {
public:
    AskFuture() noexcept = default;
    AskFuture(detail::ReplyCell<R>* cell, detail::ReplyCellPool<R>* pool) noexcept
        : cell_(cell), pool_(pool) {}

    AskFuture(const AskFuture&) = delete;
    AskFuture& operator=(const AskFuture&) = delete;
    AskFuture(AskFuture&& o) noexcept
        : cell_(std::exchange(o.cell_, nullptr)), pool_(std::exchange(o.pool_, nullptr)) {}
    AskFuture& operator=(AskFuture&& o) noexcept {
        if (this != &o) {
            release_unawaited();
            cell_ = std::exchange(o.cell_, nullptr);
            pool_ = std::exchange(o.pool_, nullptr);
        }
        return *this;
    }
    // Dropped without being awaited: return the cell (a still-in-flight responder is gen-fenced —
    // it no-ops into the recycled cell). The reply is simply discarded, as the caller asked.
    ~AskFuture() { release_unawaited(); }

    // ---- awaiter interface (co_await) ----
    [[nodiscard]] bool await_ready() const noexcept { return cell_->ready(); }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept { return cell_->suspend(h); }
    [[nodiscard]] result<R> await_resume() noexcept { return finish(); }

    // ---- off-lane blocking drive (block_on) ----
    [[nodiscard]] result<R> wait() noexcept {
        cell_->block_wait();
        return finish();
    }

private:
    result<R> finish() noexcept {
        result<R> r = cell_->take();
        pool_->release(cell_);
        cell_ = nullptr;
        return r;
    }
    void release_unawaited() noexcept {
        if (cell_ != nullptr) {
            pool_->release(cell_);
            cell_ = nullptr;
        }
    }

    detail::ReplyCell<R>* cell_ = nullptr;
    detail::ReplyCellPool<R>* pool_ = nullptr;
};

// --- LocalRouter: resolves identity + owns the send-side pools -----------------------------
// The `system` facade of 006 for local delivery: `get<A>(key)` hands back a typed ActorRef; the
// router carries the engine courier, the message pool, and per-R ReplyCell pools. Not templated on
// the engine Policy (courier is erased), so ActorRef<A> is Policy-free.
template <class A>
class ActorRef;

// ADR-044 (S1r fix): dispatches reclaim to the RIGHT pool by reading the SAME flags word
// GenState::flags_of() already exposes — never conflates EnvelopePool's free list with MessagePool's
// (the bug the design's cross-examination found: reinterpret-casting an EnvelopePool::Cell-backed
// Descriptor* through MessagePool::cell_of() reads DescriptorEnvelope::principal's bytes as
// MessagePool::Cell::destroy, an invalid function pointer). Every LocalRouter reclaim-adjacent call
// site routes through this, not a bare `pool_->reclaim(d)` — including the PERSISTENT sink handed to
// `courier_.activate()`.
struct DualReclaimSink {
    detail::MessagePool* plain = nullptr;
    detail::EnvelopePool* envelope = nullptr;
    static void thunk(Descriptor* d, void* self) noexcept {
        auto* s = static_cast<DualReclaimSink*>(self);
        const std::uint16_t flags = GenState::flags_of(d->gen_state.load(std::memory_order_relaxed));
        if (flags & kControlFlagHasEnvelope)
            s->envelope->reclaim(d);
        else
            s->plain->reclaim(d);
    }
    void reclaim(Descriptor* d) const noexcept { thunk(d, const_cast<DualReclaimSink*>(this)); }
    [[nodiscard]] ReclaimSink sink() noexcept { return ReclaimSink{&thunk, this}; }
};

class LocalRouter {
public:
    // ADR-044: `envelope_pool_` is owned internally (zero initial capacity, grows lazily on first
    // non-anonymous-principal send) rather than injected — this keeps the public constructor
    // signature unchanged for every existing call site. A deployment that never sets a Principal
    // never grows it past zero cells, matching 020's "local pays nothing" invariant.
    LocalRouter(PostCourier courier, detail::MessagePool& pool) noexcept
        : courier_(courier), pool_(&pool), envelope_pool_(0) {
        dual_.plain = pool_;
        dual_.envelope = &envelope_pool_;
    }

    LocalRouter(const LocalRouter&) = delete;
    LocalRouter& operator=(const LocalRouter&) = delete;

    // Resolve identity + placement only — never blocks, never creates state (006 §Actor references).
    template <class A>
    [[nodiscard]] ActorRef<A> get(std::uint64_t key) noexcept {
        return ActorRef<A>{actor_id_of<A>(key), this};
    }

    // --- tell (006 §tell): fire-and-forget. Constrained to protocol membership (compile error on
    // an unhandled type). 0 hot-path allocation (pooled descriptor + inline payload). -----------
    template <class A, class M>
    void tell(ActorId id, M&& m) {
        using Msg = std::remove_cvref_t<M>;
        static_assert(Handles<A, Msg>, "tell: message type is not in the actor's protocol (unhandled)");
        Schedulable* s = courier_.resolve(id);
        post_message<A, Msg>(id, s, std::forward<M>(m));
    }

    // --- deliver_from_wire (010 §Distribution): post a message that arrived off the network. -----
    // Identical to the 006 local `tell` delivery path EXCEPT the trace correlation id (009) and the
    // absolute deadline (018) come from the wire frame, NOT the local ambient — a cross-node message
    // carries its own propagated context. The distributed router's inbound dispatch (distribution.hpp)
    // decodes the 016 bytes into `msg` and calls this; from here the message is an ordinary local
    // delivery (same stamp, same pool, same courier), so per-actor FIFO and single-executor hold. If
    // the target is not registered on THIS node (a stale placement / mid-migration) the descriptor is
    // reclaimed (dead-lettered locally, 006/007) — never leaked, never posted to a null Schedulable.
    // ADR-044: `principal` is the frame's wire-propagated Principal (020 §3) — anonymous() (the
    // overwhelming default absent a security config) takes the UNMODIFIED plain-pool path byte for
    // byte; only a non-anonymous principal pays EnvelopePool.
    template <class A, class Msg, class T>
    void deliver_from_wire(ActorId id, T&& msg, std::int64_t deadline_ns,
                           std::uint64_t trace_id, const Principal& principal = Principal{}) {
        static_assert(Handles<A, std::remove_cvref_t<Msg>>,
                      "deliver_from_wire: message type is not in the actor's protocol (unhandled)");
        Schedulable* s = courier_.resolve(id);
        Descriptor* d = principal.anonymous()
            ? make_descriptor<A, std::remove_cvref_t<Msg>>(std::forward<T>(msg), trace_id, deadline_ns)
            : make_descriptor_enveloped<A, std::remove_cvref_t<Msg>>(std::forward<T>(msg), trace_id,
                                                                     deadline_ns, principal);
        if (s == nullptr) {
            dual_.reclaim(d);  // not_found on this node: dead-letter locally (006) — ADR-044 dual-dispatch
            return;
        }
        (void)courier_.post(s, d);
    }

    // --- ask (006 §ask): request/reply through a pooled ReplyCell. Returns an AskFuture<R>. -----
    template <class A, class R, class Q>
    [[nodiscard]] AskFuture<R> ask(ActorId id, Q&& q) {
        using Query = std::remove_cvref_t<Q>;
        using Envelope = Ask<Query, R>;
        static_assert(Handles<A, Envelope>,
                      "ask: actor must handle Ask<Q,R> (the reply-carrying envelope)");
        detail::ReplyCellPool<R>& cp = cell_pool<R>();
        typename detail::ReplyCellPool<R>::Lease lease = cp.acquire();
        Schedulable* s = courier_.resolve(id);
        Envelope env{std::forward<Q>(q), Responder<R>{lease.cell, lease.gen}};
        post_message<A, Envelope>(id, s, std::move(env));
        return AskFuture<R>{lease.cell, &cp};
    }

    // --- ask_stream (006 §ask_stream / ADR-018): request/N-item-reply through the flipped 024 ring.
    // Returns an OpenStreamFuture<F> — co_await/block_on-able exactly like AskFuture<R>, resolving to
    // a ReplyStream<F> drain handle once the callee `.accept()`s (or an error on reject/no-reply).
    // `make_ask_stream` builds {envelope, future} the same way `ask`'s body builds them inline above;
    // the ring is allocated cold, once per call, from `mr` (ordinary heap by default — a per-shard
    // slab allocator is a separate, unproven 022/ADR-018 residual, not built here).
    template <class A, class F, class Q>
    [[nodiscard]] OpenStreamFuture<F> ask_stream(
        ActorId id, Q&& q, typename ReplyStreamState<F>::Config cfg = {},
        std::pmr::memory_resource* mr = std::pmr::new_delete_resource()) {
        using Query = std::remove_cvref_t<Q>;
        using Envelope = AskStream<Query, F>;
        static_assert(Handles<A, Envelope>,
                      "ask_stream: actor must handle AskStream<Q,F> (the reply-stream-carrying envelope)");
        AskStreamRequest<Query, F> req = make_ask_stream<Query, F>(std::forward<Q>(q), mr, cfg);
        Schedulable* s = courier_.resolve(id);
        post_message<A, Envelope>(id, s, std::move(req.envelope));
        return std::move(req.future);
    }

    // --- passivate (ADR-028 Phase 8; 006 §passivate): on-demand, SOFT actor teardown. ------------
    // Fire-and-forget, matching tell's async nature. Delegates straight to the courier — returns
    // false iff `id` never resolves to a live activation (nothing to passivate); true otherwise
    // (accepted/already-pending, not a promise the actor has retired yet).
    [[nodiscard]] bool request_passivate(ActorId id) noexcept { return courier_.passivate(id); }

    // ADR-044: the ReclaimSink every spawned actor's Activation must be constructed with in place of
    // `pool.sink()` IF that activation may ever receive a message with a non-anonymous principal
    // (a wire arrival, or a local forward from a handler running under one) — its mailbox can then
    // receive descriptors from EITHER pool, so its reclaim path must dual-dispatch exactly like
    // LocalRouter's own internal fallback paths do. An activation that never receives one is safe
    // either way, since it never sees an EnvelopePool-sourced descriptor to misroute.
    [[nodiscard]] ReclaimSink reclaim_sink() noexcept { return dual_.sink(); }

private:
    // Build the pooled descriptor + inline payload for `msg`, stamping the ADR-007 dense dispatch
    // slot and the {trace_id, deadline_ns} the caller resolved. Shared by the local `tell`/`ask`
    // hot path (ambient context) and the 010 wire-inbound path (frame context) — one builder, so the
    // two never drift. 0 heap allocation (pooled cell + placement-new inline payload).
    template <class A, class Msg, class T>
    [[nodiscard]] Descriptor* make_descriptor(T&& msg, std::uint64_t trace_id,
                                              std::int64_t deadline_ns) {
        static_assert(sizeof(Msg) <= detail::MessagePool::kMaxPayload,
                      "message payload exceeds the pool cell size (raise MessagePool::kMaxPayload)");
        static_assert(alignof(Msg) <= detail::MessagePool::kPayloadAlign,
                      "message alignment exceeds the pool cell alignment");
        detail::MessagePool::Slot slot = pool_->acquire(&detail::destroy_payload<Msg>);
        Descriptor* d = slot.desc;
        ::new (slot.payload) Msg(std::forward<T>(msg));
        d->payload = slot.payload;
        d->payload_size = static_cast<std::uint32_t>(sizeof(Msg));
        d->trace_id = trace_id;
        d->deadline_ns = deadline_ns;
        stamp<A, Msg>(*d);  // ADR-007 dense slot into Descriptor::reserved
        return d;
    }

    // ADR-044: the enveloped twin of make_descriptor — built through EnvelopePool instead of
    // MessagePool, stamping Principal into the co-located DescriptorEnvelope tail BEFORE the
    // descriptor is ever published to a mailbox (single-writer, pre-publish — mirrors
    // make_descriptor's own trace_id/deadline_ns stamping). Only ever called when `p` is non-anonymous.
    template <class A, class Msg, class T>
    [[nodiscard]] Descriptor* make_descriptor_enveloped(T&& msg, std::uint64_t trace_id,
                                                        std::int64_t deadline_ns, const Principal& p) {
        static_assert(sizeof(Msg) <= detail::EnvelopePool::kMaxPayload,
                      "message payload exceeds the envelope pool cell size");
        static_assert(alignof(Msg) <= detail::EnvelopePool::kPayloadAlign,
                      "message alignment exceeds the envelope pool cell alignment");
        detail::EnvelopePool::Slot slot = envelope_pool_.acquire(&detail::destroy_payload<Msg>, p);
        Descriptor* d = slot.desc;
        ::new (slot.payload) Msg(std::forward<T>(msg));
        d->payload = slot.payload;
        d->payload_size = static_cast<std::uint32_t>(sizeof(Msg));
        d->trace_id = trace_id;
        d->deadline_ns = deadline_ns;
        stamp<A, Msg>(*d);
        return d;
    }

    // Build the descriptor + inline payload and post it to the target activation. If the id does
    // not resolve, try the ADR-028 Phase 4 lazy-activation hand-off first (`courier_.activate`) — if
    // the target's type was never `declare_lazy`'d (or this courier predates Phase 4, e.g. a
    // hand-rolled test/TestKit/SimEngine courier), `activate` returns false immediately and this
    // falls through to the ORIGINAL synchronous dead-letter (unregistered / remote — a 010 seam): for
    // an ask that runs the Responder destructor, failing the cell so the caller gets an error, never
    // a hang.
    template <class A, class Msg, class T>
    void post_message(ActorId id, Schedulable* s, T&& msg) {
        // Ambient propagation (#12): a tell/ask issued FROM a running handler inherits that message's
        // trace correlation id (009), its absolute deadline (018 inheritance: a child call cannot
        // outlive its parent), AND — ADR-044 — its Principal (020 §3: default full inherit; a handler
        // that wants a downgrade calls `attenuate()` explicitly before sending, out of scope here).
        // Outside a handler the ambient is null ⇒ a fresh trace / no deadline / anonymous principal.
        std::uint64_t trace_id = 0;
        std::int64_t deadline_ns = 0;
        Principal principal{};
        if (const MessageContext* amb = detail::tl_current_ctx) {
            trace_id = amb->trace_id;
            deadline_ns = amb->deadline_ns;
            principal = amb->principal;
        }
        Descriptor* d = principal.anonymous()
            ? make_descriptor<A, Msg>(std::forward<T>(msg), trace_id, deadline_ns)
            : make_descriptor_enveloped<A, Msg>(std::forward<T>(msg), trace_id, deadline_ns, principal);
        if (s == nullptr) {
            // ADR-044: `courier_.activate()` wires this sink PERSISTENTLY onto the newly-activated
            // Activation (every FUTURE reclaim for it, not just this one message) — dual_.sink() must
            // be handed here, not a bare pool_->sink(), or every later enveloped reclaim on this
            // activation would misroute through MessagePool::cell_of() (the S1r bug).
            if (!courier_.activate(id, d, dual_.sink())) dual_.reclaim(d);
            return;
        }
        (void)courier_.post(s, d);  // the wake-edge bool is the scheduler's; the sender ignores it
    }

    // One ReplyCell pool per R, created lazily and stored keyed by a per-type sentinel address
    // (RTTI-free). Setup path (first ask<R>) takes the map lock; steady asks reuse the pool.
    template <class R>
    detail::ReplyCellPool<R>& cell_pool() {
        static constexpr char tag = 0;
        const void* key = &tag;
        std::lock_guard<std::mutex> g(cp_mu_);
        auto it = cell_pools_.find(key);
        if (it == cell_pools_.end()) {
            auto p = std::make_shared<detail::ReplyCellPool<R>>(kCellPoolCapacity);
            it = cell_pools_.emplace(key, std::move(p)).first;
        }
        return *static_cast<detail::ReplyCellPool<R>*>(it->second.get());
    }

    static constexpr std::size_t kCellPoolCapacity = 1024;

    PostCourier courier_;
    detail::MessagePool* pool_;
    detail::EnvelopePool envelope_pool_;  // ADR-044: wire-scale, non-anonymous-principal pool
    DualReclaimSink dual_;                // ADR-044 (S1r): flag-dispatched reclaim — never a bare
                                           // pool_->reclaim/pool_->sink() call site remains
    std::mutex cp_mu_;
    std::unordered_map<const void*, std::shared_ptr<void>> cell_pools_;
};

// --- ActorRef<A> --------------------------------------------------------------------------
// An always-typed, cheap-to-copy handle: identity (ActorId) + a delivery context (the router).
// It is a location + identity, not a pointer to actor state (006 §Actor references).
template <class A>
class ActorRef {
public:
    using actor_type = A;

    ActorRef() noexcept = default;
    ActorRef(ActorId id, LocalRouter* router) noexcept : id_(id), router_(router) {}

    [[nodiscard]] ActorId id() const noexcept { return id_; }
    [[nodiscard]] bool valid() const noexcept { return router_ != nullptr; }

    // tell — fire-and-forget (006 §tell). Unhandled type ⇒ compile error (Handles<A,M>).
    template <class M>
    void tell(M&& m) const {
        router_->template tell<A>(id_, std::forward<M>(m));
    }

    // ask<R>(q) — request/reply (006 §ask). async-only; returns an awaitable AskFuture<R>.
    template <class R, class Q>
    [[nodiscard]] AskFuture<R> ask(Q&& q) const {
        return router_->template ask<A, R>(id_, std::forward<Q>(q));
    }

    // ask_stream<F>(q) — request/N-item-reply (006 §ask_stream, ADR-018). async-only; returns an
    // awaitable OpenStreamFuture<F> that resolves to a ReplyStream<F> drain handle.
    template <class F, class Q>
    [[nodiscard]] OpenStreamFuture<F> ask_stream(
        Q&& q, typename ReplyStreamState<F>::Config cfg = {},
        std::pmr::memory_resource* mr = std::pmr::new_delete_resource()) const {
        return router_->template ask_stream<A, F>(id_, std::forward<Q>(q), cfg, mr);
    }

    // passivate() — on-demand, SOFT actor teardown (ADR-028 Phase 8; 006 §passivate). Fire-and-
    // forget: posts the SAME Deactivate control descriptor the automatic idle-timeout wheel posts,
    // through the SAME shared interlock, so the actor drains every message already queued/in-flight
    // and retires to Dormant at the next genuinely-idle instant — never a hard mid-handler interrupt.
    // Sequential-only (mirrors IdleTimeout<Ms>'s existing restriction, policies.hpp): idle-timeout
    // eviction's Dekker close-out — which this reuses verbatim — is proven only for exactly one
    // in-flight drain; calling .passivate() on a Reentrant/MaxConcurrency<N> actor is a COMPILE
    // ERROR, never a silent no-op or an unproven race.
    // Returns false iff `id_` does not currently resolve to a live activation (never spawned, or a
    // lazily-declared type never yet touched) — nothing to passivate. Returns true otherwise,
    // meaning "accepted / already pending" — NOT "has retired yet".
    bool passivate() const {
        static_assert(max_concurrency_of<A>() == 1,
                      "ActorRef<A>::passivate(): A must be Sequential -- reuses idle-timeout "
                      "eviction's Dekker close-out, proven only for exactly one in-flight drain "
                      "(ADR-028 Phase 2); Reentrant/MaxConcurrency<N> passivation is out of scope");
        return router_->request_passivate(id_);
    }

    // Identity/equality (006): the same (type, key) denotes the same logical actor. Two refs are
    // equal iff their ActorId is equal — a ref is a value over identity, not over delivery context.
    friend bool operator==(const ActorRef& a, const ActorRef& b) noexcept { return a.id_ == b.id_; }

private:
    ActorId id_{};
    LocalRouter* router_ = nullptr;
};

// --- block_on (ADR-007 §ask): off-lane bootstrap/edge drive ------------------------------
// Drives an AskFuture to completion by blocking the CURRENT thread. Asserts it is NOT on a worker
// lane — running it on a lane would deadlock the lane (the reply drains on that same lane), so it
// fail-fasts with `on_worker` instead. Off-lane it parks on the ReplyCell until resolved.
template <class R>
[[nodiscard]] result<R> block_on(AskFuture<R> f) noexcept {
    if (current_worker_id() != 0xFFFF'FFFFu)
        return std::unexpected<error>(error{errc::internal, "on_worker"});
    return f.wait();
}

}  // namespace quark

// Hash so ActorRef drops into unordered containers off the hot path (identity = ActorId).
template <class A>
struct std::hash<quark::ActorRef<A>> {
    std::size_t operator()(const quark::ActorRef<A>& r) const noexcept { return r.id().hash(); }
};
