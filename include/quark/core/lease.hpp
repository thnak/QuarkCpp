// Implements 028-Leases-and-Leader-Election; ADR-058 (simulation-first); composes ADR-055's
// FencingEpoch/FenceToken and the 012 Store fence.
//
// A lease is a TIME-BOUNDED, SINGLE-HOLDER claim on a key, granted by one linearizable ARBITER (in a
// deployment: the node hosting the Store, or a consensus-backed Store). Leader election for a key
// is "who currently holds the lease on it". The FENCING TOKEN of a grant is the Store's own
// `acquire_fence(key)` result, so tokens are strictly monotonic by construction (the Store is the
// single source of truth) and a deposed leader is rejected by the Store's existing `tok < owner`
// check regardless of what any clock says.
//
//   SAFETY (never depends on clocks):  only the Store's token order decides whose writes land.
//   LEASE-BELIEF (depends on bounded drift, 028 §clock assumptions): at most one holder BELIEVES it
//     is leader at a real instant; this is an optimisation that lets a leader skip a Store
//     round-trip per write, never the safety argument.
//   LIVENESS: best-effort (a leader emerges once the arbiter is reachable and a lease has expired).
//
// Pure state machines: no threads, no sockets, no clock reads. Time is a caller-supplied uint64 tick
// count from the node's OWN monotonic clock (only intervals are ever compared, never cross-node
// absolute times); messages are values handed to a caller-supplied sender. Not on any hot path.
#pragma once

#include <cstdint>
#include <optional>

#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark::lease {

using Ticks = std::uint64_t;   // a node's local monotonic clock, arbitrary origin
using Peer = std::uint32_t;    // simulation/transport-level peer index (not quark::NodeId)

struct Config {
    Ticks duration = 100;            // D: lease length as measured by the ARBITER's clock
    std::uint32_t drift_ppm_bound = 50'000;  // rho: assumed max clock-rate error vs real time, ppm (5%)
    Ticks retry = 20;                // request retransmit interval (holder's clock)
    Ticks renew_before = 40;         // start renewing when this close to local expiry

    // m >= 2*rho*D/(1+rho): the safe margin the holder subtracts so its belief ends before the
    // arbiter's expiry in REAL time (028 §clock assumptions). +2 ticks absorbs integer-tick quantisation.
    [[nodiscard]] constexpr Ticks margin() const noexcept {
        return (duration * 2ull * drift_ppm_bound) / 1'000'000ull + 2;
    }
};

struct Request {
    Peer from = 0;
    std::uint64_t id = 0;  // holder-unique, increasing; echoed in the reply
};

struct Reply {
    Peer to = 0;
    std::uint64_t id = 0;
    bool granted = false;
    FenceToken term{};     // meaningful iff granted
};

// ------------------------------------------------------------------------------------------
// LeaseArbiter: the single authority for ONE key. Holds {holder, term, expiry} in memory only; the
// durable truth is the Store's owner token, so an arbiter restart loses nothing safety-relevant
// (it merely observes a quiet period of one full duration before granting anew).
// ------------------------------------------------------------------------------------------
template <class StoreT>
class LeaseArbiter {
public:
    LeaseArbiter(StoreT& store, ActorId key, Config cfg) : store_(store), key_(key), cfg_(cfg) {}

    // Process a request at arbiter-local time `now`. Returns the reply to send (nullopt = say nothing).
    [[nodiscard]] std::optional<Reply> on_request(Ticks now, const Request& rq) {
        if (now < quiet_until_) return Reply{rq.from, rq.id, false, {}};  // post-restart quiet period
        if (holder_ && *holder_ == rq.from) {                  // renewal / re-affirm: same term, no bump
            expiry_ = now + cfg_.duration;
            return Reply{rq.from, rq.id, true, term_};
        }
        if (holder_ && now < expiry_) return Reply{rq.from, rq.id, false, {}};  // someone else holds it
        auto tok = store_.acquire_fence(key_);                 // NEW term: Store-issued, strictly greater
        if (!tok) return Reply{rq.from, rq.id, false, {}};     // Store unavailable: no grant
        holder_ = rq.from;
        term_ = *tok;
        expiry_ = now + cfg_.duration;
        return Reply{rq.from, rq.id, true, term_};
    }

    // Arbiter process restart: in-memory lease state is gone; refuse all grants for one full lease
    // duration so any pre-crash holder's belief (bounded by D from a send before the crash) lapses.
    void restart(Ticks now) {
        holder_.reset();
        quiet_until_ = now + cfg_.duration;
    }

    [[nodiscard]] FenceToken term() const noexcept { return term_; }

private:
    StoreT& store_;
    ActorId key_;
    Config cfg_;
    std::optional<Peer> holder_{};
    FenceToken term_{};
    Ticks expiry_ = 0;
    Ticks quiet_until_ = 0;
};

// ------------------------------------------------------------------------------------------
// LeaseHolder: a candidate/leader. `tick()` returns a Request when one should be (re)sent.
// `believes_leader(now)` is the lease-belief (clock-dependent); `token()` is the last token ever
// granted to us, kept even after belief lapses (a paused "zombie" may still try to write with it —
// the Store fence is what stops it).
// ------------------------------------------------------------------------------------------
class LeaseHolder {
public:
    LeaseHolder(Peer self, Config cfg) : self_(self), cfg_(cfg) {}

    [[nodiscard]] std::optional<Request> tick(Ticks now) {
        const bool valid = believes_leader(now);
        const bool need = !valid || (valid_until_ - now) <= cfg_.renew_before;
        if (!need) return std::nullopt;
        if (outstanding_ && now - sent_at_ < cfg_.retry) return std::nullopt;
        if (now < not_before_) return std::nullopt;
        outstanding_ = true;
        ++next_id_;
        cur_id_ = next_id_;
        sent_at_ = now;
        return Request{self_, cur_id_};
    }

    // Apply a reply. Belief is anchored at the SEND time of the request it answers (never the receive
    // time), so network delay can only shorten our belief, never extend it.
    void on_reply(Ticks now, const Reply& r) {
        if (!outstanding_ || r.id != cur_id_) return;  // stale/duplicate/foreign
        outstanding_ = false;
        if (!r.granted) return;
        if (r.term < token_) return;                   // cannot happen from a sound arbiter; ignore
        const Ticks until = sent_at_ + cfg_.duration - cfg_.margin();
        if (now >= until) return;                      // already expired by our own clock
        token_ = r.term;
        valid_until_ = until;
    }

    // Randomised election backoff (liveness aid only): do not request before `t`.
    void backoff_until(Ticks t) noexcept { not_before_ = t; }

    [[nodiscard]] bool believes_leader(Ticks now) const noexcept {
        return !is_unfenced(token_) && now < valid_until_;
    }
    [[nodiscard]] FenceToken token() const noexcept { return token_; }
    [[nodiscard]] Peer self() const noexcept { return self_; }

private:
    Peer self_;
    Config cfg_;
    FenceToken token_{};
    Ticks valid_until_ = 0;
    bool outstanding_ = false;
    std::uint64_t next_id_ = 0, cur_id_ = 0;
    Ticks sent_at_ = 0;
    Ticks not_before_ = 0;
};

}  // namespace quark::lease
