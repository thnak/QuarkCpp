// Implements 002-Scheduler — the N-worker lane pool that drives 001 activations across threads.
//
// The Engine owns: a fixed set of SHARDS (each a per-shard K-band run-queue of Scheduled
// activations, ADR-010), a fixed set of WORKER lanes (`std::jthread`, ≤ hardware but ALWAYS an
// explicit count — NEVER hardware_concurrency), targeted per-worker wakeup, and the clean-drain
// start()/stop(). Workers are transient lanes, never owners: any worker may drain any shard, and
// stealing = winning a shard's cold drain-owner CAS (ADR-010 §Work stealing).
//
// The three load-bearing rendezvous (all seq_cst StoreLoad Dekker pairs, x86-elided on the
// producer half — ADR-004/010/015):
//   1. MAILBOX close-out (per actor) — driven inside Activation (post/close_out); orthogonal here.
//   2. RUN-QUEUE wake (producer enqueues an activation ↔ a worker parks) — the {run-queue tail,
//      idle_mask} Dekker below: a producer never fails to wake a worker that is going to sleep.
//   3. DRAIN-OWNER close-out (a worker releases a shard ↔ a producer enqueues to it) — the
//      {drain_owner, run-queue tail} Dekker: an activation enqueued in the release window is never
//      stranded (the releasing worker re-acquires and drains it).
//
// WAKEUP RIDES THE EXEC-STATE MACHINE, NEVER MAILBOX/RUN-QUEUE EMPTINESS (002, normative): the
// wake edge is the `should_wake` boolean returned by Activation::post/notify_enqueued/
// complete_parked — the Idle→Scheduled / Parked→Scheduled transition — not "a queue became
// non-empty" (Vyukov emptiness is non-linearizable and would lose wakeups).
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>  // _mm_pause (detail::cpu_relax) — MSVC has no __builtin_ia32_pause
#endif

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"           // ADR-028 Phase 4: Actor<> CRTP base for the internal broker
#include "quark/core/config.hpp"
#include "quark/core/engine_config.hpp"  // 013/ADR-008: frozen EngineConfig + ConfigBuilder + Validation
#include "quark/core/hot_cell.hpp"        // 013/ADR-008: the Live operational-word (0-RMW hot read)
#include "quark/core/ids.hpp"
#include "quark/core/metadata.hpp"        // 008: compiled ActorMetadata, factories, typed spawn wiring
#include "quark/core/metrics.hpp"         // 009: ShardCounters/MetricsRegistry — the metrics_snapshot() surface
#include "quark/core/policies.hpp"        // 005: priority_band_of / drain_budget_of / max_concurrency_of
#include "quark/core/resource.hpp"        // 004: ResourceScope for the cold activation wire pass
#include "quark/core/run_queue.hpp"
#include "quark/core/supervision.hpp"     // 007: supervision_of<A>() resolved at spawn
#include "quark/core/timer_wheel.hpp"     // 011/ADR-028 Phase 2: the per-shard idle-eviction wheel
#include "quark/detail/message_pool.hpp"  // ADR-028 Phase 4: broker_pool_ (Wake control descriptors)
#include "pal/pal.hpp"

namespace quark {

// A type-erased, Policy-independent addressing/post seam (006). `ActorRef<A>` is typed on the
// actor but MUST NOT be templated on the engine's scheduling Policy, so the engine hands out this
// small function-pointer courier (no virtual — the resolve/post wrappers are `.rodata` targets
// bound per Policy). SCOPE (006): local (same-node) delivery only; the remote/transport courier is
// a seam to 010 (a distinct `post_fn` that serializes + hands to a Transport).
struct PostCourier {
    void* engine = nullptr;
    Schedulable* (*resolve_fn)(void* engine, ActorId id) noexcept = nullptr;
    bool (*post_fn)(void* engine, Schedulable* s, Descriptor* d) noexcept = nullptr;
    // ADR-028 Phase 4: the lazy-activation cold-path hand-off. `origin_reclaim` is the CALLER's own
    // pool's reclaim sink (e.g. `LocalRouter::pool_->sink()`) — needed ONLY if construction never
    // even starts (the type was never `declare_lazy`'d, or a defensive wire failure); once a real
    // Activation exists it owns the message via its OWN wired reclaim, same as any spawned actor.
    // Null (default) ⇒ unsupported — a hand-rolled `PostCourier` (tests, TestKit, SimEngine) keeps
    // today's exact synchronous dead-letter behavior unchanged.
    bool (*activate_fn)(void* engine, ActorId id, Descriptor* d, ReclaimSink origin_reclaim) noexcept =
        nullptr;

    [[nodiscard]] Schedulable* resolve(ActorId id) const noexcept {
        return resolve_fn(engine, id);
    }
    [[nodiscard]] bool post(Schedulable* s, Descriptor* d) const noexcept {
        return post_fn(engine, s, d);
    }
    [[nodiscard]] bool activate(ActorId id, Descriptor* d, ReclaimSink origin_reclaim) const noexcept {
        return activate_fn != nullptr && activate_fn(engine, id, d, origin_reclaim);
    }
};

// Compile-time per-actor drain budget (005 policy flavor). Resolved to a runtime value at
// registration; the engine carries an engine-wide default for actors that don't override it.
template <std::uint32_t N>
struct DrainBudget {
    static constexpr std::uint32_t value = N;
};

// `EngineConfig` (frozen-core, 013/ADR-008) + `ConfigBuilder` + `Validation` now live in
// engine_config.hpp; the Live operational word (`HotCell`) in hot_cell.hpp (both included above).

namespace detail {
QUARK_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();  // MSVC has no __builtin_ia32_pause; this is its PAUSE-instruction intrinsic
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Per-worker identity, set by the worker lane on entry. Observability/test seam only (lets a
// handler learn which lane is executing it — e.g. to witness a work-steal). Not on the hot path.
inline thread_local std::uint32_t t_worker_id = 0xFFFF'FFFFu;
}  // namespace detail

// The worker-lane index of the current thread, or 0xFFFFFFFF off any worker lane (002 test seam).
[[nodiscard]] inline std::uint32_t current_worker_id() noexcept { return detail::t_worker_id; }

template <class Policy = UniformFIFO>
class Engine {
public:
    explicit Engine(EngineConfig cfg) : cfg_(cfg), type_registry_(cfg_.validation, cfg_.max_types) {
        if (cfg_.worker_count == 0) cfg_.worker_count = 1;
        if (cfg_.shard_count == 0) cfg_.shard_count = 1;
        if (cfg_.drain_budget == 0) cfg_.drain_budget = 1;
        if (cfg_.idle_tick_ms == 0) cfg_.idle_tick_ms = 1;  // 011/ADR-028: the non-validating ctor clamps
        shard_pow2_ = (cfg_.shard_count & (cfg_.shard_count - 1)) == 0;
        shard_mask_ = cfg_.shard_count - 1;
        shards_ = std::make_unique<Shard[]>(cfg_.shard_count);
        workers_ = std::make_unique<Worker[]>(cfg_.worker_count);
        // 009: register every shard's embedded ShardCounters (cold, one-time; register_shard just
        // stores a pointer) so metrics_snapshot()/metrics_prometheus() aggregate across all shards.
        for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh) metrics_.register_shard(shards_[sh].metrics);
        // ADR-028 Phase 4: one ActivationBroker per shard, constructed unconditionally (cheap:
        // Sequential, KeepAlive, silent unless `declare_lazy` is ever used). Stored directly on
        // `Shard` — NEVER in `by_id_`/addressable via `ActorRef` — a pure engine-internal seam.
        for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh) {
            auto broker = std::make_unique<ActivationBroker>(this, sh);
            auto bact = std::make_unique<Activation>(broker.get(), ActivationBroker::dispatch_table(),
                                                     broker_pool_.sink(),
                                                     max_concurrency_of<ActivationBroker>(),
                                                     supervision_of<ActivationBroker>());
            std::unique_ptr<Schedulable> bs = build_schedulable(
                sh, *bact, /*band*/ 0, cfg_.drain_budget, /*idle_ticks*/ 0);  // KeepAlive: never evict
            shards_[sh].broker_schedulable = bs.get();
            shards_[sh].broker_schedulable_owner = std::move(bs);  // per-shard, not registry_
            shards_[sh].broker_activation = std::move(bact);
            shards_[sh].broker_instance = std::move(broker);
        }
        // Seed the Live operational word (013/ADR-008) from the frozen config. This ctor is the
        // non-validating path (ConfigBuilder::build() is the validating one, 008), so it CLAMPS each
        // Live field into its packing ceiling rather than reject — the frozen `cfg_.drain_budget`
        // remains the source of truth for registration (which is why it may legitimately exceed the
        // Live 2^14 ceiling; the clamped hot word is only the operational/reconfig surface).
        hot_.publish(pack_operational(clamp_operational(cfg_.operational_seed())));
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    ~Engine() { stop(); }

