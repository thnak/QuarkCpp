// Implements 014-Testing-Model §Unit harness (`TestKit`) — a single-actor harness that formalizes
// the ad-hoc `drive(act)` pattern the existing tests use into a reusable API, WITHOUT standing up an
// engine. It drives ONE real production `Activation` (001/007/015) on the calling thread and gives a
// test the four things 014 asks for:
//
//   * INBOUND drive — `tell(M)` / `ask<R>(Q)` deliver a message to the actor under test and drain its
//     lane to a quiescent point deterministically (no threads, no wall-clock).
//   * OUTBOUND capture — the actor's `tell`/`ask` through `ref<Target>(key)` are RECORDED, not
//     delivered, so a test asserts on what the actor sent (`outbound_count`, `took<Target,M>`).
//   * VIRTUAL TIME — `advance(d)` ticks a timing wheel (011) so timers scheduled to the actor fire
//     deterministically.
//   * CONTEXT injection — `with_deadline` / `request_stop` set the `MessageContext` (deadline / trace
//     / cooperative cancellation) the next handler observes, so context-dependent handlers are
//     testable (014 §Unit harness).
//
// There is ONE actor/handler/dispatch implementation (008): TestKit drives the SAME `Activation`
// drain and supervision the engine does — it differs only in that outbound sends are captured. State
// is exposed by reference for assertions (`actor()` / `assert_state`).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor_ref.hpp"
#include "quark/core/dispatch.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metadata.hpp"
#include "quark/core/policies.hpp"
#include "quark/core/supervision.hpp"
#include "quark/core/timer_wheel.hpp"
#include "quark/detail/message_pool.hpp"
#include "quark/detail/reply_cell.hpp"

namespace quark {

// A captured outbound send (014 §"outbound messages captured"). Stores identity + the dense msg slot
// + a byte copy of the payload, so a typed accessor can recover it (the slot is trusted exactly as
// dispatch trusts it — ADR-007). Bounded inline buffer sized to the message pool cell.
struct CapturedSend {
    ActorId target{};
    std::uint16_t slot = 0;
    std::uint32_t size = 0;
    alignas(detail::MessagePool::kPayloadAlign) unsigned char bytes[detail::MessagePool::kMaxPayload]{};
};

template <class A>
class TestKit {
    static_assert(is_actor<A>, "TestKit<A>: A must derive from quark::Actor<A, ...>");

public:
    struct Config {
        std::uint64_t key = 0;               // instance key for the actor under test
        std::chrono::nanoseconds tick = std::chrono::milliseconds(1);
        std::size_t pool_capacity = 256;
        std::size_t timer_capacity = 64;
    };

    TestKit() : TestKit(Config{}) {}
    explicit TestKit(Config cfg)
        : cfg_(cfg),
          tick_ns_(cfg.tick.count() <= 0 ? 1 : static_cast<std::uint64_t>(cfg.tick.count())),
          pool_(cfg.pool_capacity),
          timer_pool_(cfg.timer_capacity),
          act_(&actor_, A::dispatch_table(), pool_.sink(), max_concurrency_of<A>(),
               supervision_of<A>()),
          capture_router_(capture_courier(), pool_) {
        act_.set_reconstruct(make_reconstruct_sink<A>());
        act_.set_dead_letter_sink(DeadLetterSink{&TestKit::dead_letter_thunk, this});
    }

    TestKit(const TestKit&) = delete;
    TestKit& operator=(const TestKit&) = delete;

    // --- The actor under test (state access for wiring + assertions, 014 §expose actor state) ---
    [[nodiscard]] A& actor() noexcept { return actor_; }
    [[nodiscard]] const A& actor() const noexcept { return actor_; }
    [[nodiscard]] Activation& activation() noexcept { return act_; }
    template <class F>
    [[nodiscard]] bool assert_state(F&& pred) {
        return static_cast<bool>(std::forward<F>(pred)(static_cast<const A&>(actor_)));
    }

    // A CAPTURING ref to some target actor. The actor-under-test stores this like any ActorRef<T>;
    // its `tell`/`ask` are recorded (see outbound()) rather than delivered anywhere.
    template <class Target>
    [[nodiscard]] ActorRef<Target> ref(std::uint64_t key) noexcept {
        return capture_router_.template get<Target>(key);
    }

    // --- Context injection (014 §injects a MessageContext) -------------------------------------
    // Set the deadline / trace the NEXT delivered message carries; fire the activation's cooperative
    // stop_token so an in-flight handler observes cancellation (001).
    TestKit& with_deadline(std::int64_t deadline_ns) noexcept {
        next_deadline_ns_ = deadline_ns;
        return *this;
    }
    TestKit& with_trace(std::uint64_t trace_id) noexcept {
        next_trace_id_ = trace_id;
        return *this;
    }
    void request_stop() noexcept { act_.request_stop(); }

    // --- Inbound tell (014): deliver M to the actor and drain the lane to a quiescent point --------
    template <class M>
    void tell(M msg) {
        static_assert(Handles<A, M>, "TestKit::tell: M is not in the actor's protocol");
        Descriptor* d = build<M>(std::move(msg));
        act_.post(d);
        drive();
    }

