// Implements 018-Clocks-and-Deadlines §Rule 1 (local monotonic instants), §Rule 3 (inheritance),
// and the 007/017 expiry seam (deadline exceeded -> errc::timeout). This is the LOCAL half of the
// model: a `Deadline` is an absolute instant on the PAL monotonic clock (pal::clock, spec 019),
// measured in ns since that clock's epoch — the same representation `MessageContext::deadline_ns`
// and `Descriptor::deadline_ns` already carry (011), so this header COMPOSES with those fields and
// never edits them. The cross-node remaining-duration translation (§Rule 2/2a/4) lives in the
// companion `deadline_propagation.hpp` (the 010 transport seam).
//
// WHY MONOTONIC, NOT WALL (018): a deadline means "N ns of elapsed time", not "until a UTC
// instant". Every function here reads pal::now() (monotonic, per-node arbitrary epoch) — never
// system_clock — so an NTP step or a backward wall-clock jump cannot move a live deadline. And
// because pal::clock is now the CLOCK_BOOTTIME-class canonical clock (counts suspend — see the
// SUSPEND note at the bottom), a suspended node treats in-flight deadlines as already-expired on
// resume, with no change to any arithmetic here: it all rides pal::now().
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "pal/pal.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/error.hpp"
#include "quark/core/message_context.hpp"

namespace quark {

// ns since the pal::clock (monotonic) epoch for a time point — the canonical scalar the whole
// engine (011 wheel, 004 descriptor, 001 context) agrees a deadline is measured in.
[[nodiscard]] inline std::int64_t monotonic_ns(pal::clock::time_point tp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}
[[nodiscard]] inline std::int64_t monotonic_now_ns() noexcept { return monotonic_ns(pal::now()); }

// --- Deadline: an absolute monotonic instant (018 §Rule 1) -----------------------------------
// A trivially-copyable value type wrapping ns-since-epoch on pal::clock. `0 == none` matches the
// `MessageContext`/`Descriptor` field convention (a real steady-clock instant is never 0 — the
// epoch is boot and now() is strictly positive and monotonically increasing), so a Deadline round-
// trips through those int64 fields losslessly with no separate "has deadline" flag to keep in sync.
class Deadline {
public:
    constexpr Deadline() noexcept = default;

    // Build from an absolute monotonic instant (ns since epoch). This is what a receiver produces
    // after rebasing a wire duration, and what the 011 wheel stores.
    [[nodiscard]] static constexpr Deadline at_ns(std::int64_t ns) noexcept { return Deadline(ns); }

    // Build from a relative budget: `now + d`. Saturates rather than overflowing int64 for absurd
    // durations (a "never" deadline stays a huge-but-valid instant, never wraps negative).
    [[nodiscard]] static Deadline after(std::chrono::nanoseconds d,
                                        pal::clock::time_point now = pal::now()) noexcept {
        const std::int64_t base = monotonic_ns(now);
        const std::int64_t budget = d.count() < 0 ? 0 : d.count();
        // saturating add
        if (budget > INT64_MAX - base) return Deadline(INT64_MAX);
        std::int64_t ns = base + budget;
        if (ns == none_sentinel) ns = 1;  // keep the 0==none invariant intact for the epoch corner
        return Deadline(ns);
    }

    // No deadline at all (unbounded budget).
    [[nodiscard]] static constexpr Deadline none() noexcept { return Deadline(); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return ns_ != none_sentinel; }
    [[nodiscard]] constexpr std::int64_t ns() const noexcept { return ns_; }  // for the int64 fields

    // Elapsed-relative queries — all against the monotonic clock, never wall time.
    [[nodiscard]] bool expired(pal::clock::time_point now = pal::now()) const noexcept {
        return has_value() && ns_ <= monotonic_ns(now);
    }

    // Remaining budget = deadline - now. Unbounded (no deadline) reports nanoseconds::max(); an
    // already-passed deadline reports a NEGATIVE duration (so callers can tell "just missed" from
    // "loads of time"). Use `remaining_clamped` when a duration for the wire must be >= 0.
    [[nodiscard]] std::chrono::nanoseconds remaining(
        pal::clock::time_point now = pal::now()) const noexcept {
        if (!has_value()) return std::chrono::nanoseconds::max();
        return std::chrono::nanoseconds(ns_ - monotonic_ns(now));
    }
    [[nodiscard]] std::chrono::nanoseconds remaining_clamped(
        pal::clock::time_point now = pal::now()) const noexcept {
        const auto r = remaining(now);
        return r.count() < 0 ? std::chrono::nanoseconds::zero() : r;
    }

    friend constexpr bool operator==(Deadline, Deadline) = default;

