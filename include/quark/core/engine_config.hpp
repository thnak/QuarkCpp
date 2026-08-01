// Implements 013-Configuration §"Source of truth: a programmatic struct" + ADR-008 Frozen-Core —
// the FROZEN, immutable-after-build engine configuration, its builder, and startup Validation (008).
//
// THE POLICY vs CONFIGURATION BOUNDARY (013, normative):
//   * POLICY is COMPILE-TIME (005 CRTP): per-actor BEHAVIOR — execution mode (Sequential/Reentrant),
//     placement strategy, supervision, message protocol. It lives in the actor's `Policies...` pack
//     (policies.hpp) and NEVER appears here. Configuration MAY NOT override a safety invariant: it
//     can set the DEFAULT drain budget but cannot make a Sequential actor reentrant; it can set a
//     mailbox bound but cannot remove FIFO ordering.
//   * CONFIGURATION is RUNTIME (here): OPERATIONAL values — worker/thread count, shard count, band
//     count, pool sizes, and the DEFAULT operational knobs (drain budget, mailbox bound + overflow,
//     idle timeout). "Convention over configuration": every field has a valid default.
//
// FROZEN-CORE vs HOT-LEAF (ADR-008 reconfig class):
//   * FROZEN-CORE (BuildOnly) fields live in `EngineConfig` — set once at construction, NEVER mutated
//     live. A live change is a COMPILE-TIME impossibility (no live setter; not in `OperationalDelta`).
//   * HOT-LEAF (Live) fields are the operational read-set — they SEED the engine's `HotCell`
//     (hot_cell.hpp) at build and are live-reconfigurable via `Engine::reconfigure()`.
#pragma once

#include <cstdint>
#include <string_view>

#include "quark/core/error.hpp"
#include "quark/core/hot_cell.hpp"

namespace quark {

// NUMA layout intent (013 §Policy-vs-config table — a configuration knob, FROZEN). `Auto` lets the
// PAL (019) choose; `None` disables NUMA-aware arena placement. Runtime is a 003/019 seam.
enum class Numa : std::uint8_t { None = 0, Auto = 1 };

// Validation strictness (008/013). `Strict` fails `build()` on any range/consistency error;
// `Permissive` clamps-with-report (reserved — the core builder is Strict-only today).
enum class Validation : std::uint8_t { Strict = 0, Permissive = 1 };

// Security posture (013/020). `Off` is the single-host / dev default: the plaintext dev transport is
// allowed and no security machinery is required. `Strict` is the datacenter posture: a multi-node
// cluster on the plaintext transport is a startup Validation failure (020 §2, see `validate_security`
// in security.hpp — the check lives there because it needs cluster/transport knowledge this struct
// does not carry). A trivial enum, defaulted `Off` ⇒ zero cost when security is unused.
enum class SecurityMode : std::uint8_t { Off = 0, Strict = 1 };

// ---------------------------------------------------------------------------------------------
// FROZEN-CORE configuration. The first four members preserve the historical aggregate-init order
// `EngineConfig{worker_count, shard_count, drain_budget, busy_spin_limit}` used across the tests and
// benches — new members are appended with defaults so that spelling stays valid. Everything here is
// BuildOnly: mutating it after `Engine` construction is not offered (compile-time enforcement).
// ---------------------------------------------------------------------------------------------
struct EngineConfig {
    // --- Structural (FROZEN / BuildOnly) -------------------------------------------------------
    std::uint32_t worker_count = 1;     // EXPLICIT — never hardware_concurrency (machine safety)
    std::uint32_t shard_count = 1;      // per-shard single-writer run-queue state (002 §Sharding)
    std::uint32_t drain_budget = 1024;  // engine-wide DEFAULT drain budget — SEEDS the HotCell (Live)
    unsigned busy_spin_limit = 64;      // bounded spin on non-linearizable `Busy` (never unbounded)

