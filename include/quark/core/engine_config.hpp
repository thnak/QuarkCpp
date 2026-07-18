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
