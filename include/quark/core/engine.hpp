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

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "quark/core/activation.hpp"
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

    [[nodiscard]] Schedulable* resolve(ActorId id) const noexcept {
        return resolve_fn(engine, id);
    }
    [[nodiscard]] bool post(Schedulable* s, Descriptor* d) const noexcept {
        return post_fn(engine, s, d);
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
#if defined(__x86_64__) || defined(_M_X64)
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
    explicit Engine(EngineConfig cfg) : cfg_(cfg) {
        if (cfg_.worker_count == 0) cfg_.worker_count = 1;
        if (cfg_.shard_count == 0) cfg_.shard_count = 1;
        if (cfg_.drain_budget == 0) cfg_.drain_budget = 1;
        shard_pow2_ = (cfg_.shard_count & (cfg_.shard_count - 1)) == 0;
        shard_mask_ = cfg_.shard_count - 1;
        shards_ = std::make_unique<Shard[]>(cfg_.shard_count);
        workers_ = std::make_unique<Worker[]>(cfg_.worker_count);
        // 009: register every shard's embedded ShardCounters (cold, one-time; register_shard just
        // stores a pointer) so metrics_snapshot()/metrics_prometheus() aggregate across all shards.
        for (std::uint32_t sh = 0; sh < cfg_.shard_count; ++sh) metrics_.register_shard(shards_[sh].metrics);
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
    Schedulable* register_activation(ActorId id, Activation& act, std::uint16_t band = 0,
                                     std::uint32_t budget = 0) {
        auto s = std::make_unique<Schedulable>();
        s->activation = &act;
        s->shard = shard_of(id);
        act.set_metrics(&shards_[s->shard].metrics);  // 009: wire this activation to its shard's counters
        s->band = static_cast<std::uint16_t>(band < Policy::bands ? band : Policy::bands - 1);
        // Default drain budget = the FROZEN `EngineConfig::drain_budget` (013/ADR-008): a per-actor
        // `DrainBudget<N>` still wins (budget != 0), else the engine-wide frozen default. This is
        // resolved ONCE at registration into `Schedulable::budget` (unchanged 002 semantics). The
        // ceiling-bounded LIVE operational default lives in `hot_` (see `default_drain_budget()` /
        // `reconfigure()`); it seeds from this frozen value and is what the 023-gated operational read
        // and control-plane reconfig observe. Per-actor budgets don't re-read the hot word per message
        // (they're fixed at registration), so the frozen path here can exceed the Live 2^14 ceiling.
        s->budget = budget != 0 ? budget : cfg_.drain_budget;
        Schedulable* raw = s.get();
        registry_.push_back(std::move(s));
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
        }

        const ActorId id = actor_id_of<A>(key);
        register_activation(id, *act, static_cast<std::uint16_t>(priority_band_of<A>()),
                            drain_budget_of<A>());
        owned_actors_.push_back(std::move(actor));       // engine owns instance + Activation lifetime
        owned_activations_.push_back(std::move(act));
        return id;
    }

    // --- Addressing (006) ------------------------------------------------------------------
    // Resolve a stable ActorId to its registered Schedulable. Local-only; returns nullptr for an
    // unregistered id (the remote/lazy-activation path is a seam to 010/002 §lazy activation). A
    // pure lookup — no allocation, no mutation — so it stays on the `tell`/`ask` 0-alloc hot path.
    [[nodiscard]] Schedulable* resolve(ActorId id) const noexcept {
        const auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : it->second;
    }

    // Hand out the Policy-erased courier `ActorRef`/`LocalRouter` (006) use to resolve + post.
    [[nodiscard]] PostCourier post_courier() noexcept {
        return PostCourier{this, &resolve_courier, &post_courier_fn};
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
    }

    // Clean shutdown: signal stop, wake every worker, drain everything pending, then join.
    void stop() {
        if (!running_) return;
        stopping_.store(true, std::memory_order_release);
        wake_all();
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

    static constexpr std::uint32_t kNoOwner = 0xFFFF'FFFFu;

    struct alignas(::quark::cache_line_size) Shard {
        RunQueue<Policy> run_queue;
        QUARK_CACHE_ALIGNED std::atomic<std::uint32_t> drain_owner{kNoOwner};  // null→self per session
        ShardCounters metrics;  // 009: this shard's counter block (cold-allocated with the array)
    };

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
                return;  // released to Idle — done
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
    std::atomic<std::uint32_t> idle_mask_{0};  // bit w set ⇒ worker w is parked/parking
    std::atomic<bool> stopping_{false};
    bool running_ = false;
};

}  // namespace quark