    // --- Structural (FROZEN / BuildOnly), added by 013 ----------------------------------------
    std::uint32_t band_count = 1;       // K priority bands (runtime mirror of the Policy's `bands`)
    std::uint32_t max_types = 256;      // pre-sized (shard × type_index) cell cap (ADR-008 add-type)
    std::uint32_t pool_capacity = 4096; // per-shard descriptor pool sizing (003) — structural
    Numa numa = Numa::Auto;             // NUMA arena placement intent (019 seam)
    Validation validation = Validation::Strict;  // startup Validation mode (008)
    SecurityMode security_mode = SecurityMode::Off;  // 013/020 posture; Off = plaintext dev default
    // Per-shard `IdleTimeout<Ms>` wheel granularity (011/ADR-028 Phase 2): the wall-clock ms one
    // wheel tick represents. BuildOnly — the packed HotCell `idle_ticks` field (013) is already a
    // full 16 bits (ADR-008 §Consequences), so the tick's real-time SCALE cannot ride the same Live
    // word and lives here instead. `idle_timeout_ms_of<A>() / idle_tick_ms` (rounded up, min 1) is
    // resolved once per `spawn<A>()` into the actor's `idle_ticks` count.
    std::uint32_t idle_tick_ms = 100;
    // 009 §"Build-time budget gate" / ADR-022: worst-case metrics grid/arena footprint ceiling. A
    // caller computes the exact footprint via `sizeof()` (metrics_cardinality.hpp's
    // `TypeMetricsGrid<Spec>::footprint_bytes` / `InstanceMetricsArena<Spec>::footprint_bytes`) and
    // checks it against this knob with `validate_metrics_budget` BEFORE attempting the allocation.
    // Default is generous (64 MiB) so an unconfigured engine never spuriously rejects.
    std::size_t metrics_memory_budget_bytes = 64ull * 1024 * 1024;
    // ADR-035: bounded, read-only spin BEFORE a worker commits to park()'s OS block (futex/
    // WaitOnAddress). Distinct axis from busy_spin_limit (a DIFFERENT spin bounding the mailbox/
    // run-queue's non-linearizable Busy tri-state WHILE draining, ns-order); this one bridges the
    // gap between "no work right now" and the µs-order park/wake syscall round trip, never touches
    // idle_mask_, and always falls through to the unmodified park() Dekker rendezvous. 0 disables it
    // (byte-for-byte park() path, only a predictable branch added at the call site). Never unbounded.
    std::uint32_t pre_park_spin_limit = 256;
    // ADR-036 (round 3, adaptive): CEILING for a bounded, read-only re-poll of an activation's OWN
    // mailbox before drain_step commits to a DrainedEmpty exit (Running->Idle). This is NOT the
    // spin count actually used on any given call — Activation keeps a lane-only, per-activation
    // evidence counter (linger_evidence_, 0..4) that scales the EFFECTIVE bound between 0 and this
    // ceiling, so a genuinely idle/zero-concurrency activation pays ~zero cost (evidence stays 0,
    // effective bound stays 0) while an activation under real sustained/clustered contention ramps
    // toward this ceiling. Evidence grows ONLY from a real dispatched batch (>=2 messages in one
    // drain_step call) or the linger's own re-poll finding a message — NEVER from a raw Busy status
    // (round 1 bumped on Busy too; that let 1-2 ordinary timing races saturate evidence and reproduce
    // near-full linger cost on the next idle transition — "evidence hangover", closed structurally
    // in round 3 by removing Busy from the evidence-write path entirely, see the ADR).
    //
    // Round 1 shipped this as an UNCONDITIONAL spin (every Empty exit paid the full bound) and it
    // proved a real backlog/contention win (+26.4% throughput, one measured case) — but it was never
    // run against this repo's own bench-gate targets (activation_bench/sched_bench), which are
    // strictly-sequential zero-concurrency loops: there the unconditional spin was PURE WASTE on
    // every message and regressed both benches 12-17x ([goal]->[MISS]), confirmed live on real
    // hardware. Round 3's adaptive gating fixed that regression (bench-gate parity re-confirmed,
    // within ~1-3% of pre-ADR-036 numbers) while keeping the mechanism available for real backlog.
    // Round 4 fixed the F2/F5 harnesses (the originals never let the mailbox go genuinely empty, so
    // no config could show an effect) and, with that bug and a second warm-up/process-order bias
    // both closed, RE-MEASURED the round-1 contention win for the adaptive mechanism as shipped:
    // REFUTED, not merely unreconfirmed — limit=32/256 showed no statistically distinguishable
    // reduction in activation churn or throughput improvement over limit=0, cross-validated under
    // g++ and clang++ (see decisions/ADR-036-...md's round-4 section for the full harness fix and
    // data). Default is therefore 0 (mechanism byte-for-byte disabled, matching the pre-ADR-036
    // drain_step exactly) per this project's own "proven beats claimed" bar: an unproven-benefit
    // default is not justified, even though sizeof(Activation) already carries the (unconditional)
    // +64B linger_evidence_/constants footprint regardless of this default. The mechanism remains
    // available and correct for callers who opt in with a measured need. Distinct axis from
    // pre_park_spin_limit (worker-level, any_work() across ALL
    // shards) and busy_spin_limit (bounds the Busy tri-state). Sequential drain path only
    // (drain_step_governed_seq/drain_step_reentrant unaffected). 0 disables it (byte-for-byte the
    // pre-linger drain_step). Never unbounded — the setter clamps to kLingerSpinLimitMax (4096) to
    // keep the evidence-scaled bound arithmetic overflow-free.
    std::uint32_t activation_linger_spin_limit = 0;
    // ADR-038: bounded cooperative drain-owner eviction. Under OS-thread oversubscription (worker +
    // producer thread count > hardware_concurrency()), a worker holding a shard's `drain_owner` can be
    // preempted mid-drain; every other worker's scan just sees the shard as owned and skips it (never
    // blocks), so a producer's message is stuck until the OS specifically reschedules the preempted
    // owner — an OS-reschedule-timescale stall (observed: p999 ~4x and max latency ~60x worse at 2x
    // oversubscription, bench/caf_comparison/README.md). This knob bounds how many `cpu_relax()`
    // iterations a contending worker probes the owner's per-activation progress checkpoint before
    // posting a generation-tagged eviction request; the owner honors it voluntarily at its own next
    // checkpoint (never mid-activation — see `cooperative_evict()`), never by force. 0 disables it
    // (byte-for-byte the pre-ADR-038 try_drain_shard/drain_run_queue — proven, ADR-038 F1). Default is
    // 0: ADR-038 shipped this default-off pending re-measurement of a disclosed p999 side-effect (10/10
    // trials regressed, bounded magnitude, plausibly-but-not-yet-proven attributable to the eviction
    // probe's added atomic traffic on the busy-poll path in the proving harness) on a quiet, dedicated
    // host — see decisions/ADR-038-scheduler-oversubscription-tail-latency.md before enabling.
    std::uint32_t drain_owner_steal_probe_limit = 0;
    // Bounded `cpu_relax()`-paced spin a contender waits for the owner's ack after posting an eviction
    // request, before giving up and falling back to skipping the shard (today's unchanged behavior).
    // Only consulted when `drain_owner_steal_probe_limit != 0`.
    std::uint32_t drain_owner_steal_ack_spin_limit = 128;
    // ADR-038 Round 3 (the cheaper heuristic Round 2's own residual risks named as the concrete next
    // step): the CAS-fail miss path (`try_drain_shard_with_steal`, reached every time a worker's scan
    // finds a shard owned by someone else) is far hotter than the eviction-probe itself — under real
    // idle-avoidance, Round 2 measured the unconditional probe-on-every-miss shape making p999 WORSE
    // (median +130% at P=12), not better, plausibly from the added atomic traffic on that already-hot
    // path. This knob bounds how many CONSECUTIVE misses on the SAME shard, by the SAME worker, are
    // paid at near-zero cost (two lane-local integer compares, no atomics touched) before the full
    // probe-spin/eviction-request machinery engages — a worker cycling through many different
    // momentarily-busy shards never accumulates a streak and never pays the expensive path; only a
    // worker that keeps coming back to find the SAME shard still owned (the actual stuck-owner
    // signature this mechanism exists to catch) does. Only consulted when
    // `drain_owner_steal_probe_limit != 0`; irrelevant when the mechanism is disabled (the default).
    std::uint32_t drain_owner_steal_miss_threshold = 8;