    // Convenience ctor (worker + shard count, defaults for the rest).
    Engine(std::uint32_t worker_count, std::uint32_t shard_count)
        : Engine(EngineConfig{worker_count, shard_count, 1024, 64}) {}

    // --- Registration (COLD) ---------------------------------------------------------------
    // Map the actor to its stable shard, resolve its band + budget, and hand back a Schedulable the
    // producer uses on the hot path. The engine owns the Schedulable's lifetime; the CALLER owns the
    // Activation (and its ReclaimSink → DescriptorPool wiring). `band` 0 = highest priority.
    // `idle_ticks` (ADR-028 Phase 2): the actor's declared `IdleTimeout<Ms>` already converted to a
    // wheel-tick count by the caller (0 = KeepAlive/no timeout, the default — existing callers that
    // don't pass it get no idle-eviction behavior, unchanged from before this parameter existed).
    // Build a Schedulable for `act` on shard `sid` (band/budget/idle_ticks resolved, shard-wired),
    // returning OWNERSHIP to the caller — NOT stored in `registry_` here. `registry_` is a single
    // Engine-wide `vector`, safe only because it was, pre-Phase-4, mutated exclusively cold (before
    // `start()`, single-threaded). ADR-028 Phase 4's broker runs AT RUNTIME, concurrently across
    // shards — a shared Engine-wide container mutated from multiple shards' concurrently-running
    // brokers is EXACTLY the class of bug the ADR's bug (a) warns about (found the hard way: an
    // earlier draft of this code had the lazy path push into `registry_` too, and TSan/ASan caught a
    // vector-reallocation use-after-free under concurrent multi-shard first-touch). So each caller
    // now owns the result: `register_activation` (cold, single-threaded) still uses `registry_`;
    // the broker's lazy path (`handle_wake`) and the broker's own bootstrap store it in the
    // PER-SHARD `Shard::lazy_owned`/`broker_schedulable_` (never a shared container) instead.
    std::unique_ptr<Schedulable> build_schedulable(std::uint32_t sid, Activation& act,
                                                   std::uint16_t band, std::uint32_t budget,
                                                   std::uint32_t idle_ticks) {
        auto s = std::make_unique<Schedulable>();
        s->activation = &act;
        s->shard = sid;
        act.set_metrics(&shards_[sid].metrics);  // 009: wire this activation to its shard's counters
        act.set_idle_ticks(idle_ticks);  // 011/ADR-028 Phase 2
        s->band = static_cast<std::uint16_t>(band < Policy::bands ? band : Policy::bands - 1);
        // Default drain budget = the FROZEN `EngineConfig::drain_budget` (013/ADR-008): a per-actor
        // `DrainBudget<N>` still wins (budget != 0), else the engine-wide frozen default. This is
        // resolved ONCE at registration into `Schedulable::budget` (unchanged 002 semantics). The
        // ceiling-bounded LIVE operational default lives in `hot_` (see `default_drain_budget()` /
        // `reconfigure()`); it seeds from this frozen value and is what the 023-gated operational read
        // and control-plane reconfig observe. Per-actor budgets don't re-read the hot word per message
        // (they're fixed at registration), so the frozen path here can exceed the Live 2^14 ceiling.
        s->budget = budget != 0 ? budget : cfg_.drain_budget;
        return s;
    }

    Schedulable* register_activation(ActorId id, Activation& act, std::uint16_t band = 0,
                                     std::uint32_t budget = 0, std::uint32_t idle_ticks = 0) {
        std::unique_ptr<Schedulable> s = build_schedulable(shard_of(id), act, band, budget, idle_ticks);
        Schedulable* raw = s.get();
        registry_.push_back(std::move(s));  // cold-only: safe, single-threaded before start()
        by_id_[id] = raw;  // 006 addressing registry: ActorId → Schedulable (resolve() lookup)
        return raw;
    }

    // --- Typed spawn (008 startup integration; COLD) ---------------------------------------
    // The 008 keystone: build the actor + its Activation from the COMPILED metadata and wire every
    // accumulated cross-module seam in one place, then register it on the 002 scheduler.
    //   * 007 supervision — `supervision_of<A>()` → the Activation ctor; `make_reconstruct_sink<A>()`
    //     → `set_reconstruct` so a `Restart` reconstructs TRUE fresh state (not assert-intact).
    //   * 004 resources   — if `A` exposes a `wire(const ResourceScope&)` member, run the one-time
    //     COLD wire pass here; a `wire()` returning `errc::validation` (undeclared resource) fails
    //     the spawn fail-fast (Strict startup Validation, 008 §Validation) — nothing is registered.
    //   * 013 config      — the drain budget default falls through `register_activation` to the
    //     FROZEN `EngineConfig::drain_budget` when the actor declares no `DrainBudget<N>` (budget 0).
    //   * 005/002 seams   — band = `priority_band_of<A>()`, keying = `actor_id_of<A>` (unchanged).
    // The engine OWNS the actor instance + Activation lifetime (freed after `stop()` at destruction).
    // Returns the `ActorId` to route to (`router.get<A>(key)` uses the same key). The Sequential
    // `Activation` allocates zero 015 machinery (max_concurrency == 1), preserving zero-cost.
    template <class A>
    [[nodiscard]] result<ActorId> spawn(std::uint64_t key, ReclaimSink reclaim = {},
                                        const ResourceScope* scope = nullptr) {
        static_assert(is_actor<A>, "spawn<A>: A must derive from quark::Actor<A, ...>");
        static_assert(validate_actor_policies<A>(), "spawn<A>: actor policy validation failed");
        static_assert(priority_band_of<A>() < Policy::bands,
                      "spawn<A>: Priority<P> exceeds the engine's PriorityBands<K> (P < K) — ADR-010");

        auto actor = std::make_shared<A>();
        auto act = std::make_unique<Activation>(actor.get(), A::dispatch_table(), reclaim,
                                                max_concurrency_of<A>(), supervision_of<A>());
        act->set_reconstruct(make_reconstruct_sink<A>());  // 007: fresh-state Restart factory

        if constexpr (has_resource_wire<A>) {  // 004: one-time COLD wire pass (fail-fast Validation)
            if (scope != nullptr) {
                if (result<void> w = actor->wire(*scope); !w)
                    return std::unexpected<error>(w.error());
            }
            // 007 Restart re-wire ("Phase 6" redirected): remember this SAME wire()/scope so a later
            // OnFailure<Restart> reconstruction re-wires the fresh instance instead of leaving its
            // Cached<>/PerMessage<> members null.
            act->set_resource_wire(make_wire_fn<A>(), scope);
        }

        // 011/ADR-028 Phase 2: `IdleTimeout<Ms>` (compile-time ms) resolved to a runtime tick count
        // ONCE here, using this engine's configured wheel granularity (`idle_tick_ms`, BuildOnly —
        // see EngineConfig). 0 stays 0 (KeepAlive/no policy ⇒ no idle eviction, unchanged default).
        constexpr std::uint64_t idle_ms = idle_timeout_ms_of<A>();
        const std::uint32_t idle_ticks =
            idle_ms == 0 ? 0
                        : static_cast<std::uint32_t>(
                              std::max<std::uint64_t>(1, idle_ms / cfg_.idle_tick_ms));

        const ActorId id = actor_id_of<A>(key);
        register_activation(id, *act, static_cast<std::uint16_t>(priority_band_of<A>()),
                            drain_budget_of<A>(), idle_ticks);
        owned_actors_.push_back(std::move(actor));       // engine owns instance + Activation lifetime
        owned_activations_.push_back(std::move(act));
        return id;
    }

