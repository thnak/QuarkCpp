// Implements 022-Resource-Governance-and-Overload-Control §Mechanisms — the std-only, O(1),
// allocation-free governance primitives that sit at the ingress/admission boundary:
//   * token-bucket RATE LIMITING (§1) — one cheap admission decision, bursts up to the bucket;
//   * the Closed→Open→Half-Open CIRCUIT BREAKER (§3) — bounds a downstream `ask` pileup;
//   * weighted FAIR-SHARE admission (§4) — one hot key degrades ITSELF, not its shard-mates.
//
// DESIGN (spec §Self-debate): these are PER-SHARD-LOCAL and SINGLE-WRITER (mutated only by the
// worker holding the lane), so there is no hot global atomic — exactly the cross-core contention
// 002/009 designed away. Global enforcement is therefore *approximate* ("cheap and roughly right",
// spec §Self-debate), which is the right trade for overload protection. An exact cross-node limit
// is an optional coordinator adapter behind the `RateLimiter` seam (Redis/etcd), NEVER the default.
//
// CLOCK: every time-dependent method takes an explicit monotonic `now_ns` (the caller passes
// `quark::monotonic_now_ns()` / `pal::now()` in production, a fake clock in tests) — so this header
// is clock-free, deterministic under test, and free of any hidden syscall on the decision path.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "quark/core/error.hpp"

namespace quark {

// The admission verdict (022 §Rate limiting). `Delay` is the bounded-smoothing option; deep queuing
// is refused by design (spec §Load shedding: shed, don't buffer).
enum class Admit : std::uint8_t { Accept = 0, Shed = 1, Delay = 2 };

// A heavy message may consume more than one token (022 §Rate limiting — `Cost`).
struct Cost {
    std::uint32_t tokens = 1;
};

// The governance accounting key (022 §Fair sharing / §Rate limiting): the authenticated principal
// (020) at ingress, the actor-type internally. An opaque 64-bit identity the limiters account by.
struct GovernanceKey {
    std::uint64_t value = 0;
    friend constexpr bool operator==(GovernanceKey, GovernanceKey) = default;
};

// ============================================================================================
// Token bucket — the default rate-limit algorithm (022 §1). Permits bursts up to `capacity`,
// refills at `refill_per_sec`. O(1), no allocation, lazy refill on check(). Single-writer.
// ============================================================================================
class TokenBucket {
public:
    constexpr TokenBucket() noexcept = default;
    // `capacity` = burst tolerance (max tokens); `refill_per_sec` = steady admission rate.
    constexpr TokenBucket(double capacity, double refill_per_sec) noexcept
        : cap_(capacity), tokens_(capacity), refill_per_sec_(refill_per_sec) {}

    // Refill for elapsed time, then spend `cost` if affordable. `now_ns` is a monotonic instant.
    [[nodiscard]] Admit check(std::int64_t now_ns, Cost cost = {}) noexcept {
        refill(now_ns);
        const double need = static_cast<double>(cost.tokens);
        if (tokens_ + kEps >= need) {
            tokens_ -= need;
            return Admit::Accept;
        }
        return Admit::Shed;
    }

    [[nodiscard]] double available() const noexcept { return tokens_; }
    [[nodiscard]] double capacity() const noexcept { return cap_; }
    void refill_to_full(std::int64_t now_ns) noexcept {
        tokens_ = cap_;
        last_ns_ = now_ns;
        primed_ = true;
    }

private:
    // Small epsilon so an exact-integer refill (e.g. 1s → 1000 tokens computed via double) is not
    // rejected by a sub-ULP shortfall.
    static constexpr double kEps = 1e-9;

    void refill(std::int64_t now_ns) noexcept {
        if (!primed_) {  // first observation anchors the clock (now may legitimately be 0); starts full
            primed_ = true;
            last_ns_ = now_ns;
            return;
        }
        if (now_ns <= last_ns_) return;  // non-advancing / backward clock: no refill, never negative
        const double elapsed_s = static_cast<double>(now_ns - last_ns_) * 1e-9;
        tokens_ = std::min(cap_, tokens_ + elapsed_s * refill_per_sec_);
        last_ns_ = now_ns;
    }

    double cap_ = 0.0;
    double tokens_ = 0.0;
    double refill_per_sec_ = 0.0;
    std::int64_t last_ns_ = 0;
    bool primed_ = false;
};

// ============================================================================================
// Circuit breaker — per (caller, logical target) (022 §3). Closed → Open (fail fast) → Half-Open
// (a SINGLE probe) → Closed on success / Open on failure. Trips after `fail_threshold` consecutive
// failures; stays Open for `open_ns`, then admits one half-open probe. O(1), no alloc, single-writer.
// Composes with SWIM (010): a per-remote-node breaker rides the failure detector (spec §3) — that
// wiring is a 010 seam; this is the standalone target-level breaker.
// ============================================================================================
class CircuitBreaker {
public:
    enum class State : std::uint8_t { Closed = 0, Open = 1, HalfOpen = 2 };

    constexpr CircuitBreaker() noexcept = default;
    constexpr CircuitBreaker(std::uint32_t fail_threshold, std::int64_t open_ns) noexcept
        : fail_threshold_(fail_threshold == 0 ? 1 : fail_threshold), open_ns_(open_ns) {}

