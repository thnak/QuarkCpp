// Implements 025-Placement-Policies-and-Stateless-Workers §Part A — the std-only, typed, STATIC node
// capability model + the capability-annotated membership view. Design pinned by ADR-013 (the accepted
// `Weighted` strategy consumes the `weight` scalar declared here).
//
// A node advertises a set of STATIC, typed capabilities at startup (013 config): boolean `Flag`s,
// string `Label`s, and numeric `Scalar`s (notably `weight` = relative capacity). Capabilities are
// static for a node's lifetime — a change is a rejoin (021), never a live mutation — which is exactly
// what lets eligibility/weight be a PURE FUNCTION of the gossiped state: every node computes the same
// `{NodeId → capabilities}` map and therefore the same placement, with no coordinator (same property
// that makes HRW coordinator-free, placement.hpp).
//
// WHAT THIS FILE IS (025 Part A std-only core):
//   * Flag / Label / Scalar   — the advertised capability VALUE types (the 013 node-config vocabulary).
//   * NodeCapabilities         — a node's capability set + queries (`has_flag`, `label`, `weight`).
//   * CapabilityPredicate      — the compile-time constraint contract a modifier's `Cap` satisfies
//                                (`static bool eligible(const NodeCapabilities&)`); `HasFlag<Name>` /
//                                `HasLabel<Key,Val>` are the ready-made predicates a developer aliases
//                                (e.g. `using Gpu = HasFlag<"gpu">;`).
//   * CapabilityView           — a MembershipView (placement's node set) ANNOTATED with the per-node
//                                capability map, so `Require`/`Prefer`/`Weighted` (placement_policies.hpp)
//                                are a pure function of (ActorId, gossiped membership+capability set).
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (documented, NOT implemented here — each names the downstream owner):
//   * CAPABILITY WIRE GOSSIP — carrying the per-node capability set in the SWIM join payload and
//     disseminating it with membership  → 021 / 010. This file builds the MODEL + the LOCAL
//     eligibility/weight computation; the wire (encode caps into the join, gossip, decode into the
//     annotated view) is a `Membership`/Transport adapter behind the 010/021 seam. `CapabilityView`
//     is the object such an adapter would publish; `InProcessCapabilityView` below is the std-only
//     test double (no network), exactly as `InProcessMembership` stands in for SWIM.
//   * LIVE LOAD (queue depth / CPU) is NOT a capability — it is a 009 observability signal consumed
//     ONLY by stateless-pool routing (025 Part C, stateless_pool.hpp); it MUST NOT enter this model
//     or the deterministic hash (025 §determinism invariant).
// ============================================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"

namespace quark {

// ============================================================================================
// Advertised capability VALUE types (025 Part A / 013 node config). Plain structs — a node lists a
// mix of them at startup: `{ Label{"zone","eu-west-1"}, Flag{"gpu"}, Scalar{"weight", 2.0} }`.
// ============================================================================================
struct Flag {
    std::string name;
};
struct Label {
    std::string key;
    std::string value;
};
struct Scalar {
    std::string name;
    double value = 0.0;
};

// The canonical scalar name for relative capacity (consumed by the `Weighted` strategy, ADR-013).
inline constexpr std::string_view kWeightScalar = "weight";

// ============================================================================================
// NodeCapabilities — one node's static capability set. Built once from advertised values (variadic
// ctor accepts any mix of Flag/Label/Scalar); queried by placement predicates. std::string storage is
// fine: capabilities live on the CONTROL plane (membership/gossip), never the drain hot path.
// ============================================================================================
class NodeCapabilities {
public:
    NodeCapabilities() = default;

    // Build from a mix of advertised capability values, e.g.
    // `NodeCapabilities{Flag{"gpu"}, Label{"zone","eu"}, Scalar{"weight",2.0}}`.
    template <class... Cs>
    explicit NodeCapabilities(Cs... cs) {
        (add(std::move(cs)), ...);
    }

    NodeCapabilities& add(Flag f) {
        flags_.push_back(std::move(f.name));
        return *this;
    }
    NodeCapabilities& add(Label l) {
        labels_.emplace_back(std::move(l.key), std::move(l.value));
        return *this;
    }
    NodeCapabilities& add(Scalar s) {
        scalars_.emplace_back(std::move(s.name), s.value);
        return *this;
    }