    // --- HOT-LEAF SEEDS (Live). These set the INITIAL packed HotCell word; they are the ONLY fields
    //     here that a later `reconfigure()` may change (via the HotCell, not this struct). ---------
    std::uint32_t default_mailbox_bound = 4096;         // 006/022 default capacity
    Overflow      default_overflow = Overflow::Block;    // 006/022 default overflow policy
    std::uint32_t default_idle_ticks = 0;                // 011 default idle-deactivation ticks
    std::uint32_t default_log_level = 0;                 // 009 default verbosity
    std::uint32_t default_shed_level = 0;                // 022 default shed threshold class

    // The Live operational read-set this config seeds the HotCell with.
    [[nodiscard]] constexpr OperationalConfig operational_seed() const noexcept {
        return OperationalConfig{
            .drain_budget = drain_budget,
            .mailbox_bound = default_mailbox_bound,
            .overflow = default_overflow,
            .idle_ticks = default_idle_ticks,
            .log_level = default_log_level,
            .shed_level = default_shed_level,
        };
    }
};

// Startup Validation (008/013), fail-fast → `errc::validation`. Checks structural ranges AND that the
// seeded Live read-set packs (delegates to `validate_operational` so ceilings can never drift). On
// success returns the packed seed word so the caller need not re-pack. Strict mode fails `build()`.
[[nodiscard]] constexpr result<std::uint64_t> validate_engine_config(const EngineConfig& c) noexcept {
    if (c.worker_count == 0)  return fail(errc::validation, "worker_count must be > 0");
    if (c.shard_count == 0)   return fail(errc::validation, "shard_count must be > 0");
    if (c.band_count == 0 || c.band_count > 8)
        return fail(errc::validation, "band_count must be in [1,8] (ADR-010)");
    if (c.max_types == 0)     return fail(errc::validation, "max_types must be > 0");
    if (c.idle_tick_ms == 0)  return fail(errc::validation, "idle_tick_ms must be > 0");
    return validate_operational(c.operational_seed());  // seeds the Live read-set; ceilings enforced
}

// ---------------------------------------------------------------------------------------------
// ConfigBuilder — the programmatic source of truth (013). No parser, no file-format dependency in the
// core (file/env loaders are optional adapters that *produce* an EngineConfig, outside the core).
// `build()` runs Validation and returns `result<EngineConfig>` — Strict fail-fast at load.
// ---------------------------------------------------------------------------------------------
class ConfigBuilder {
public:
    // Structural (FROZEN) setters.
    ConfigBuilder& workers(std::uint32_t n) noexcept { cfg_.worker_count = n; return *this; }
    ConfigBuilder& shards(std::uint32_t n) noexcept { cfg_.shard_count = n; return *this; }
    ConfigBuilder& bands(std::uint32_t k) noexcept { cfg_.band_count = k; return *this; }
    ConfigBuilder& max_types(std::uint32_t n) noexcept { cfg_.max_types = n; return *this; }
    ConfigBuilder& pool_capacity(std::uint32_t n) noexcept { cfg_.pool_capacity = n; return *this; }
    ConfigBuilder& numa(Numa v) noexcept { cfg_.numa = v; return *this; }
    ConfigBuilder& validation(Validation v) noexcept { cfg_.validation = v; return *this; }
    ConfigBuilder& security_mode(SecurityMode v) noexcept { cfg_.security_mode = v; return *this; }
    ConfigBuilder& busy_spin_limit(unsigned n) noexcept { cfg_.busy_spin_limit = n; return *this; }
    ConfigBuilder& pre_park_spin_limit(std::uint32_t n) noexcept { cfg_.pre_park_spin_limit = n; return *this; }
    ConfigBuilder& activation_linger_spin_limit(std::uint32_t n) noexcept {
        cfg_.activation_linger_spin_limit = n;
        return *this;
    }
    ConfigBuilder& drain_owner_steal_probe_limit(std::uint32_t n) noexcept {
        cfg_.drain_owner_steal_probe_limit = n;
        return *this;
    }
    ConfigBuilder& drain_owner_steal_ack_spin_limit(std::uint32_t n) noexcept {
        cfg_.drain_owner_steal_ack_spin_limit = n;
        return *this;
    }
    ConfigBuilder& drain_owner_steal_miss_threshold(std::uint32_t n) noexcept {
        cfg_.drain_owner_steal_miss_threshold = n;
        return *this;
    }
    ConfigBuilder& idle_tick_ms(std::uint32_t ms) noexcept { cfg_.idle_tick_ms = ms; return *this; }
    ConfigBuilder& metrics_memory_budget_bytes(std::size_t n) noexcept {
        cfg_.metrics_memory_budget_bytes = n;
        return *this;
    }

