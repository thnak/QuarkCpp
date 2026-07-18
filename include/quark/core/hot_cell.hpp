// Implements 013-Configuration §"Reconfig class and override scope" + ADR-008 (Frozen-Core +
// Hot-Leaf, D3 WINNER) — the LIVE operational read-set packed into ONE 8-byte-aligned atomic word.
//
// The entire per-message operational read-set (default drain budget 002, mailbox bound + overflow
// 006/022, idle timeout 011, plus overload/observability leaves shed_level/log_level) is
// pre-resolved through the precedence chain and packed into a single 8-byte word. The HOT-PATH READ
// is a single relaxed atomic load + mask (`mov + and`) — 0 cross-core RMW, no decode branch, and no
// possible tear (an 8-byte-aligned word is hardware-indivisible on x86-64; ADR-008 S2: 0 torn over
// 4.6B reads). This satisfies the 023 0-RMW operational-read gate directly.
//
// "Live reconfig" is a single relaxed STORE of a re-packed word — the store IS the publish. Because
// the whole read-set fits ONE word, there is no seqlock/version and no pointer to reclaim on the hot
// word (a field set that overflowed one word would need a seqlock or a warm-leaf indirection — see
// ADR-008 residual risk 5/6 — but the hot read would then split; we keep it to one word so the READ
// stays 0-RMW). The cell is `alignas(cache_line_size)` and padded to a full line: false sharing
// between a reconfig storm and a co-resident drain is the load-bearing fix (ADR-008 F3: packed-8B
// control = 74% drain drop; padded = within noise).
//
// x86-TSO: a relaxed load/store of a self-contained aligned scalar gives single-location coherence
// with no fence and no RMW. // TODO(arm64): the no-tear arm rests on std::atomic single-location
// coherence and is unverified under weak memory — re-gate before any ARM64 promotion (019/023).
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "quark/core/config.hpp"
#include "quark/core/error.hpp"

namespace quark {

// Mailbox overflow policy (006/022). 2 bits in the packed word. Configuration may set the bound and
// the overflow policy but MUST NOT remove FIFO ordering (013 safety-invariant rule).
enum class Overflow : std::uint8_t { Block = 0, DropOldest = 1, DropNewest = 2, Fail = 3 };

// The decoded LIVE operational read-set. These are the ONLY knobs classed Live (ADR-008): every
// other config value is Frozen-Core (BuildOnly). Ceilings are the packing ceilings — an out-of-range
// value is fail-fast `errc::validation` (never truncated, ADR-008 §Packing ceilings).
struct OperationalConfig {
    std::uint32_t drain_budget = 1024;      // 002 §Fairness — max msgs drained before yield; (0, 2^14)
    std::uint32_t mailbox_bound = 4096;     // 006/022 — mailbox capacity; [0, 2^24)
    Overflow overflow = Overflow::Block;    // 006/022 — action on a full mailbox; 2 bits
    std::uint32_t idle_ticks = 0;           // 011 — idle-deactivation coarse ticks (0 ⇒ KeepAlive); [0, 2^16)
    std::uint32_t log_level = 0;            // 009 — observability verbosity; [0, 8)
    std::uint32_t shed_level = 0;           // 022 — overload shed threshold class; [0, 32)