    // --- Lazy declaration (008 startup integration; COLD; ADR-028 Phase 3/4) ---------------
    // Compile + publish A's ActorMetadata (dispatch table, policy pack, supervision, factories)
    // into this engine's TypeRegistry WITHOUT constructing an instance or a Schedulable. `resolve()`/
    // `by_id_` are entirely untouched by this call: a type declared only via `declare_lazy<A>()` has
    // no live actor until its first message routes it through the per-shard `ActivationBroker`
    // (Phase 4), which consumes this record by `TypeKey` to build the lazy activation.
    // Same fail-fast Validation as `spawn<A>()` (Strict/Permissive per `EngineConfig::validation`);
    // `check` is the optional 004 resource-wire validation closure `TypeRegistry::register_type`
    // already supports (undeclared resource ⇒ errc::validation in Strict mode, same as `spawn<A>`).
    // `scope`/`reclaim` (ADR-028 Phase 4) are stored (not just validated against): the broker's real
    // first construction wires `A`'s resources against `*scope` exactly like `spawn<A>()` does today
    // (ADR-021: same scope, resolved once) and wires every subsequent message's reclaim to `reclaim`.
    // Omitting them (the Phase-3 call shape) is still fully supported: an `A` with no `wire()` needs
    // neither; an `A` WITH `wire()` but no `scope` here will fail cleanly at first real construction
    // (dead-lettered, never a null-pointer hazard) rather than at `declare_lazy` time.
    template <class A, class WireCheck>
        requires std::is_invocable_r_v<result<void>, WireCheck&>
    [[nodiscard]] result<std::uint16_t> declare_lazy(WireCheck&& check,
                                                     const ResourceScope* scope = nullptr,
                                                     ReclaimSink reclaim = {}) {
        static_assert(is_actor<A>, "declare_lazy<A>: A must derive from quark::Actor<A, ...>");
        static_assert(validate_actor_policies<A>(), "declare_lazy<A>: actor policy validation failed");
        static_assert(priority_band_of<A>() < Policy::bands,
                      "declare_lazy<A>: Priority<P> exceeds the engine's PriorityBands<K> (P < K) — "
                      "ADR-010");
        return type_registry_.register_type<A>(std::forward<WireCheck>(check), scope, reclaim);
    }
    template <class A>
    [[nodiscard]] result<std::uint16_t> declare_lazy(const ResourceScope* scope = nullptr,
                                                     ReclaimSink reclaim = {}) {
        return declare_lazy<A>(
            [&]() -> result<void> {
                if constexpr (has_resource_wire<A>) {
                    if (scope == nullptr) return {};  // nothing to validate without a scope (see above)
                    A tmp;
                    return tmp.wire(*scope);
                } else {
                    return {};
                }
            },
            scope, reclaim);
    }

    // ADR-028 Phase 5: declare `A` against a concrete `Store` `store` so the broker's one-time lazy
    // construction (`handle_wake`) generically recovers persisted state via `recover_snapshot` before
    // the actor's first message dispatches. `A` MUST declare `Persistent<Snapshot,...>` — checked here
    // at compile time so a misuse (calling this overload for a non-persistent or EventSourced actor)
    // fails to compile rather than silently registering an inert `recover`. `store` must outlive every
    // activation of `A` (the same lifetime contract `scope`/`reclaim` already carry).
    template <class A, class S>
        requires Store<S>
    [[nodiscard]] result<std::uint16_t> declare_lazy(S& store, const ResourceScope* scope = nullptr,
                                                     ReclaimSink reclaim = {}) {
        static_assert(is_actor<A>, "declare_lazy<A>: A must derive from quark::Actor<A, ...>");
        static_assert(validate_actor_policies<A>(), "declare_lazy<A>: actor policy validation failed");
        static_assert(priority_band_of<A>() < Policy::bands,
                      "declare_lazy<A>: Priority<P> exceeds the engine's PriorityBands<K> (P < K) — "
                      "ADR-010");
        static_assert(is_snapshot_persistent_v<A>,
                      "declare_lazy<A>(store, ...): A must declare Persistent<Snapshot,...> to use the "
                      "store-taking overload (ADR-028 Phase 5); use declare_lazy<A>(scope, reclaim) "
                      "otherwise");
        return type_registry_.register_type<A>(store, scope, reclaim);
    }

    // Read-only access to the compiled metadata registry (008) — e.g. `find(type_key_of<A>())` to
    // inspect a declared-but-not-yet-spawned type's compiled record.
    [[nodiscard]] const TypeRegistry& type_registry() const noexcept { return type_registry_; }

    // --- Addressing (006) ------------------------------------------------------------------
    // Resolve a stable ActorId to its registered Schedulable. Local-only; returns nullptr for an
    // unregistered id. A pure lookup — no allocation, no mutation — so it stays on the `tell`/`ask`
    // 0-alloc hot path. `by_id_` (cold, frozen after `start()`) is checked FIRST — zero added cost
    // for every eagerly-`spawn`'d actor, unchanged from before ADR-028 Phase 4. Only on a miss does it
    // fall through to this id's shard's lock-free-read `id_table` (ADR-028 Phase 4: entries the
    // ActivationBroker publishes at runtime) — the documented, opt-in extra cost of a lazily-declared
    // (never eagerly `spawn`'d) actor: every lookup after its first pays one extra map miss, forever.
    [[nodiscard]] Schedulable* resolve(ActorId id) const noexcept {
        const auto it = by_id_.find(id);
        if (it != by_id_.end()) return it->second;
        return shards_[shard_of(id)].id_table.find(id);
    }

    // Hand out the Policy-erased courier `ActorRef`/`LocalRouter` (006) use to resolve + post.
    [[nodiscard]] PostCourier post_courier() noexcept {
        return PostCourier{this, &resolve_courier, &post_courier_fn, &activate_courier_fn};
    }

    // --- Producer hot path (002 §Wakeup) ---------------------------------------------------
    // Enqueue a message on the actor's mailbox; if THIS caller drove Idle→Scheduled, carry the
    // activation onto its shard run-queue and wake EXACTLY ONE sleeping worker (never broadcast).
    QUARK_ALWAYS_INLINE bool post(Schedulable* s, Descriptor* d) noexcept {
        const bool wake = s->activation->post(d);  // mailbox enqueue + producer fence + exec CAS
        if (wake) schedule_and_wake(s);
        return wake;
    }

    // If the caller enqueued through the mailbox directly, it must drive the wake edge with this.
    QUARK_ALWAYS_INLINE bool notify(Schedulable* s) noexcept {
        const bool wake = s->activation->notify_enqueued();
        if (wake) schedule_and_wake(s);
        return wake;
    }

    // --- 015 re-admit (blocking/fiber/async completion) ------------------------------------
    // Drive the parked activation's completion and, on the Parked→Scheduled edge, re-enqueue +
    // wake. This carries the SAME seq_cst Dekker rendezvous as post() but is a DISTINCT StoreLoad
    // pair (carrier→actor, not producer→consumer — ADR-015).
    bool complete_parked(Schedulable* s) noexcept {
        const bool wake = s->activation->complete_parked();
        if (wake) schedule_and_wake(s);
        return wake;
    }

    // --- Lifecycle -------------------------------------------------------------------------
    void start() {
        if (running_) return;
        running_ = true;
        stopping_.store(false, std::memory_order_release);
        // Precompute each worker's shard scan order: OWN shards first (locality), then the rest
        // (the stealing path). Own = shards whose home worker is this worker.
        for (std::uint32_t w = 0; w < cfg_.worker_count; ++w) {
            auto& order = workers_[w].scan_order;
            order.clear();
            for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh)
                if (home_worker(sh) == w) order.push_back(sh);
            for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh)
                if (home_worker(sh) != w) order.push_back(sh);
        }
        threads_.reserve(cfg_.worker_count);
        for (std::uint32_t w = 0; w < cfg_.worker_count; ++w)
            threads_.emplace_back([this, w] { worker_loop(w); });