    static constexpr std::int64_t none_sentinel = 0;

private:
    explicit constexpr Deadline(std::int64_t ns) noexcept : ns_(ns) {}
    std::int64_t ns_ = none_sentinel;
};

// --- Inheritance (018 §Rule 3) ---------------------------------------------------------------
// The earlier of two deadlines, treating `none` as +infinity. This IS the inheritance rule: a
// child call inherits the parent's remaining budget, and MAY tighten it, but MAY NOT loosen it —
// so the effective child deadline is min(parent, child_request). Because both live on the same
// node's monotonic clock, the parent's absolute instant already encodes "remaining budget minus
// elapsed"; no re-subtraction is needed locally (that only happens across a node boundary, §Rule 2).
[[nodiscard]] constexpr Deadline earlier(Deadline a, Deadline b) noexcept {
    if (!a.has_value()) return b;
    if (!b.has_value()) return a;
    return Deadline::at_ns(std::min(a.ns(), b.ns()));
}

// Derive the deadline a handler's outbound tell/ask should carry: inherit `parent`, optionally
// tightened by a handler-supplied `child_request` (default: pure inheritance). Never loosens.
[[nodiscard]] constexpr Deadline inherit_deadline(Deadline parent,
                                                  Deadline child_request = {}) noexcept {
    return earlier(parent, child_request);
}

// --- Composition with the ambient context / descriptor (004/001) — no field edits ------------
// These free functions are the ONLY coupling to `MessageContext`/`Descriptor`: they read and write
// the pre-existing `deadline_ns` int64, so 018 threads through the ambient context without owning
// or modifying it (the field is defined by 011/004).
[[nodiscard]] inline Deadline deadline_of(const MessageContext& ctx) noexcept {
    return Deadline::at_ns(ctx.deadline_ns);
}
[[nodiscard]] inline Deadline deadline_of(const Descriptor& d) noexcept {
    return Deadline::at_ns(d.deadline_ns);
}
inline void set_deadline(MessageContext& ctx, Deadline dl) noexcept { ctx.deadline_ns = dl.ns(); }
inline void set_deadline(Descriptor& d, Deadline dl) noexcept { d.deadline_ns = dl.ns(); }

// The deadline a child message spawned by the handler running under `parent_ctx` must carry: the
// parent's inherited deadline, optionally tightened. Callers stamp the result onto the outbound
// descriptor/context via `set_deadline`. This is the hook 007/017 use so a downstream call can
// never outlive the deadline that caused it.
[[nodiscard]] inline Deadline child_deadline(const MessageContext& parent_ctx,
                                             Deadline tighter = {}) noexcept {
    return inherit_deadline(deadline_of(parent_ctx), tighter);
}

// --- Expiry detection -> errc::timeout (007/017 seam) ----------------------------------------
// The arithmetic half of "deadline exceeded". WHAT happens on expiry — dead-letter, cancel the
// activation, count a `deadline_miss` (009) — is owned by 007/009/017; this supplies the detection
// and returns the canonical `errc::timeout` so those layers act on a `quark::result`.
[[nodiscard]] inline result<void> check_deadline(Deadline dl,
                                                 pal::clock::time_point now = pal::now()) noexcept {
    if (dl.expired(now)) return fail(errc::timeout, "deadline exceeded");
    return {};
}
[[nodiscard]] inline result<void> check_deadline(const MessageContext& ctx,
                                                 pal::clock::time_point now = pal::now()) noexcept {
    return check_deadline(deadline_of(ctx), now);
}

}  // namespace quark

// SUSPEND / CLOCK_BOOTTIME seam (018 §Suspend, resume; 019 PAL) -- IMPLEMENTED at the pal::clock
// seam. pal::clock is `pal::BootClock` (pal/linux_x86_64/clock.hpp) reading CLOCK_BOOTTIME, which
// (unlike steady_clock == CLOCK_MONOTONIC) COUNTS the time the machine spends suspended. So a node
// resuming from suspend sees its in-flight deadlines as already expired (the safe, stale-work
// outcome 018 requires): the stored absolute instant did not advance during the sleep, but
// pal::now() did. All deadline arithmetic above is written against pal::now() and inherited the fix
// with no edit. Off Linux (no CLOCK_BOOTTIME), BootClock falls back to CLOCK_MONOTONIC and this
// suspend guarantee is re-gated with the rest of the 019 cross-platform surface (a build #warning
// fires). The two engine sites that once minted instants via a bare steady_clock — cluster.hpp
// SWIM ping deadlines and activation.hpp's restart-window/022-shed clock — were routed through
// pal::now() so the entire engine shares this one clock domain.