    friend constexpr bool operator==(const OperationalConfig&, const OperationalConfig&) = default;
};

namespace detail {
// --- Packing layout (one 64-bit word). Fields chosen so `drain_budget` lands in the low bits ⇒ its
// read is a bare `load & 0x3fff` (single mov + and, the 023 gate). Total width = 64 bits exactly. ---
inline constexpr int      kBudgetShift = 0;    inline constexpr int      kBudgetBits = 14;
inline constexpr int      kOverflowShift = 14; inline constexpr int      kOverflowBits = 2;
inline constexpr int      kBoundShift = 16;    inline constexpr int      kBoundBits = 24;
inline constexpr int      kIdleShift = 40;     inline constexpr int      kIdleBits = 16;
inline constexpr int      kLogShift = 56;      inline constexpr int      kLogBits = 3;
inline constexpr int      kShedShift = 59;     inline constexpr int      kShedBits = 5;

inline constexpr std::uint64_t low_mask(int bits) noexcept { return (std::uint64_t{1} << bits) - 1; }

inline constexpr std::uint64_t kBudgetMask   = low_mask(kBudgetBits) << kBudgetShift;
inline constexpr std::uint64_t kOverflowMask = low_mask(kOverflowBits) << kOverflowShift;
inline constexpr std::uint64_t kBoundMask    = low_mask(kBoundBits) << kBoundShift;
inline constexpr std::uint64_t kIdleMask     = low_mask(kIdleBits) << kIdleShift;
inline constexpr std::uint64_t kLogMask      = low_mask(kLogBits) << kLogShift;
inline constexpr std::uint64_t kShedMask     = low_mask(kShedBits) << kShedShift;

// Ceilings (exclusive upper bounds) — the packing widths, surfaced for validation.
inline constexpr std::uint32_t kBudgetCeil = std::uint32_t{1} << kBudgetBits;   // 16384
inline constexpr std::uint32_t kBoundCeil  = std::uint32_t{1} << kBoundBits;    // 16777216
inline constexpr std::uint32_t kIdleCeil   = std::uint32_t{1} << kIdleBits;     // 65536
inline constexpr std::uint32_t kLogCeil    = std::uint32_t{1} << kLogBits;      // 8
inline constexpr std::uint32_t kShedCeil   = std::uint32_t{1} << kShedBits;     // 32
}  // namespace detail

// Pack a validated `OperationalConfig` into one word (assumes ranges already checked — see
// `validate_operational`). Pure ALU; no atomics.
[[nodiscard]] constexpr std::uint64_t pack_operational(const OperationalConfig& c) noexcept {
    using namespace detail;
    return (static_cast<std::uint64_t>(c.drain_budget)       << kBudgetShift) |
           (static_cast<std::uint64_t>(std::uint8_t(c.overflow)) << kOverflowShift) |
           (static_cast<std::uint64_t>(c.mailbox_bound)      << kBoundShift) |
           (static_cast<std::uint64_t>(c.idle_ticks)         << kIdleShift) |
           (static_cast<std::uint64_t>(c.log_level)          << kLogShift) |
           (static_cast<std::uint64_t>(c.shed_level)         << kShedShift);
}

// Decode a packed word back to the struct (used off the hot path — reconfig re-resolution, tests).
[[nodiscard]] constexpr OperationalConfig unpack_operational(std::uint64_t w) noexcept {
    using namespace detail;
    OperationalConfig c;
    c.drain_budget  = static_cast<std::uint32_t>((w & kBudgetMask)   >> kBudgetShift);
    c.overflow      = static_cast<Overflow>((w & kOverflowMask)      >> kOverflowShift);
    c.mailbox_bound = static_cast<std::uint32_t>((w & kBoundMask)    >> kBoundShift);
    c.idle_ticks    = static_cast<std::uint32_t>((w & kIdleMask)     >> kIdleShift);
    c.log_level     = static_cast<std::uint32_t>((w & kLogMask)      >> kLogShift);
    c.shed_level    = static_cast<std::uint32_t>((w & kShedMask)     >> kShedShift);
    return c;
}

// Validate ranges fail-fast (008/013). Out-of-range ⇒ `errc::validation` — NEVER truncated
// (ADR-008 C2 / §Packing ceilings). On success returns the packed word ready to publish/seed.
[[nodiscard]] constexpr result<std::uint64_t> validate_operational(const OperationalConfig& c) noexcept {
    using namespace detail;
    if (c.drain_budget == 0)            return fail(errc::validation, "drain_budget must be > 0");
    if (c.drain_budget >= kBudgetCeil)  return fail(errc::validation, "drain_budget exceeds 2^14 ceiling");
    if (c.mailbox_bound >= kBoundCeil)  return fail(errc::validation, "mailbox_bound exceeds 2^24 ceiling");
    if (c.idle_ticks >= kIdleCeil)      return fail(errc::validation, "idle_ticks exceeds 2^16 ceiling");
    if (c.log_level >= kLogCeil)        return fail(errc::validation, "log_level exceeds [0,8)");
    if (c.shed_level >= kShedCeil)      return fail(errc::validation, "shed_level exceeds [0,32)");
    return pack_operational(c);
}

// Clamp each field into its packing ceiling — the NON-validating seed path only (the Engine ctor).
// `reconfigure()` and `ConfigBuilder::build()` never clamp: an out-of-range value there is fail-fast
// `errc::validation` (ADR-008 "never truncates"). This exists so the frozen `EngineConfig` may carry
// a large drain-budget default (the per-actor registration source of truth) while the ceiling-bounded
// Live word still gets a sensible, representable operational default.
[[nodiscard]] constexpr OperationalConfig clamp_operational(OperationalConfig c) noexcept {
    using namespace detail;
    if (c.drain_budget == 0)             c.drain_budget = 1;
    if (c.drain_budget >= kBudgetCeil)   c.drain_budget = kBudgetCeil - 1;
    if (c.mailbox_bound >= kBoundCeil)   c.mailbox_bound = kBoundCeil - 1;
    if (c.idle_ticks >= kIdleCeil)       c.idle_ticks = kIdleCeil - 1;
    if (c.log_level >= kLogCeil)         c.log_level = kLogCeil - 1;
    if (c.shed_level >= kShedCeil)       c.shed_level = kShedCeil - 1;
    return c;
}

// A sparse LIVE delta for `reconfigure()` (013 control-plane). Only Live knobs are expressible —
// Frozen-Core (BuildOnly) fields (worker/shard count, band count, execution mode, topology, type
// set) are simply NOT members here, so a live change to them is rejected AT COMPILE TIME by the type
// system (stronger than the ADR-008 runtime `ReconfigError::BuildOnly` fail-fast). Absent fields are
// left unchanged; present fields re-resolve through validation before publish.
struct OperationalDelta {
    std::optional<std::uint32_t> drain_budget{};
    std::optional<std::uint32_t> mailbox_bound{};
    std::optional<Overflow>      overflow{};
    std::optional<std::uint32_t> idle_ticks{};
    std::optional<std::uint32_t> log_level{};
    std::optional<std::uint32_t> shed_level{};