        // ADR-028 Phase 2 (011 §"engine-wide backstop thread"): a fully-idle shard's worker parks and
        // never revisits try_drain_shard on its own — nothing would ever advance that shard's wheel (a
        // Deactivate armed on an otherwise-silent actor would never fire). Periodically visit every
        // shard directly (backstop_tick_shard) to advance its wheel. This deliberately does NOT go
        // through wake_all()/wake_one()/idle_mask_ — an earlier version did, and it masked
        // sched_no_lost_wakeup_control's deliberately-broken-park() control (a periodic broadcast wake
        // incidentally rescues a permanently-stranded worker, hiding the exact bug that test exists to
        // detect). backstop_tick_shard also never drains the run-queue, so it cannot rescue stranded
        // real work either — it only ever touches the wheel, under the SAME drain_owner exclusion
        // try_drain_shard uses, so it adds no new synchronization primitive and cannot mask a lost
        // wakeup: a park()/idle_mask_ bug is completely untouched by anything this thread does.
        backstop_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.idle_tick_ms));
                if (stopping_.load(std::memory_order_acquire)) return;
                for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh) backstop_tick_shard(sh);
            }
        });
    }

    // Clean shutdown: signal stop, wake every worker, drain everything pending, then join.
    void stop() {
        if (!running_) return;
        stopping_.store(true, std::memory_order_release);
        wake_all();
        backstop_.request_stop();
        if (backstop_.joinable()) backstop_.join();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        threads_.clear();
        running_ = false;
    }

    // --- Configuration (013/ADR-008) -------------------------------------------------------
    // The Live operational read-set word. The hot path reads through this (single relaxed load +
    // mask, 0 cross-core RMW, no tear). ENGINE-WIDE single cell here; the per-(shard × type_index)
    // padded cell array (ADR-008 §Consequences) is the documented scaling seam.
    [[nodiscard]] const HotCell& operational() const noexcept { return hot_; }

    // The engine-wide default drain budget — the 023 operational-read gate (`mov + and`).
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t default_drain_budget() const noexcept {
        return hot_.drain_budget();
    }

    // Live control-plane reconfig (013 §Live reconfiguration). Re-resolves the affected Live fields,
    // VALIDATES fail-fast (out-of-range ⇒ `errc::validation`, never truncates), then publishes ONE
    // re-packed word with a single relaxed store — the store IS the publish, so it can never stall a
    // drain. FROZEN-CORE fields are not expressible in an `OperationalDelta` (compile-time BuildOnly
    // enforcement). Returns a `ReconfigReceipt` on success.
    [[nodiscard]] result<ReconfigReceipt> reconfigure(const OperationalDelta& delta) noexcept {
        const OperationalConfig next = delta.apply_to(hot_.decode());
        const auto packed = validate_operational(next);  // fail-fast, no partial publish
        if (!packed) return std::unexpected<error>(packed.error());
        hot_.publish(*packed);  // single relaxed store — the publish; nothing to reclaim
        return ReconfigReceipt{.published_word = *packed, .masked_count = 0};
    }

    // --- 022 Resource Governance & Overload Control ----------------------------------------
    // Enable the bounded mailbox + Overflow policy + (optional) deadline-aware shedding on a
    // registered activation (COLD; at registration). The LIVE bound/overflow/shed values are read
    // from THIS engine's HotCell — the same 0-RMW word `reconfigure()` publishes, so an operator can
    // retune the bound/overflow of governed actors live without a recompile (013). A message posted
    // to a governed actor MUST go through `governed_post` so the producer-side depth counter stays
    // accurate; the ungoverned `post()` hot path is untouched (zero cost for ungoverned actors).
    void enable_governance(Schedulable* s, bool deadline_shed = false,
                           std::uint32_t shed_threshold = 0) noexcept {
        Activation::GovernanceConfig gc;
        gc.hot = &hot_;
        gc.deadline_shed = deadline_shed;
        gc.shed_threshold = shed_threshold;
        s->activation->enable_governance(gc);
    }

    // Governed producer admission (022 §Load shedding). Enforces the bound + Overflow BEFORE the
    // mailbox enqueue and, on an Admitted frame that drove Idle→Scheduled, carries the activation
    // onto its run-queue and wakes one worker (exactly like `post`). On Blocked/Shed the frame is NOT
    // enqueued — the CALLER owns the descriptor (return it to the send pool / fail the `ask` cell);
    // the lane's single-writer pool + dead-letter sink are never touched off-lane (single-executor).
    [[nodiscard]] Activation::PostAdmission governed_post(Schedulable* s, Descriptor* d) noexcept {
        const Activation::PostAdmission pa = s->activation->post_governed(d);
        if (pa.result == Activation::AdmitResult::Admitted && pa.wake) schedule_and_wake(s);
        return pa;
    }

    // --- Observers / test seams ------------------------------------------------------------
    [[nodiscard]] std::uint32_t worker_count() const noexcept { return cfg_.worker_count; }
    [[nodiscard]] std::uint32_t shard_count() const noexcept { return cfg_.shard_count; }
    [[nodiscard]] std::uint32_t shard_of(ActorId id) const noexcept { return shard_of_hash(id.hash()); }
    [[nodiscard]] std::uint32_t shard_of_hash(std::uint64_t h) const noexcept {
        return shard_pow2_ ? static_cast<std::uint32_t>(h & shard_mask_)
                           : static_cast<std::uint32_t>(h % cfg_.shard_count);
    }
    // True iff any shard run-queue currently holds work (acquire probe — never mutates).
    [[nodiscard]] bool any_work() const noexcept {
        for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh)
            if (shards_[sh].run_queue.has_work()) return true;
        return false;
    }

    // --- 009 Observability — the consumer-facing metrics surface --------------------------
    // Off the hot path (aggregates every shard's counters on read). `engine.metrics_snapshot()` is
    // the exact name 009-Observability.md documents as the embedding/test surface.
    [[nodiscard]] MetricsSnapshot metrics_snapshot() const { return metrics_.snapshot(); }
    // Prometheus text exposition — pure string formatting, no client library (009 §Export).
    [[nodiscard]] std::string metrics_prometheus() const { return metrics_.to_prometheus(); }
    // Direct registry access (e.g. `set_user_counter_name` before/while the engine is running).
    [[nodiscard]] MetricsRegistry& metrics_registry() noexcept { return metrics_; }