    // HOT-LEAF SEED setters (these set the INITIAL Live word).
    ConfigBuilder& default_drain_budget(std::uint32_t n) noexcept { cfg_.drain_budget = n; return *this; }
    ConfigBuilder& default_mailbox_bound(std::uint32_t bound, Overflow ov = Overflow::Block) noexcept {
        cfg_.default_mailbox_bound = bound;
        cfg_.default_overflow = ov;
        return *this;
    }
    ConfigBuilder& default_idle_ticks(std::uint32_t t) noexcept { cfg_.default_idle_ticks = t; return *this; }
    ConfigBuilder& default_log_level(std::uint32_t l) noexcept { cfg_.default_log_level = l; return *this; }
    ConfigBuilder& default_shed_level(std::uint32_t s) noexcept { cfg_.default_shed_level = s; return *this; }

    // The raw (unvalidated) config — for callers that want the Engine ctor's clamping path.
    [[nodiscard]] const EngineConfig& raw() const noexcept { return cfg_; }

    // Validate + build. Strict mode fails `build()` on any range/consistency error (008).
    [[nodiscard]] result<EngineConfig> build() const noexcept {
        auto packed = validate_engine_config(cfg_);
        if (!packed) return std::unexpected<error>(packed.error());
        return cfg_;
    }

private:
    EngineConfig cfg_{};
};

}  // namespace quark
