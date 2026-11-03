// Implements 021-Cluster-Formation-and-Lifecycle §"Mobile member profile (ADR-071)" — the receiver-side
// suspension lease table a SwimMembership keeps for peers that announced a graceful SUSPEND (a phone going
// to the background / Doze). Pure data structure: no clock of its own, no I/O, no allocation after setup
// beyond the bounded map; every time value is a caller-supplied injected-clock reading (018).
//
// WHY A LEASE, NOT A STATE: a suspended member is neither failed nor gone. The receiver promises "I will not
// probe, suspect or kill this member until `until_ns`", and the PROMISE EXPIRES: a member that never
// resumes (phone switched off, app killed) is removed by the ordinary Suspect -> Dead path once the lease
// lapses (ADR-071 claim C2), so a lease can never make a zombie. The receiver, not the announcer, bounds the
// lease (`max_lease_ns`), and bounds how many leases it will hold (`max_suspended`), so a hostile member
// cannot park itself forever or exhaust the table (ADR-071 claim C6).
// ADR-071 "Red-team of the merged changes (#47)" (M3): the owner clears a lease at every tombstone/eviction/Join,
// so the table needs no prune() (removed: it had no caller).
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace quark {

// Why the host application suspended the node. Informational (events/health); never changes the rules.
enum class SuspendReason : std::uint8_t {
    AppBackground = 1,  // Activity stopped / process cached
    Doze = 2,           // the device entered Doze / App Standby
    NetworkLost = 3,    // no usable network (airplane mode, no Wi-Fi, no cellular)
    Other = 4,
};

// The receiver-side policy for peers' suspension announcements.
struct MobileProfile {
    // Upper bound the RECEIVER places on any announced lease (the announcer's request is clamped to it).
    std::int64_t max_lease_ns = 15LL * 60 * 1'000'000'000;
    // Hard cap on concurrently leased members; an announcement beyond it is ignored (the member is then
    // treated as any silent member would be).
    std::uint32_t max_suspended = 64;
    // The lease a node announces for ITSELF when the app calls suspend() without an explicit one.
    std::int64_t default_lease_ns = 10LL * 60 * 1'000'000'000;
    // ADR-082 D2: lease cycling. `max_lease_ns` bounds ONE lease; a member can refute and re-announce again and again, so the
    // granted lease time per member is counted in a rolling window. Crossing `lease_alarm_fraction` of the window raises one
    // alarm (event + counter, no state change). Enforcement is opt-in: with `lease_budget_fraction` < 1 a request is clamped to
    // what is left of that fraction of the window, and at zero the member is treated by plain SWIM.
    std::int64_t lease_budget_window_ns = 24LL * 60 * 60 * 1'000'000'000;
    double lease_alarm_fraction = 0.75;
    double lease_budget_fraction = 1.0;  // 1.0 = no enforcement
};

class SuspendLeases {
public:
    struct Entry {
        std::int64_t until_ns = 0;
        std::uint64_t incarnation = 0;  // the member's incarnation at suspend; a resume bumps it
        SuspendReason reason = SuspendReason::Other;
    };

    // Grant or refresh a lease. Returns false (and changes nothing) when `lease_ns <= 0`, or the table is
    // full and `node` is not already leased. The lease is clamped to `profile.max_lease_ns`.
    // ADR-082 D2: `*alarm` is set when this grant takes the member's granted time in the budget window past the alarm fraction
    // (once per window). Only the part of a lease that EXTENDS the silence counts (a refresh inside a lease adds the difference).
    bool grant(std::uint64_t node, std::uint64_t incarnation, std::int64_t lease_ns, SuspendReason why,
               std::int64_t now_ns, const MobileProfile& profile, bool* alarm = nullptr) {
        if (alarm) *alarm = false;
        if (lease_ns <= 0) return false;
        const auto it = map_.find(node);
        if (it == map_.end() && map_.size() >= profile.max_suspended) return false;
        std::int64_t clamped = std::min(lease_ns, profile.max_lease_ns);
        const std::int64_t base = (it != map_.end() && it->second.until_ns > now_ns) ? it->second.until_ns : now_ns;
        Budget& b = budget_[node];
        if (now_ns - b.start_ns >= profile.lease_budget_window_ns) b = Budget{now_ns, 0, false};
        if (profile.lease_budget_fraction < 1.0) {  // opt-in enforcement
            const auto allowed = static_cast<std::int64_t>(static_cast<double>(profile.lease_budget_window_ns) *
                                                           profile.lease_budget_fraction);
            const std::int64_t left = allowed - b.granted_ns;
            if (left <= 0) return false;
            clamped = std::min(clamped, left + (base - now_ns));  // the already-held part of a refresh is free
        }
        const std::int64_t until = now_ns + clamped;
        if (until > base) b.granted_ns += until - base;
        ++grants_;
        if (!b.alarmed && static_cast<double>(b.granted_ns) >
                              static_cast<double>(profile.lease_budget_window_ns) * profile.lease_alarm_fraction) {
            b.alarmed = true;
            if (alarm) *alarm = true;
        }
        map_[node] = Entry{until, incarnation, why};
        return true;
    }
    [[nodiscard]] std::uint64_t grants() const noexcept { return grants_; }
    // Lease time granted to `node` in its current budget window (0 if none).
    [[nodiscard]] std::int64_t granted_in_window(std::uint64_t node) const {
        const auto it = budget_.find(node);
        return it == budget_.end() ? 0 : it->second.granted_ns;
    }

    [[nodiscard]] bool contains(std::uint64_t node) const { return map_.contains(node); }
    [[nodiscard]] const Entry* find(std::uint64_t node) const {
        const auto it = map_.find(node);
        return it == map_.end() ? nullptr : &it->second;
    }
    void clear(std::uint64_t node) { map_.erase(node); }
    // ADR-082 D2: the member spoke before its lease ran out, so the unused part was never silence: give it back to the budget.
    void refund_unused(std::uint64_t node, std::int64_t now_ns) {
        const auto it = map_.find(node);
        const auto bt = budget_.find(node);
        if (it == map_.end() || bt == budget_.end() || it->second.until_ns <= now_ns) return;
        bt->second.granted_ns = std::max<std::int64_t>(0, bt->second.granted_ns - (it->second.until_ns - now_ns));
    }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

    // Remove and return (ascending, so event order is deterministic) every lease whose time has come.
    [[nodiscard]] std::vector<std::uint64_t> expire(std::int64_t now_ns) {
        std::vector<std::uint64_t> out;
        for (const auto& [id, e] : map_)
            if (now_ns >= e.until_ns) out.push_back(id);
        std::sort(out.begin(), out.end());
        for (std::uint64_t id : out) map_.erase(id);
        return out;
    }
    // Forget budgets whose window has ended and that hold no lease (bounds the table; the owner calls it on its tick).
    void prune_budgets(std::int64_t now_ns, const MobileProfile& profile) {
        std::erase_if(budget_, [&](const auto& kv) {
            return now_ns - kv.second.start_ns >= profile.lease_budget_window_ns && !map_.contains(kv.first);
        });
    }

private:
    struct Budget {
        std::int64_t start_ns = 0;
        std::int64_t granted_ns = 0;
        bool alarmed = false;
    };
    std::unordered_map<std::uint64_t, Entry> map_;
    std::unordered_map<std::uint64_t, Budget> budget_;
    std::uint64_t grants_ = 0;
};

}  // namespace quark