private:
    // Courier wrappers (bound into PostCourier). Static so they are plain `.rodata` fn-ptr targets
    // — no virtual, no per-Policy indirection beyond the one already-typed call.
    static Schedulable* resolve_courier(void* eng, ActorId id) noexcept {
        return static_cast<Engine*>(eng)->resolve(id);
    }
    static bool post_courier_fn(void* eng, Schedulable* s, Descriptor* d) noexcept {
        return static_cast<Engine*>(eng)->post(s, d);
    }
    // ADR-028 Phase 4: the lazy-activation cold-path hand-off wrapper.
    static bool activate_courier_fn(void* eng, ActorId id, Descriptor* d,
                                    ReclaimSink origin_reclaim) noexcept {
        return static_cast<Engine*>(eng)->activate(id, d, origin_reclaim);
    }

    static constexpr std::uint32_t kNoOwner = 0xFFFF'FFFFu;
    // ADR-028 Phase 2: the backstop thread's drain_owner tag — distinct from every real `wid` (which
    // only ever ranges [0, worker_count)) and from kNoOwner, so backstop_tick_shard's brief ownership
    // is never confused with a real worker's session.
    static constexpr std::uint32_t kBackstopOwner = 0xFFFF'FFFEu;

    // ============================================================================================
    // ADR-028 Phase 4 — lazy activation: the per-shard id-table, ownership record, and broker.
    // ============================================================================================

    // Permanent, per-shard, single-writer-insert / lock-free-read table for broker-constructed
    // (lazily activated) actors. NOT `by_id_`: `by_id_` (a plain unordered_map) is safe only because
    // it is mutated cold, before `start()` — mutating it at runtime from concurrently-running
    // per-shard brokers would be a data race. Single-writer BY CONSTRUCTION: only THIS shard's own
    // `ActivationBroker` ever calls `insert` (a Sequential Activation — never two workers touching
    // the same shard's table at once). Readers are arbitrary producer threads calling `resolve()`
    // concurrently with an insert; `find` never locks, only acquire-ordered loads down an append-only
    // chain (mirrors the ADR's own proven torn-read-free id-table shape, S4). Entries are never
    // removed — permanent, matching the ADR's own documented residual risk (unbounded growth under
    // huge-cardinality one-shot ids; no compaction seam here, out of scope).
    class ShardIdTable {
    public:
        explicit ShardIdTable(std::size_t bucket_count = 1024) : buckets_(bucket_count) {
            for (auto& b : buckets_) b.store(nullptr, std::memory_order_relaxed);
        }
        ShardIdTable(const ShardIdTable&) = delete;
        ShardIdTable& operator=(const ShardIdTable&) = delete;
        ~ShardIdTable() {
            for (auto& head : buckets_) {
                Entry* e = head.load(std::memory_order_relaxed);
                while (e != nullptr) {
                    Entry* next = e->next.load(std::memory_order_relaxed);
                    delete e;
                    e = next;
                }
            }
        }

        // Single-writer (this shard's broker only). Publishes with a release store — see `find`.
        void insert(ActorId id, Schedulable* s) {
            auto* e = new Entry{id, s, nullptr};
            const std::size_t b = bucket_of(id);
            e->next.store(buckets_[b].load(std::memory_order_relaxed), std::memory_order_relaxed);
            buckets_[b].store(e, std::memory_order_release);  // the publish
        }

        // Lock-free concurrent reader — no lock, no CAS, only acquire loads.
        [[nodiscard]] Schedulable* find(ActorId id) const noexcept {
            const std::size_t b = bucket_of(id);
            for (const Entry* e = buckets_[b].load(std::memory_order_acquire); e != nullptr;
                e = e->next.load(std::memory_order_acquire))
                if (e->id == id) return e->sched;
            return nullptr;
        }

    private:
        struct Entry {
            ActorId id;
            Schedulable* sched;
            std::atomic<Entry*> next;
        };
        [[nodiscard]] std::size_t bucket_of(ActorId id) const noexcept {
            return static_cast<std::size_t>(id.hash()) % buckets_.size();
        }
        std::vector<std::atomic<Entry*>> buckets_;
    };

    // Per-shard ownership of broker-constructed {actor, Activation} pairs (ADR-028 Phase 4 bug-(a)
    // fix): lives on `Shard`, NEVER a single Engine-wide container — only this shard's own broker,
    // running exclusively on whichever worker currently holds THIS shard's drain_owner, ever appends
    // here. A shared/Engine-wide container mutated by concurrently-running per-shard brokers was the
    // exact TSan-proven bug the ADR's proving run found and required fixed.
    struct LazyOwned {
        void* actor = nullptr;
        DestroyFn destroy = nullptr;
        std::unique_ptr<Activation> act;
        std::unique_ptr<Schedulable> sched;  // per-shard Schedulable ownership — see build_schedulable

        LazyOwned() = default;
        LazyOwned(void* a, DestroyFn d, std::unique_ptr<Activation> ac,
                 std::unique_ptr<Schedulable> sc) noexcept
            : actor(a), destroy(d), act(std::move(ac)), sched(std::move(sc)) {}
        // NOT `= default`: `actor`/`destroy` are plain pointers, which a defaulted move would only
        // COPY (raw pointers have no move semantics) — leaving the moved-FROM element's destructor
        // to `delete` the SAME `actor` a second time once `std::vector<LazyOwned>` reallocates (every
        // `emplace_back` past the first move-constructs prior elements into the new buffer, then
        // destroys the old ones). Must explicitly null the source's `actor`/`destroy` to disarm it.
        LazyOwned(LazyOwned&& o) noexcept
            : actor(std::exchange(o.actor, nullptr)), destroy(std::exchange(o.destroy, nullptr)),
              act(std::move(o.act)), sched(std::move(o.sched)) {}
        LazyOwned& operator=(LazyOwned&& o) noexcept {
            if (this != &o) {
                if (destroy != nullptr && actor != nullptr) destroy(actor);
                actor = std::exchange(o.actor, nullptr);
                destroy = std::exchange(o.destroy, nullptr);
                act = std::move(o.act);
                sched = std::move(o.sched);
            }
            return *this;
        }
        LazyOwned(const LazyOwned&) = delete;
        LazyOwned& operator=(const LazyOwned&) = delete;
        ~LazyOwned() {
            if (destroy != nullptr && actor != nullptr) destroy(actor);
        }
    };

    // The broker's own control message: the target id, the ORIGINAL first-touch descriptor, and the
    // origin caller's reclaim sink (used ONLY if construction never starts/fails before any Activation
    // exists to own the message — see `handle_wake`). Engine-internal only, never a user-facing type.
    // `enqueued_ns` (ADR-028 Phase 7 §009 broker convoy observability, residual risk #2) is stamped in
    // `activate()` and read back at the top of `handle_wake` to record `broker_stall_ns`.
    struct Wake {
        ActorId id;
        Descriptor* first = nullptr;
        ReclaimSink first_reclaim{};
        std::int64_t enqueued_ns = 0;
    };

    // Real wall-clock nanoseconds (pal::BootClock — CLOCK_BOOTTIME-class, see pal/pal.hpp), used ONLY
    // for the broker's own stall observability (009). Not injectable via Activation::set_clock — the
    // broker is engine-internal plumbing, not a simulated actor's deadline/restart-window clock.
    [[nodiscard]] static std::int64_t wall_now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch()).count();
    }

    // The per-shard lazy-activation broker (ADR-028 Phase 4): an ORDINARY internal actor that reuses
    // the engine's own single-executor guarantee to arbitrate first-touch construction — the ADR's
    // winning "Broker-Actor Serialization" design — instead of inventing new CAS/RCU machinery.
    // Sequential, not `MaxConcurrency<K>`-bounded: `construct_and_wire` here is fully synchronous (no
    // persistence/`co_await` yet — Phase 5), so a Reentrant broker would buy no throughput while
    // importing the ADR's bug-(b) risk (an `activating_` set with erase-strictly-after-publish
    // ordering to get right) for free; Sequential sidesteps it by construction (no second Wake for
    // the same id is even LOOKED AT until the first one's id-table publish has already committed).
    // Revisit if/when Phase 5 makes construction genuinely async. Never addressed via `ActorId`/
    // `resolve()`/`ActorRef` — a pure engine-internal implementation detail, stored on `Shard`.
    //
    // ADR-028 Phase 7 (residual risk #5 — "a dedicated SupervisionPolicy, not inherited defaults"):
    // `OnFailure<Restart>` is declared EXPLICITLY here rather than left to `supervision_of<A>()`'s
    // implicit spec-default (which already computes to the same Restart/unbounded value) — a
    // deliberate, documented choice, not an accident of what the default happens to be today.
    // Structurally unreachable as of this phase: `handle_wake` below is now exception-safe (its own
    // try/catch converts any construct/wire/recover fault into a synchronous dead-letter, never an
    // escaping exception), so this policy is a regression guard, not a currently-exercised path.
    class ActivationBroker : public Actor<ActivationBroker, Sequential, OnFailure<Restart>> {
    public:
        using protocol = Protocol<Wake>;
        ActivationBroker(Engine* eng, std::uint32_t shard_id) noexcept
            : eng_(eng), shard_id_(shard_id) {}
        void handle(const Wake& w) noexcept { eng_->handle_wake(shard_id_, w); }

    private:
        Engine* eng_;
        std::uint32_t shard_id_;
    };

    struct alignas(::quark::cache_line_size) Shard {
        RunQueue<Policy> run_queue;
        QUARK_CACHE_ALIGNED std::atomic<std::uint32_t> drain_owner{kNoOwner};  // null→self per session
        ShardCounters metrics;  // 009: this shard's counter block (cold-allocated with the array)
        // ADR-028 Phase 2: the per-shard idle-eviction wheel + its Deactivate-entry pool. Single-
        // writer by construction — only the worker currently holding `drain_owner` for this shard ever
        // touches either (arm/tick/fire all happen inside try_drain_shard's owned critical section,
        // 011 §"per-shard, single-writer" wheel integration) — no atomics needed here.
        detail::TimingWheel wheel{};
        detail::TimerEntryPool wheel_pool{256};
        std::int64_t wheel_last_tick_ns = 0;  // 0 = not yet seeded; see try_drain_shard
        // ADR-028 Phase 4: lazy-activation state. `id_table`/`lazy_owned` are mutated ONLY by this
        // shard's own broker (see `ShardIdTable`/`LazyOwned`); the `broker_*` triple is this shard's
        // permanent, engine-internal broker (constructed once, in `Engine`'s constructor).
        ShardIdTable id_table;
        std::vector<LazyOwned> lazy_owned;
        std::unique_ptr<Schedulable> broker_schedulable_owner;  // per-shard — NOT registry_ (see above)
        Schedulable* broker_schedulable = nullptr;              // == broker_schedulable_owner.get()
        std::unique_ptr<Activation> broker_activation;
        std::unique_ptr<ActivationBroker> broker_instance;
    };

    // ADR-028 Phase 4: the `LocalRouter` cold-path hand-off (`resolve() == nullptr`). Checks
    // `type_registry_` SYNCHRONOUSLY first (cold read; frozen after `start()` — `declare_lazy` is
    // only ever called before it, exactly like `spawn<A>()`): if the type was never `declare_lazy`'d,
    // returns `false` immediately so the caller's EXISTING synchronous dead-letter fires unchanged —
    // this is what keeps every pre-Phase-4 dead-letter test's immediate-reclaim semantics intact.
    // Only a genuinely-declared type is handed to its shard's broker.
    [[nodiscard]] bool activate(ActorId id, Descriptor* first, ReclaimSink origin_reclaim) noexcept {
        if (type_registry_.find(id.type) == nullptr) return false;
        const std::uint32_t sid = shard_of(id);
        Shard& sh = shards_[sid];
        detail::MessagePool::Slot slot = broker_pool_.acquire(&detail::destroy_payload<Wake>);
        Descriptor* wd = slot.desc;
        ::new (slot.payload) Wake{id, first, origin_reclaim, wall_now_ns()};
        wd->payload = slot.payload;
        wd->payload_size = static_cast<std::uint32_t>(sizeof(Wake));
        wd->trace_id = 0;
        wd->deadline_ns = 0;
        stamp<ActivationBroker, Wake>(*wd);
        // ADR-028 Phase 7 (009 broker convoy observability, residual risk #2): producer side — many
        // threads may call `activate()` concurrently (for the same or different cold ids), so this
        // mirrors the existing `mailbox_enqueued` convention (`inc_atomic()`, a real fetch_add).
        sh.metrics.broker_wakes_enqueued.inc_atomic();
        // `post()`'s return is the WAKE-EDGE bool (did THIS call need to wake a worker for the
        // broker?) — false the vast majority of the time under any concurrency (the broker is
        // already Scheduled/Running), NOT "did the hand-off fail". The enqueue itself always
        // succeeds here (the mailbox has no bound). Returning `post()`'s bool directly would tell
        // `LocalRouter` to ALSO reclaim `first` on every non-waking call — a double-use of a
        // descriptor already owned by the in-flight `Wake` message. Always report success once
        // enqueued; only the earlier type_registry_ miss ever returns false.
        (void)post(sh.broker_schedulable, wd);
        return true;
    }

    // The broker's own handler (runs on `ActivationBroker`'s Sequential lane — see its class comment
    // for why that fully serializes every Wake for this shard, sidestepping the ADR's bug (b)).
    //
    // ADR-028 Phase 7 exception safety: `handle()` (the caller) is `noexcept`, so ANY exception that
    // would otherwise escape this function terminates the whole process, not just this activation
    // (confirmed: `make_construct_fn<A>()` compiles a plain `new A()`, which can throw — a user's
    // actor constructor throwing, or a bare `bad_alloc`, used to crash the entire engine on its first
    // lazy touch). The try/catch below converts any such fault into the SAME dead-letter-and-return
    // shape the pre-existing wire-failure/recover-failure blocks already use. Ownership is tracked via
    // `owned`: `sh.lazy_owned.emplace_back(...)` runs BEFORE `sh.id_table.insert(...)` (swapped from
    // the original insert-then-emplace order — safe, since bug (b) only requires the id-table publish
    // to happen before `post()`/the next `handle_wake` call, not before this purely-local bookkeeping),
    // so a throw from `ShardIdTable::insert()` (not noexcept — does `new Entry{...}`) after ownership
    // already transferred never double-frees `actor` via the catch block's `m->destroy(actor)`.
    void handle_wake(std::uint32_t sid, const Wake& w) noexcept {
        Shard& sh = shards_[sid];
        // ADR-028 Phase 7 (009 broker convoy observability, residual risk #2): counted for EVERY Wake,
        // including the duplicate/racing-first-touch fast path below — both are "handled" by the broker.
        sh.metrics.broker_wakes_handled.inc();
        const std::int64_t stall_ns = wall_now_ns() - w.enqueued_ns;
        sh.metrics.broker_stall_ns.record(static_cast<std::uint64_t>(stall_ns > 0 ? stall_ns : 0));
        if (Schedulable* existing = sh.id_table.find(w.id)) {
            (void)post(existing, w.first);  // a duplicate/racing first-touch — already live, deliver
            return;
        }
        const ActorMetadata* m = type_registry_.find(w.id.type);  // guaranteed present (see activate())
        void* actor = nullptr;
        bool owned = false;
        try {
            actor = m->construct();
            if (m->wire != nullptr) {  // A declared resources (has_resource_wire<A>)
                // `declare_lazy<A>(&scope)` already validated THIS SAME scope in Strict mode, so this
                // is structurally unreachable in the intended (Strict + matching scope) usage —
                // defensive only. `m->scope == nullptr` (a resource-bearing A declared without a
                // scope) hits this same path deterministically, by design (dead-letter, never a
                // null-pointer hot-path hit).
                bool ok = false;
                if (m->scope != nullptr) {
                    if (result<void> wired = m->wire(actor, *m->scope); wired) ok = true;
                }
                if (!ok) {
                    m->destroy(actor);
                    w.first_reclaim(w.first);
                    return;
                }
            }
            // ADR-028 Phase 5: A declared Persistent<Snapshot,...> via declare_lazy<A>(store, ...) —
            // recover its persisted state (or seed from its own default via snapshot_state()) exactly
            // once, before the actor's first message dispatches. A failure dead-letters `first`
            // through the SAME construct/destroy/reclaim shape as a wire failure above — never a
            // half-constructed actor.
            if (m->recover != nullptr) {
                if (result<void> rec = m->recover(actor, m->store, w.id); !rec) {
                    m->destroy(actor);
                    w.first_reclaim(w.first);
                    return;
                }
            }
            auto act = std::make_unique<Activation>(actor, m->dispatch, m->reclaim, m->max_concurrency,
                                                    m->supervision);
            act->set_reconstruct(m->reconstruct);
            // 007 Restart re-wire ("Phase 6" redirected): a lazily-broker-constructed actor gets the
            // same re-wire-on-Restart coverage a spawn<A>()'d one does — m->wire/m->scope are null
            // unless the actor declared resources (matching the guard above), so this is a no-op
            // otherwise.
            act->set_resource_wire(m->wire, m->scope);
            const std::uint32_t idle_ticks =
                m->idle_timeout_ms == 0
                    ? 0
                    : static_cast<std::uint32_t>(
                          std::max<std::uint64_t>(1, m->idle_timeout_ms / cfg_.idle_tick_ms));
            std::unique_ptr<Schedulable> s =
                build_schedulable(sid, *act, m->band, m->drain_budget, idle_ticks);
            Schedulable* raw = s.get();
            // Per-shard ONLY (bug (a) fix) — never registry_/an Engine-wide container; see
            // build_schedulable's comment for the concurrent-multi-shard use-after-free this replaced.
            sh.lazy_owned.emplace_back(actor, m->destroy, std::move(act), std::move(s));
            owned = true;  // ownership now belongs to sh.lazy_owned's LazyOwned destructor
            sh.id_table.insert(w.id, raw);  // publish — BEFORE delivering `first` (bug (b) ordering)
            post(raw, w.first);  // deliver the ORIGINAL message through the ordinary, proven post() path
        } catch (...) {
            if (!owned && actor != nullptr) m->destroy(actor);
            w.first_reclaim(w.first);
        }
    }

    struct alignas(::quark::cache_line_size) Worker {
        std::atomic<std::uint32_t> wake_seq{0};  // personal futex word (targeted wakeup)
        std::vector<std::uint32_t> scan_order;   // shard scan order (own first), built cold at start()
    };

    [[nodiscard]] std::uint32_t home_worker(std::uint32_t shard) const noexcept {
        return shard % cfg_.worker_count;
    }

    // Producer half of the run-queue wake Dekker — order the run-queue enqueue before the idle_mask
    // load. ELIDED on x86-TSO (the enqueue's acq_rel exchange is a full StoreLoad barrier there).
    QUARK_ALWAYS_INLINE static void producer_wake_fence() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        // elided