    // Fold this delta onto a base config (present fields win).
    [[nodiscard]] constexpr OperationalConfig apply_to(OperationalConfig base) const noexcept {
        if (drain_budget)  base.drain_budget  = *drain_budget;
        if (mailbox_bound) base.mailbox_bound = *mailbox_bound;
        if (overflow)      base.overflow      = *overflow;
        if (idle_ticks)    base.idle_ticks    = *idle_ticks;
        if (log_level)     base.log_level     = *log_level;
        if (shed_level)    base.shed_level    = *shed_level;
        return base;
    }
};

// The receipt returned by a successful live reconfig (013/ADR-008). `masked_count` surfaces how many
// per-instance-overridden cells a type/engine-scope delta had to fan into (ADR-008 residual risk 7);
// for the engine-wide single-cell wiring it is 0 (the per-(shard×type_index) cell array is the
// documented seam — see engine.hpp `hot_`).
struct ReconfigReceipt {
    std::uint64_t published_word = 0;  // the word now live in the HotCell
    std::uint32_t masked_count = 0;    // per-instance cells re-resolved by the fan-out sweep
};

// ============================================================================================
// HotCell — one cache-line-padded atomic operational word. Read = single relaxed load + mask
// (0 RMW, no tear); publish = single relaxed store. `alignas(cache_line_size)` + full-line padding
// is the ADR-008 F3 false-sharing fix (load-bearing under a co-resident reconfig storm).
// ============================================================================================
class alignas(::quark::cache_line_size) HotCell {
public:
    HotCell() noexcept : word_(pack_operational(OperationalConfig{})) {}
    explicit HotCell(std::uint64_t seed) noexcept : word_(seed) {}

    HotCell(const HotCell&) = delete;
    HotCell& operator=(const HotCell&) = delete;

    // --- The hot-path read: ONE relaxed load, indivisible on x86-64 (no tear), 0 RMW. ---
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint64_t raw() const noexcept {
        return word_.load(std::memory_order_relaxed);
    }

    // Operational-read accessors. `drain_budget()` is the 023 gate: `mov (%rdi),%rax ; and $0x3fff`.
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t drain_budget() const noexcept {
        return static_cast<std::uint32_t>(raw() & detail::kBudgetMask);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t mailbox_bound() const noexcept {
        return static_cast<std::uint32_t>((raw() & detail::kBoundMask) >> detail::kBoundShift);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE Overflow overflow() const noexcept {
        return static_cast<Overflow>((raw() & detail::kOverflowMask) >> detail::kOverflowShift);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t idle_ticks() const noexcept {
        return static_cast<std::uint32_t>((raw() & detail::kIdleMask) >> detail::kIdleShift);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t log_level() const noexcept {
        return static_cast<std::uint32_t>((raw() & detail::kLogMask) >> detail::kLogShift);
    }
    [[nodiscard]] QUARK_ALWAYS_INLINE std::uint32_t shed_level() const noexcept {
        return static_cast<std::uint32_t>((raw() & detail::kShedMask) >> detail::kShedShift);
    }

    // Decode the whole word (cold — control-plane re-resolution / observability).
    [[nodiscard]] OperationalConfig decode() const noexcept { return unpack_operational(raw()); }

    // --- The publish: ONE relaxed store. The store is the publish; nothing to reclaim. ---
    QUARK_ALWAYS_INLINE void publish(std::uint64_t packed) noexcept {
        word_.store(packed, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> word_;
    // Reserve the rest of the line so a co-resident object can never share it with the hot word.
    [[maybe_unused]] char pad_[::quark::cache_line_size - sizeof(std::atomic<std::uint64_t>)]{};
};

static_assert(sizeof(HotCell) == ::quark::cache_line_size, "HotCell must own a full cache line");
static_assert(alignof(HotCell) == ::quark::cache_line_size, "HotCell must be cache-line aligned");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the hot-leaf word must be lock-free (single-load read, no tear)");

}  // namespace quark