    // --- Inbound ask (014): deliver Ask<Q,R>, drain, return the reply (or the failure value) -------
    // Builds the reply-carrying envelope exactly as LocalRouter::ask does, posts it, drives the lane,
    // and reads the resolved cell. The handler runs synchronously on this thread, so the cell is
    // resolved by the time drive() returns (or failed by reply-before-teardown if the handler never
    // replied) — never a hang.
    template <class R, class Q>
    [[nodiscard]] result<R> ask(Q query) {
        using Envelope = Ask<std::remove_cvref_t<Q>, R>;
        static_assert(Handles<A, Envelope>, "TestKit::ask: actor must handle Ask<Q,R>");
        auto& pool = cell_pool<R>();
        typename detail::ReplyCellPool<R>::Lease lease = pool.acquire();
        Envelope env{std::move(query), detail::Responder<R>{lease.cell, lease.gen}};
        Descriptor* d = build<Envelope>(std::move(env));
        act_.post(d);
        drive();
        result<R> r = lease.cell->take();
        pool.release(lease.cell);
        return r;
    }

    // --- Virtual time (014 §drives virtual time) -----------------------------------------------
    template <class M>
    void schedule_after(std::chrono::nanoseconds delay, M msg) {
        static_assert(Handles<A, M>, "TestKit::schedule_after: M is not in the actor's protocol");
        using Bound = SelfSend<std::remove_cvref_t<M>>;
        static_assert(sizeof(Bound) <= detail::TimerEntry::kInlineCap, "timer payload too large");
        detail::TimerEntry* e = timer_pool_.acquire();
        e->expiry_ticks = wheel_.now_ticks() + ticks_ceil(delay);
        e->period_ticks = 0;
        ::new (e->storage) Bound{this, std::move(msg)};
        e->fire_fn = &fire_self<std::remove_cvref_t<M>>;
        e->destroy_fn = &destroy_self<std::remove_cvref_t<M>>;
        wheel_.insert(e);
    }
    void advance(std::chrono::nanoseconds by) {
        const std::uint64_t ns = static_cast<std::uint64_t>(by.count() <= 0 ? 0 : by.count());
        const std::uint64_t ticks = ns / tick_ns_;
        for (std::uint64_t i = 0; i < ticks; ++i)
            wheel_.tick([this](detail::TimerEntry* e) noexcept { on_expire(e); });
        now_ns_ += ticks * tick_ns_;
        drive();
    }
    [[nodiscard]] std::int64_t now_ns() const noexcept { return static_cast<std::int64_t>(now_ns_); }

    // --- Outbound assertions (014 §outbound messages captured) ---------------------------------
    [[nodiscard]] std::size_t outbound_count() const noexcept { return outbound_.size(); }
    [[nodiscard]] const std::vector<CapturedSend>& outbound() const noexcept { return outbound_; }
    void clear_outbound() noexcept { outbound_.clear(); }

    // Count the captured sends of message type M addressed to `Target` (by dense slot + type key).
    template <class Target, class M>
    [[nodiscard]] std::size_t count_tells_to() const noexcept {
        static_assert(Handles<Target, M>, "count_tells_to<Target,M>: M not in Target's protocol");
        std::size_t n = 0;
        const std::uint16_t want = slot_of<Target, M>();
        const TypeKey tk = type_key_of<Target>();
        for (const CapturedSend& c : outbound_)
            if (c.slot == want && c.target.type == tk) ++n;
        return n;
    }

    // Recover the captured M messages sent to `Target` (trivially-copyable messages only — the test
    // messages are). Lets a test assert exact outbound content: `kit.tells_to<Inv,Reserve>()[0]`.
    template <class Target, class M>
    [[nodiscard]] std::vector<M> tells_to() const {
        static_assert(Handles<Target, M>, "tells_to<Target,M>: M not in Target's protocol");
        static_assert(std::is_trivially_copyable_v<M>, "tells_to<M>: M must be trivially copyable");
        std::vector<M> out;
        const std::uint16_t want = slot_of<Target, M>();
        const TypeKey tk = type_key_of<Target>();
        for (const CapturedSend& c : outbound_) {
            if (c.slot == want && c.target.type == tk && c.size == sizeof(M)) {
                M m;
                std::memcpy(&m, c.bytes, sizeof(M));
                out.push_back(m);
            }
        }
        return out;
    }

    [[nodiscard]] std::uint64_t dead_letters() const noexcept { return dead_letters_; }

private:
    // Build a pooled descriptor + inline payload for message M (mirrors LocalRouter::post_message).
    template <class M>
    Descriptor* build(M&& msg) {
        using Msg = std::remove_cvref_t<M>;
        static_assert(sizeof(Msg) <= detail::MessagePool::kMaxPayload, "message too large for the pool");
        detail::MessagePool::Slot slot = pool_.acquire(&detail::destroy_payload<Msg>);
        Descriptor* d = slot.desc;
        ::new (slot.payload) Msg(std::forward<M>(msg));
        d->payload = slot.payload;
        d->payload_size = static_cast<std::uint32_t>(sizeof(Msg));
        d->deadline_ns = next_deadline_ns_;
        d->trace_id = next_trace_id_;
        stamp<A, Msg>(*d);
        next_deadline_ns_ = 0;  // one-shot context injection
        next_trace_id_ = 0;
        return d;
    }