    [[nodiscard]] bool has_flag(std::string_view name) const noexcept {
        for (const auto& f : flags_)
            if (f == name) return true;
        return false;
    }
    // The label value for `key`, or empty string_view if the label is absent.
    [[nodiscard]] std::string_view label(std::string_view key) const noexcept {
        for (const auto& [k, v] : labels_)
            if (k == key) return v;
        return {};
    }
    [[nodiscard]] bool has_label(std::string_view key) const noexcept {
        for (const auto& [k, v] : labels_)
            if (k == key) return true;
        return false;
    }
    // The scalar for `name`, or `fallback` if absent.
    [[nodiscard]] double scalar(std::string_view name, double fallback = 0.0) const noexcept {
        for (const auto& [n, val] : scalars_)
            if (n == name) return val;
        return fallback;
    }
    // Relative capacity for weighted placement (ADR-013). Default 1.0 (no `weight` scalar ⇒ unit
    // weight ⇒ uniform HRW). A non-positive advertised weight is clamped to a tiny positive so the
    // proportional log-WRH score `w / (−ln H)` stays finite and the node can still win occasionally.
    [[nodiscard]] double weight() const noexcept {
        const double w = scalar(kWeightScalar, 1.0);
        return w > 0.0 ? w : 1e-9;
    }

private:
    std::vector<std::string> flags_;
    std::vector<std::pair<std::string, std::string>> labels_;
    std::vector<std::pair<std::string, double>> scalars_;
};

// ============================================================================================
// Compile-time capability PREDICATES (the modifier `Cap` contract). A predicate is any type with a
// `static bool eligible(const NodeCapabilities&)`. `Require<Cap...>` / `Prefer<Cap...>` fold over
// these (placement_policies.hpp). Two ready-made predicates + a FixedString NTTP so a developer writes
// `using Gpu = HasFlag<"gpu">;  using SameZone = HasLabel<"zone","eu-west-1">;`.
// ============================================================================================

// A structural compile-time string usable as a non-type template parameter (C++20).
template <std::size_t N>
struct FixedString {
    char value[N]{};
    constexpr FixedString(const char (&s)[N]) noexcept {  // NOLINT(google-explicit-constructor)
        for (std::size_t i = 0; i < N; ++i) value[i] = s[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(value, N - 1);  // drop the trailing NUL
    }
};

template <class T>
concept CapabilityPredicate = requires(const NodeCapabilities& c) {
    { T::eligible(c) } -> std::same_as<bool>;
};

// Eligible iff the node advertises boolean flag `Name`.
template <FixedString Name>
struct HasFlag {
    [[nodiscard]] static bool eligible(const NodeCapabilities& c) noexcept {
        return c.has_flag(Name.view());
    }
};

// Eligible iff the node's label `Key` equals `Val`.
template <FixedString Key, FixedString Val>
struct HasLabel {
    [[nodiscard]] static bool eligible(const NodeCapabilities& c) noexcept {
        return c.label(Key.view()) == Val.view();
    }
};

// ============================================================================================
// CapabilityView — a MembershipView annotated with the per-node capability map. Placement policies
// read the node SET from the base view and the caps/weight from the annotation; both are pure
// functions of the gossiped CONTENT, so every node computes the same eligibility/weight regardless of
// the order the roster or caps were assembled (determinism invariant, 025).
//
// The capability map is pinned by shared_ptr alongside the MembershipView's pinned node vector, so a
// concurrent membership/capability republish never invalidates a view already handed out (UAF-free,
// TSan-clean) — same discipline as MembershipView.
// ============================================================================================
class CapabilityView {
public:
    using CapMap = std::unordered_map<std::uint64_t, NodeCapabilities>;  // NodeId::value → caps

    CapabilityView() = default;
    CapabilityView(MembershipView base, std::shared_ptr<const CapMap> caps) noexcept
        : base_(std::move(base)), caps_(std::move(caps)) {}

    [[nodiscard]] std::span<const NodeId> nodes() const noexcept { return base_.nodes(); }
    [[nodiscard]] std::size_t size() const noexcept { return base_.size(); }
    [[nodiscard]] bool empty() const noexcept { return base_.empty(); }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return base_.epoch(); }
    [[nodiscard]] const MembershipView& membership() const noexcept { return base_; }

    // The capability set of `n` (empty set if the node advertised none / is unknown). Never throws.
    [[nodiscard]] const NodeCapabilities& capabilities_of(NodeId n) const noexcept {
        static const NodeCapabilities kEmpty{};
        if (!caps_) return kEmpty;
        const auto it = caps_->find(n.value);
        return it == caps_->end() ? kEmpty : it->second;
    }
    // Relative capacity of `n` for `Weighted` placement (default 1.0). Pure function of gossiped caps.
    [[nodiscard]] double weight_of(NodeId n) const noexcept { return capabilities_of(n).weight(); }

private:
    MembershipView base_;
    std::shared_ptr<const CapMap> caps_;
};

// ============================================================================================
// InProcessCapabilityView — the std-only TEST DOUBLE (no network). Builds a CapabilityView from an
// explicit `{NodeId → NodeCapabilities}` list + a MembershipView, so placement policies are testable
// without SWIM/gossip. The real capability-gossip adapter (021/010 seam) publishes the same
// CapabilityView shape; this is NOT that adapter (mirrors InProcessMembership vs SwimMembership).
// ============================================================================================
[[nodiscard]] inline CapabilityView make_capability_view(
    MembershipView base, std::initializer_list<std::pair<NodeId, NodeCapabilities>> caps) {
    auto m = std::make_shared<CapabilityView::CapMap>();
    for (const auto& [n, c] : caps) m->emplace(n.value, c);
    return CapabilityView{std::move(base), std::move(m)};
}

// Build a CapabilityView from a raw node list + caps (convenience for tests / adapters). The node
// vector is pinned and sorted ascending, matching MembershipView's invariant.
[[nodiscard]] inline CapabilityView make_capability_view(
    std::vector<NodeId> nodes, std::uint64_t epoch,
    std::vector<std::pair<NodeId, NodeCapabilities>> caps) {
    auto nv = std::make_shared<std::vector<NodeId>>(std::move(nodes));
    // MembershipView expects an ascending, deduplicated node vector for stable HRW iteration.
    std::sort(nv->begin(), nv->end(), [](NodeId a, NodeId b) { return a.value < b.value; });
    nv->erase(std::unique(nv->begin(), nv->end(),
                          [](NodeId a, NodeId b) { return a.value == b.value; }),
              nv->end());
    auto m = std::make_shared<CapabilityView::CapMap>();
    for (auto& [n, c] : caps) m->emplace(n.value, std::move(c));
    return CapabilityView{MembershipView{std::move(nv), epoch}, std::move(m)};
}

}  // namespace quark
