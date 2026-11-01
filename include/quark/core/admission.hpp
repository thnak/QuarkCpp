// Implements 020-Security §1a (application-driven admission snapshots, Draft amendment) and
// 021-Cluster-Formation §"Application-driven eviction" (Draft amendment); ADR-069. Consumer: AeroCoWorker
// RFC 002 §6e item 4, ADR-044 L6b/L6c.
//
// `AdmissionSnapshot` is an IMMUTABLE, generation-numbered map from a peer key to a tri-state verdict:
//   Admit            - traffic on every lane may flow;
//   LogExchangeOnly  - ONLY the designated control lane (FrameKind::Control) may carry traffic; the
//                      question is asked again when the next generation is published;
//   Deny             - no session, no handshake, no delivery. A key absent from the snapshot is Deny
//                      (closed world), so an empty snapshot admits nobody.
// `lookup` is a pure read of the immutable snapshot: no lock, no allocation, no call into the
// application's live state (ADR-069 claim C1).
//
// `AdmissionAuthority` holds the CURRENT snapshot behind `std::atomic<std::shared_ptr<const ...>>` (the
// repo's idiom for a cold-written, hot-read pointer: secure_transport.hpp `revoked_snapshot_`,
// membership.hpp snapshots). `publish(g)` swaps it, then synchronously runs the registered listeners
// (SecureTransport re-answers handshakes + live sessions and purges; SwimMembership schedules eviction).
// When `publish` returns, every listener has finished: the guarantees (a)-(d) of 020 §1a hold.
// Generations are strictly increasing; a stale or equal generation is refused and counted, so a replayed
// or reordered publish can never re-admit a peer a newer generation denied.
//
// IDENTITY: the snapshot is keyed by `AdmissionKey`, today an alias of `NodeId`. Q1 (issue #40) makes the
// transport identity an Ed25519 public key with NodeId derived from it; the alias is the single place to
// change (ADR-069 §Identity). Nothing here assumes the key is 64 bits except `Entry`'s storage.
//
// LIFETIME: an `AdmissionAuthority` must OUTLIVE every object that listens to it (SecureTransport::
// set_admission, SwimMembership::set_admission): they unregister in their destructors. Declare it first.
//
// `AdmissionAuthority` IS-A `NodeAuthority` (`admitted(n)` == verdict is Admit), so placement
// (`place_admitted`) and the TcpTransport relay hook (`set_relay_forwarding(const NodeAuthority&)`) consult
// the current snapshot with no further wiring; LogExchangeOnly is NOT admitted for either.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/node_authority.hpp"
#include "quark/detail/atomic_shared_ptr.hpp"

namespace quark {

using AdmissionKey = NodeId;  // ADR-069 §Identity: becomes the Ed25519 public key when Q1 lands.

enum class AdmissionVerdict : std::uint8_t { Deny = 0, Admit = 1, LogExchangeOnly = 2 };

class AdmissionSnapshot {
    struct Entry {
        std::uint64_t key;
        AdmissionVerdict verdict;
    };

public:
    class Builder {
    public:
        explicit Builder(std::uint64_t generation) noexcept : generation_(generation) {}
        Builder& admit(AdmissionKey k) { return set(k, AdmissionVerdict::Admit); }
        Builder& log_exchange_only(AdmissionKey k) { return set(k, AdmissionVerdict::LogExchangeOnly); }
        Builder& deny(AdmissionKey k) { return set(k, AdmissionVerdict::Deny); }
        [[nodiscard]] std::shared_ptr<const AdmissionSnapshot> build() {
            std::sort(entries_.begin(), entries_.end(),
                      [](const Entry& a, const Entry& b) { return a.key < b.key; });
            return std::shared_ptr<const AdmissionSnapshot>(
                new AdmissionSnapshot(generation_, std::move(entries_)));
        }