    // The reusable drive loop — the formalized `drive(act)`. Acquires the lane and drains to a
    // quiescent stop (Idle, or Suspended when an async handler parks with no carrier).
    void drive() {
        if (!act_.try_acquire()) return;  // nothing schedulable
        constexpr std::uint32_t kBudget = 1u << 20;
        std::uint64_t busy = 0;
        for (;;) {
            switch (act_.drain_step(kBudget)) {
                case Activation::DrainOutcome::DrainedEmpty:
                    if (act_.close_out()) { busy = 0; continue; }
                    return;
                case Activation::DrainOutcome::BudgetExhausted:
                    act_.yield_to_scheduled();
                    if (act_.try_acquire()) { busy = 0; continue; }
                    return;
                case Activation::DrainOutcome::Busy:
                    if (++busy > (1u << 24)) return;  // single-thread: never spins for real
                    continue;
                case Activation::DrainOutcome::Suspended:
                    return;  // async parked; no carrier in the unit harness (a documented seam)
            }
        }
    }

    // --- Outbound capture courier (records sends instead of delivering) -------------------------
    [[nodiscard]] PostCourier capture_courier() noexcept {
        return PostCourier{this, &TestKit::capture_resolve, &TestKit::capture_post};
    }
    // A non-null sentinel so LocalRouter posts (a null resolve makes it reclaim/fail the ask cell).
    static Schedulable* capture_resolve(void* self, ActorId id) noexcept {
        auto* k = static_cast<TestKit*>(self);
        k->capture_id_ = id;  // stash the resolved target for the paired post
        return &k->capture_sched_;
    }
    static bool capture_post(void* self, Schedulable*, Descriptor* d) noexcept {
        auto* k = static_cast<TestKit*>(self);
        CapturedSend c;
        c.target = k->capture_id_;
        c.slot = slot_from(*d);
        c.size = d->payload_size;
        const std::uint32_t n = d->payload_size <= detail::MessagePool::kMaxPayload
                                    ? d->payload_size
                                    : detail::MessagePool::kMaxPayload;
        if (d->payload != nullptr) std::memcpy(c.bytes, d->payload, n);
        k->outbound_.push_back(c);
        k->pool_.reclaim(d);  // captured; return the cell (runs the payload dtor — e.g. Responder)
        return false;         // no real wake — nothing is scheduled
    }

    // --- Virtual-time self-send timer ----------------------------------------------------------
    template <class M>
    struct SelfSend {
        TestKit* kit;
        M msg;
    };
    template <class M>
    static void fire_self(void* p) noexcept {
        auto* s = static_cast<SelfSend<M>*>(p);
        Descriptor* d = s->kit->template build<M>(M{s->msg});  // copy: leave msg for a re-arm
        s->kit->act_.post(d);
    }
    template <class M>
    static void destroy_self(void* p) noexcept {
        static_cast<SelfSend<M>*>(p)->~SelfSend<M>();
    }
    void on_expire(detail::TimerEntry* e) noexcept {
        if (e->cancelled) { recycle(e); return; }
        e->fire_fn(e->storage);
        recycle(e);
    }
    void recycle(detail::TimerEntry* e) noexcept {
        if (e->destroy_fn != nullptr) e->destroy_fn(e->storage);
        timer_pool_.release(e);
    }
    [[nodiscard]] std::uint64_t ticks_ceil(std::chrono::nanoseconds d) const noexcept {
        const std::int64_t ns = d.count() <= 0 ? 0 : d.count();
        const std::uint64_t t = (static_cast<std::uint64_t>(ns) + tick_ns_ - 1) / tick_ns_;
        return t == 0 ? 1 : t;
    }

    template <class R>
    detail::ReplyCellPool<R>& cell_pool() {
        static detail::ReplyCellPool<R> pool(64);  // per-R, per-TU test pool (single-threaded)
        return pool;
    }

    static void dead_letter_thunk(Descriptor*, error, void* ctx) noexcept {
        ++static_cast<TestKit*>(ctx)->dead_letters_;
    }

    Config cfg_;
    std::uint64_t tick_ns_;
    A actor_;  // default-init (implicit ctor); brace-init would need the protected Actor<> base ctor
    detail::MessagePool pool_;
    detail::TimerEntryPool timer_pool_;
    detail::TimingWheel wheel_{};
    Activation act_;
    LocalRouter capture_router_;
    Schedulable capture_sched_{};   // sentinel for the capture courier (never scheduled)
    ActorId capture_id_{};          // target of the send being captured (resolve→post hand-off)

    std::vector<CapturedSend> outbound_;
    std::int64_t next_deadline_ns_ = 0;
    std::uint64_t next_trace_id_ = 0;
    std::uint64_t now_ns_ = 0;
    std::uint64_t dead_letters_ = 0;
};

}  // namespace quark