    // Admission at send time (022 §3). Accept while Closed; Shed while Open (until the cooldown
    // elapses, when exactly ONE half-open probe is admitted); at most one probe in flight at once.
    [[nodiscard]] Admit on_send(std::int64_t now_ns) noexcept {
        switch (state_) {
            case State::Closed:
                return Admit::Accept;
            case State::Open:
                if (now_ns - opened_at_ns_ >= open_ns_) {
                    state_ = State::HalfOpen;
                    probe_inflight_ = true;
                    return Admit::Accept;  // the single half-open probe
                }
                return Admit::Shed;  // fail fast — errc::circuit_open, no deadline wait
            case State::HalfOpen:
                if (probe_inflight_) return Admit::Shed;  // one probe at a time
                probe_inflight_ = true;
                return Admit::Accept;
        }
        return Admit::Accept;  // unreachable; keeps the compiler happy
    }

    // Feed back the outcome of an admitted send (ok = reply within deadline; !ok = failure/timeout).
    void on_result(bool ok, std::int64_t now_ns) noexcept {
        if (state_ == State::HalfOpen) {
            probe_inflight_ = false;
            if (ok) {
                state_ = State::Closed;
                consec_fail_ = 0;
            } else {
                trip(now_ns);  // still failing → re-open, restart the cooldown
            }
            return;
        }
        if (ok) {
            consec_fail_ = 0;
            return;
        }
        if (++consec_fail_ >= fail_threshold_) trip(now_ns);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return consec_fail_; }
    [[nodiscard]] bool is_open() const noexcept { return state_ == State::Open; }

private:
    void trip(std::int64_t now_ns) noexcept {
        state_ = State::Open;
        opened_at_ns_ = now_ns;
        probe_inflight_ = false;
    }

    std::uint32_t fail_threshold_ = 5;
    std::int64_t open_ns_ = 0;
    std::uint32_t consec_fail_ = 0;
    std::int64_t opened_at_ns_ = 0;
    State state_ = State::Closed;
    bool probe_inflight_ = false;
};

// ============================================================================================
// Weighted fair-share admission (022 §4). A fixed table of per-key token buckets whose rates are
// the weighted split of a total rate: key k gets `weight_k / Σweight × total_rate`. Under
// saturation a greedy key drains ITS OWN bucket and is shed while a quiet key still admits — so a
// misbehaving-but-authorized source degrades itself, not its neighbours (spec §4). Fixed capacity
// `MaxKeys` (no allocation, linear find over a small table); single-writer / per-shard-local.
// ============================================================================================
template <std::size_t MaxKeys = 16>
class FairShare {
public:
    FairShare() noexcept = default;
    // `total_rate` (tokens/sec) and `total_burst` (max in-flight burst) are split across the
    // registered keys by weight when finalize() runs.
    explicit FairShare(double total_rate, double total_burst) noexcept
        : total_rate_(total_rate), total_burst_(total_burst) {}

    // Register a key with a relative weight (COLD — startup). Returns false iff the table is full.
    bool configure(GovernanceKey key, double weight) noexcept {
        if (count_ >= MaxKeys || weight <= 0.0) return false;
        for (std::size_t i = 0; i < count_; ++i)
            if (keys_[i] == key) {  // update an existing key's weight
                weights_[i] = weight;
                return true;
            }
        keys_[count_] = key;
        weights_[count_] = weight;
        ++count_;
        return true;
    }

    // Resolve the per-key buckets from the weights (COLD — after all configure() calls).
    void finalize() noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < count_; ++i) sum += weights_[i];
        if (sum <= 0.0) return;
        for (std::size_t i = 0; i < count_; ++i) {
            const double share = weights_[i] / sum;
            buckets_[i] = TokenBucket(total_burst_ * share, total_rate_ * share);
        }
    }

    // Hot admission decision (022 §4). An unregistered key is admitted (governance is opt-in per
    // key); a registered key spends from its own weighted bucket.
    [[nodiscard]] Admit admit(GovernanceKey key, std::int64_t now_ns, Cost cost = {}) noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (keys_[i] == key) return buckets_[i].check(now_ns, cost);
        return Admit::Accept;  // unkeyed traffic is not fair-share governed
    }

    [[nodiscard]] double available(GovernanceKey key) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (keys_[i] == key) return buckets_[i].available();
        return 0.0;
    }
    [[nodiscard]] std::size_t key_count() const noexcept { return count_; }

private:
    double total_rate_ = 0.0;
    double total_burst_ = 0.0;
    std::size_t count_ = 0;
    std::array<GovernanceKey, MaxKeys> keys_{};
    std::array<double, MaxKeys> weights_{};
    std::array<TokenBucket, MaxKeys> buckets_{};
};

// ============================================================================================
// The RateLimiter seam (022 §1 / §Dependencies). A function-pointer courier (consistent with the
// codebase's DeadLetterSink/ReclaimSink seams — no virtual, no RTTI, ADR-007 hot-path rule). The
// std-only default binds a TokenBucket; an exact global limiter (Redis/etcd) is an adapter that
// supplies its own `check_fn`, NEVER linked into the single-node core.
// ============================================================================================
struct RateLimiter {
    Admit (*check_fn)(void* self, GovernanceKey key, Cost cost, std::int64_t now_ns) noexcept =
        nullptr;
    void* self = nullptr;

    // Null seam ⇒ Accept (ungoverned): governance is opt-in with a self-contained default (spec).
    [[nodiscard]] Admit check(GovernanceKey key, Cost cost, std::int64_t now_ns) const noexcept {
        return check_fn ? check_fn(self, key, cost, now_ns) : Admit::Accept;
    }
};

// Bind a single TokenBucket (key-agnostic coarse gate) as a RateLimiter (022 §1 default).
[[nodiscard]] inline RateLimiter as_rate_limiter(TokenBucket& b) noexcept {
    return RateLimiter{
        [](void* s, GovernanceKey, Cost c, std::int64_t now) noexcept {
            return static_cast<TokenBucket*>(s)->check(now, c);
        },
        &b};
}

}  // namespace quark