#else
        pal::store_load_barrier();  // TODO(arm64): re-gate the run-queue Dekker under weak memory
#endif
    }

    QUARK_ALWAYS_INLINE void schedule_and_wake(Schedulable* s) noexcept {
        shards_[s->shard].run_queue.enqueue(s);  // carry the activation onto the shard run-queue
        producer_wake_fence();                    // Dekker: enqueue happens-before the idle_mask load
        if (wake_one()) {
            // 009: producer-side, possibly concurrent (many producers can post to the same shard) —
            // the genuinely-atomic path, not the drain-owner-exclusive inc(). Counted only when a
            // worker was ACTUALLY woken (not merely attempted — every worker busy is the common case).
            shards_[s->shard].metrics.wakeups.inc_atomic();
        }
    }

    // Wake EXACTLY ONE idle worker (targeted, never broadcast — 002 §Wakeup). If none is idle every
    // worker is busy and will re-scan on finishing its session (the park() Dekker rescan guarantees
    // it observes this enqueue), so no wakeup is lost. Returns true iff a worker was actually woken.
    bool wake_one() noexcept {
        std::uint32_t m = idle_mask_.load(std::memory_order_acquire);
        while (m != 0) {
            const std::uint32_t id = static_cast<std::uint32_t>(std::countr_zero(m));
            const std::uint32_t bit = 1u << id;
            if (idle_mask_.compare_exchange_weak(m, m & ~bit, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                workers_[id].wake_seq.fetch_add(1, std::memory_order_release);
                workers_[id].wake_seq.notify_one();
                return true;
            }
            // m was reloaded by the failed CAS — retry with the fresh idle set.
        }
        return false;  // every worker busy — no lost wakeup (the park() Dekker rescan catches it)
    }

    void wake_all() noexcept {  // cold, stop() only — shutdown broadcast, not the hot wake path.
        for (std::uint32_t w = 0; w < cfg_.worker_count; ++w) {
            workers_[w].wake_seq.fetch_add(1, std::memory_order_release);
            workers_[w].wake_seq.notify_one();
        }
    }

    // ---- The worker lane loop -------------------------------------------------------------
    void worker_loop(std::uint32_t wid) noexcept {
        detail::t_worker_id = wid;  // test/observability seam (current_worker_id())
        for (;;) {
            if (scan_and_run(wid)) continue;  // found + drained work somewhere
            if (stopping_.load(std::memory_order_acquire)) {
                if (scan_and_run(wid)) continue;  // final confirm nothing is left
                break;
            }
            park(wid);  // deschedule until targeted wake; returns early if it re-finds work
        }
    }

    // Scan shards in this worker's preference order; drain the first productive shard fully, then
    // restart from the top (own-shard-first locality). Returns true iff any activation was run.
    bool scan_and_run(std::uint32_t wid) noexcept {
        const auto& order = workers_[wid].scan_order;
        bool any = false;
        for (std::uint32_t sh : order) {
            if (try_drain_shard(wid, sh)) {
                any = true;
                return true;  // restart the scan → re-prefer own shards (locality)
            }
        }
        return any;
    }

    // ADR-028 Phase 2: the backstop thread's ONLY shard access — deliberately narrower than
    // try_drain_shard. Wins the SAME drain_owner CAS (so wheel access is still single-writer,
    // uncontended with a real worker's own session), advances the wheel, then releases. Never calls
    // drain_run_queue (so it can't rescue stranded real work) and never touches idle_mask_/wake_*/park
    // (so it can't mask a lost wakeup) — see the backstop_ comment in start() for why that distinction
    // is load-bearing (sched_no_lost_wakeup_control's deliberately-broken-park() build must still
    // strand a single worker permanently; this function must be inert with respect to that entirely).
    void backstop_tick_shard(std::uint32_t sid) noexcept {
        Shard& sh = shards_[sid];
        std::uint32_t expected = kNoOwner;
        if (!sh.drain_owner.compare_exchange_strong(expected, kBackstopOwner, std::memory_order_acquire,
                                                    std::memory_order_relaxed))
            return;  // a real worker (or a concurrent backstop pass) owns this shard right now — skip
        advance_wheel(sh);
        sh.drain_owner.store(kNoOwner, std::memory_order_release);
        pal::store_load_barrier();  // consumer half of the SAME drain-owner Dekker try_drain_shard uses
    }

    // Win the shard's drain-owner CAS (single-consumer arbitration; a cold per-session edge — this
    // IS the stealing gate), drain its run-queue to empty, then close out with the drain-owner
    // Dekker re-check. Returns true iff it ran at least one activation.
    bool try_drain_shard(std::uint32_t wid, std::uint32_t sid) noexcept {
        Shard& sh = shards_[sid];
        std::uint32_t expected = kNoOwner;
        if (!sh.drain_owner.compare_exchange_strong(expected, wid, std::memory_order_acquire,
                                                    std::memory_order_relaxed))
            return false;  // another worker owns this shard's drain right now — skip (never block)

        bool processed = false;
        for (;;) {
            drain_run_queue(sid, sh, processed);
            advance_wheel(sh);  // 011/ADR-028 Phase 2: opportunistic per-shard idle-eviction tick

            // Close out the drain session — the {drain_owner, run-queue tail} Dekker vs producers.
            sh.drain_owner.store(kNoOwner, std::memory_order_release);
            pal::store_load_barrier();  // consumer half of the drain-owner Dekker (always a barrier)
            if (!sh.run_queue.has_work()) break;  // truly empty — stay released

            // Work appeared in the release window: re-acquire and keep draining, else another
            // worker grabbed the drain and will handle it.
            expected = kNoOwner;
            if (!sh.drain_owner.compare_exchange_strong(expected, wid, std::memory_order_acquire,
                                                        std::memory_order_relaxed))
                break;
        }
        // 009: a steal = this worker drained a shard it doesn't own AND actually moved work — winning
        // an empty shard's CAS isn't a steal. Drain-owner-exclusive (only the CAS winner reaches here
        // for this shard), so the plain non-atomic-RMW inc() is safe.
        if (processed && home_worker(sid) != wid) sh.metrics.steals.inc();
        return processed;
    }

    // Cap catch-up ticks per call so a long-idle shard (worker parked, no visits for a while) can't
    // turn its next visit into an unbounded loop — a bounded backlog drain instead (011 §"drift
    // between the timekeeper's tick and per-shard active advance" is an accepted, bounded looseness).
    static constexpr int kMaxWheelCatchUpTicks = 4096;

    // ADR-028 Phase 2: advance this shard's wheel by however many real `idle_tick_ms` intervals have
    // elapsed since its last tick, firing any Deactivate entries that came due. Called only from
    // inside try_drain_shard's drain-owner-exclusive section (single-writer by construction — see the
    // Shard member comment), so `wheel`/`wheel_pool`/`wheel_last_tick_ns` need no synchronization.
    void advance_wheel(Shard& sh) noexcept {
        const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      pal::now().time_since_epoch())
                                      .count();
        if (sh.wheel_last_tick_ns == 0) {
            sh.wheel_last_tick_ns = now;  // first visit: seed, no backlog to fire
            return;
        }
        const std::int64_t tick_ns = static_cast<std::int64_t>(cfg_.idle_tick_ms) * 1'000'000;
        for (int guard = 0; guard < kMaxWheelCatchUpTicks && now - sh.wheel_last_tick_ns >= tick_ns;
             ++guard) {
            sh.wheel_last_tick_ns += tick_ns;
            sh.wheel.tick([this, &sh](detail::TimerEntry* e) { on_deactivate_fire(sh, e); });
        }
    }

    // ADR-028 Phase 2: arm a one-shot Deactivate entry for `act`, `idle_ticks()` wheel-ticks from now.
    // Called only from run_activation right after close_out() reports a genuine (non-Dormant) Idle —
    // the shard's drain-owner exclusivity makes arm/fire/cancel single-writer with no new fence.
    void arm_deactivate(Shard& sh, Activation* act, Schedulable* s) noexcept {
        detail::TimerEntry* e = sh.wheel_pool.acquire();
        e->period_ticks = 0;  // one-shot — a fresh idle edge re-arms, never a periodic re-fire
        e->expiry_ticks = sh.wheel.now_ticks() + act->idle_ticks();
        struct Ctx {
            Activation* act;
            Schedulable* s;
        };
        static_assert(sizeof(Ctx) <= detail::TimerEntry::kInlineCap);
        ::new (static_cast<void*>(e->storage)) Ctx{act, s};
        act->set_armed_deactivate_entry(e);
        sh.wheel.insert(e);
    }

    // The wheel's fire callback. `e` may be STALE — the activation went busy and armed a newer entry
    // since, or was already evicted — because arming never scans/cancels a prior entry out of its
    // bucket (011 §"re-arm-with-cancel" / "O(1) under flap": a stale fire is a cheap no-op instead).
    // The activation's own `armed_deactivate_entry()` pointer is the sole liveness discriminant.
    void on_deactivate_fire(Shard& sh, detail::TimerEntry* e) noexcept {
        struct Ctx {
            Activation* act;
            Schedulable* s;
        };
        auto* ctx = reinterpret_cast<Ctx*>(e->storage);
        Activation* act = ctx->act;
        Schedulable* s = ctx->s;
        const bool live = act->armed_deactivate_entry() == e;
        sh.wheel_pool.release(e);
        if (!live) return;  // stale — a busy edge cancelled it, or a newer arm already won
        act->set_armed_deactivate_entry(nullptr);
        Descriptor* d = act->deactivate_descriptor();
        d->release();                          // single-writer in-place reset: Queued, gen++, flags=0
        d->set_flags(kControlFlagDeactivate);  // re-tag as the Deactivate control descriptor
        act->mailbox().enqueue(d);
        notify(s);  // the SAME producer wake path every real message uses (Idle/Dormant->Scheduled)
    }

    // Pop and run activations until the run-queue reports Empty (bounded-spin on the non-linearizable
    // Busy publish window — never unbounded, ADR-010 normative note).
    void drain_run_queue(std::uint32_t sid, Shard& sh, bool& processed) noexcept {
        unsigned busy = 0;
        for (;;) {
            const RunResult r = sh.run_queue.select();
            if (r.status == RunStatus::Item) {
                processed = true;
                busy = 0;
                run_activation(sid, r.item);
                continue;
            }
            if (r.status == RunStatus::Busy) {
                if (++busy < cfg_.busy_spin_limit) {
                    detail::cpu_relax();
                    continue;
                }
                break;  // spun out — the close-out re-check / next wakeup catches it
            }
            break;  // Empty
        }
    }

    // Run ONE activation exactly as the 001 Activation banner documents.
    void run_activation(std::uint32_t sid, Schedulable* s) noexcept {
        Activation* act = s->activation;
        if (!act->try_acquire()) return;  // Scheduled→Running; false ⇒ skip, never block

        unsigned busy = 0;
        for (;;) {
            const Activation::DrainOutcome o = act->drain_step(s->budget);
            if (o == Activation::DrainOutcome::DrainedEmpty) {
                if (act->close_out()) {  // re-acquired: a producer added work in the close-out window
                    busy = 0;
                    continue;
                }
                // Genuinely relinquished (never re-acquired Running) — either released to Idle, or
                // just evicted to Dormant by close_out_retire() (ADR-028 Phase 1). `went_dormant()`
                // is the one cheap post-hoc check that tells the two apart: only a real Idle gets a
                // fresh Deactivate token armed (011/ADR-028 Phase 2) — an evicted activation has
                // nothing to arm until a future message reactivates it and it goes idle again.
                if (!act->went_dormant() && act->idle_ticks() != 0) {
                    arm_deactivate(shards_[sid], act, s);
                }
                return;  // released to Idle (or evicted to Dormant) — done
            }
            if (o == Activation::DrainOutcome::Busy) {
                if (++busy < cfg_.busy_spin_limit) {
                    detail::cpu_relax();
                    continue;  // mailbox producer mid-publish — bounded spin, keep Running
                }
                // still Busy after the bounded spin — yield the lane, re-enqueue (never unbounded).
                act->yield_to_scheduled();
                shards_[sid].run_queue.enqueue(s);
                return;
            }
            if (o == Activation::DrainOutcome::BudgetExhausted) {
                act->yield_to_scheduled();          // Running→Scheduled (002 §Fairness)
                shards_[sid].run_queue.enqueue(s);  // re-enqueue behind others; no wake (self awake)
                return;
            }
            return;  // Suspended — the activation parked itself; 015 re-admits via complete_parked()
        }
    }

    // Deschedule this worker until a targeted wake. The {idle_mask, run-queue tail} Dekker (announce
    // idle → seq_cst barrier → rescan) is what makes a wakeup impossible to lose: either a producer's
    // wake_one() observes our idle bit, or our rescan observes its enqueue.
    void park(std::uint32_t wid) noexcept {
        Worker& self = workers_[wid];
        const std::uint32_t seq = self.wake_seq.load(std::memory_order_acquire);
        idle_mask_.fetch_or(1u << wid, std::memory_order_acq_rel);  // announce idle
#if defined(QUARK_SCHED_BROKEN_WAKEUP)
        // CONTROL (no-lost-wakeup teeth): skip the Dekker barrier + rescan. A producer that enqueues
        // between our full-scan and our sleep is then never observed → the activation is STRANDED.
#else
        pal::store_load_barrier();  // worker half of the wake Dekker (announce-idle store ↔ tail load)
        if (any_work() || stopping_.load(std::memory_order_acquire)) {
            idle_mask_.fetch_and(~(1u << wid), std::memory_order_acq_rel);  // retract — go run it
            return;
        }
#endif
        self.wake_seq.wait(seq, std::memory_order_acquire);  // sleep until wake_one()/wake_all() bumps
        idle_mask_.fetch_and(~(1u << wid), std::memory_order_relaxed);  // clear (waker may already have)
    }

    EngineConfig cfg_;                 // FROZEN-CORE (013/ADR-008) — immutable after construction
    // 008/ADR-028 Phase 3: TypeKey → {ActorMetadata, factory} registry, seeded from the FROZEN
    // `cfg_.validation`/`cfg_.max_types`. Populated by `declare_lazy<A>()`; not yet consumed by
    // `resolve()`/`spawn<A>()` (Phase 4's ActivationBroker is the first real reader).
    TypeRegistry type_registry_;
    // ADR-028 Phase 4: a small dedicated pool for `Wake` control descriptors (the broker hand-off).
    // Cheap default (256 cells); grows cold on demand like any `detail::MessagePool` if exhausted —
    // broker traffic is inherently low (once per `ActorId`'s true first touch, never per-message).
    detail::MessagePool broker_pool_{256};
    HotCell hot_;                      // HOT-LEAF — the Live operational word (0-RMW read, live store)
    bool shard_pow2_ = false;
    std::uint64_t shard_mask_ = 0;
    std::unique_ptr<Shard[]> shards_;
    std::unique_ptr<Worker[]> workers_;
    MetricsRegistry metrics_;          // 009: aggregates every Shard's embedded ShardCounters
    std::vector<std::unique_ptr<Schedulable>> registry_;  // engine owns Schedulable lifetimes
    // 008 typed-spawn ownership: the engine owns actor instances + their Activations. Declared AFTER
    // registry_ so they outlive nothing that outlives them; all are torn down in ~Engine AFTER stop()
    // has joined every worker (no lane can touch them during destruction).
    std::vector<std::shared_ptr<void>> owned_actors_;        // type-erased actor instances (008 factory)
    std::vector<std::unique_ptr<Activation>> owned_activations_;
    std::unordered_map<ActorId, Schedulable*> by_id_;     // 006 addressing: ActorId → Schedulable
    std::vector<std::jthread> threads_;
    std::jthread backstop_;  // 011/ADR-028 Phase 2: the engine-wide idle-shard wheel-advance backstop
    std::atomic<std::uint32_t> idle_mask_{0};  // bit w set ⇒ worker w is parked/parking
    std::atomic<bool> stopping_{false};
    bool running_ = false;
};

}  // namespace quark