    private:
        Builder& set(AdmissionKey k, AdmissionVerdict v) {
            for (Entry& e : entries_)
                if (e.key == k.value) {
                    e.verdict = v;  // the last call for a key wins
                    return *this;
                }
            entries_.push_back(Entry{k.value, v});
            return *this;
        }
        std::uint64_t generation_;
        std::vector<Entry> entries_;
    };

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Pure read: binary search over immutable storage. Absent key == Deny.
    [[nodiscard]] AdmissionVerdict lookup(AdmissionKey k) const noexcept {
        const auto it = std::lower_bound(entries_.begin(), entries_.end(), k.value,
                                         [](const Entry& e, std::uint64_t v) { return e.key < v; });
        return it != entries_.end() && it->key == k.value ? it->verdict : AdmissionVerdict::Deny;
    }

    // The keys this snapshot lists with `v` (cold: sweeps and tests).
    [[nodiscard]] std::vector<AdmissionKey> keys_with(AdmissionVerdict v) const {
        std::vector<AdmissionKey> out;
        for (const Entry& e : entries_)
            if (e.verdict == v) out.push_back(NodeId{e.key});
        return out;
    }

private:
    AdmissionSnapshot(std::uint64_t generation, std::vector<Entry> entries) noexcept
        : generation_(generation), entries_(std::move(entries)) {}
    std::uint64_t generation_;
    std::vector<Entry> entries_;
};

class AdmissionAuthority final : public NodeAuthority {
public:
    using Listener = std::function<void(const AdmissionSnapshot&)>;

    // Starts at generation 0 with an EMPTY snapshot: nobody is admitted until the application publishes.
    AdmissionAuthority() { current_.store(AdmissionSnapshot::Builder(0).build()); }

    AdmissionAuthority(const AdmissionAuthority&) = delete;
    AdmissionAuthority& operator=(const AdmissionAuthority&) = delete;

    // Pure reads (any thread, no lock): the current immutable snapshot / one answer from it.
    [[nodiscard]] std::shared_ptr<const AdmissionSnapshot> current() const noexcept { return current_.load(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return current_.load()->generation(); }
    [[nodiscard]] AdmissionVerdict verdict(AdmissionKey k) const noexcept { return current_.load()->lookup(k); }

    // NodeAuthority compatibility (placement, relay): only Admit counts as admitted.
    [[nodiscard]] bool admitted(NodeId n) const noexcept override { return verdict(n) == AdmissionVerdict::Admit; }

    // Cold setup. A listener runs inside publish(), on the publishing thread, in registration order, after
    // the swap. It must not call publish() or add/remove_listener (refused / would self-deadlock).
    std::uint64_t add_listener(Listener l) {
        std::lock_guard<std::mutex> g(mu_);
        listeners_.push_back({++next_listener_id_, std::move(l)});
        return next_listener_id_;
    }
    // Waits for an in-progress publish() (so a listener is never running when its owner is destroyed).
    void remove_listener(std::uint64_t id) {
        std::lock_guard<std::mutex> g(mu_);
        std::erase_if(listeners_, [id](const Slot& s) { return s.id == id; });
    }

    // Publish `next` by atomic swap; returns true iff it was accepted AND every listener has completed.
    // Refused (false, counted): null, generation <= the current one, or called from inside a listener.
    [[nodiscard]] bool publish(std::shared_ptr<const AdmissionSnapshot> next) {
        if (!next || publishing_on_this_thread()) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::lock_guard<std::mutex> g(mu_);
        if (next->generation() <= current_.load()->generation()) {
            refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        current_.store(next);
        publishes_.fetch_add(1, std::memory_order_relaxed);
        struct PublishingFlag {
            PublishingFlag() { publishing_on_this_thread() = true; }
            ~PublishingFlag() { publishing_on_this_thread() = false; }
        } flag;
        for (const Slot& s : listeners_) s.fn(*next);
        return true;
    }

    [[nodiscard]] std::uint64_t publishes() const noexcept { return publishes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t publishes_refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::uint64_t id;
        Listener fn;
    };
    static bool& publishing_on_this_thread() noexcept {
        static thread_local bool flag = false;
        return flag;
    }

    // detail::AtomicSharedPtr is std::atomic<std::shared_ptr> where the library has it (ADR-071 section 3).
    detail::AtomicSharedPtr<const AdmissionSnapshot> current_;
    std::mutex mu_;
    std::vector<Slot> listeners_;
    std::uint64_t next_listener_id_ = 0;
    std::atomic<std::uint64_t> publishes_{0};
    std::atomic<std::uint64_t> refused_{0};
};

}  // namespace quark
