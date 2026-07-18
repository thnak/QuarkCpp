// Implements the core identity value types. The full ActorRef / addressing model is owned by
// 006, and TypeKey's derivation from metadata by 008 — these are the stable spine those specs
// build on. All are trivially-copyable value types (no heap, no identity object).
#pragma once

#include <cstdint>
#include <functional>

#include "quark/detail/hash.hpp"

namespace quark {

// Node identity in a cluster (010/021). Opaque, stable for a node's lifetime.
struct NodeId {
    std::uint64_t value = 0;
    friend constexpr bool operator==(NodeId, NodeId) = default;
};

// Compile-time-stable type identity (008). Produced from a type's metadata fingerprint;
// used for dispatch, wire type tags, and durable record headers (016).
struct TypeKey {
    std::uint64_t value = 0;
    friend constexpr bool operator==(TypeKey, TypeKey) = default;
};

// Actor identity: (type, instance key). Placement is a pure function of `hash()` over the
// gossiped membership (010/026) — stable ActorId -> node with no coordinator. `hash()` is the
// input 026 feeds to `splitmix64(...) & (B-1)` to pick a VirtualBin.
struct ActorId {
    TypeKey type{};
    std::uint64_t key = 0;

    [[nodiscard]] constexpr std::uint64_t hash() const noexcept {
        return detail::hash_combine(type.value, key);
    }
    friend constexpr bool operator==(const ActorId&, const ActorId&) = default;
};

}  // namespace quark

// Standard hashing so these keys drop into unordered containers off the hot path.
template <>
struct std::hash<quark::NodeId> {
    std::size_t operator()(quark::NodeId n) const noexcept { return n.value; }
};
template <>
struct std::hash<quark::TypeKey> {
    std::size_t operator()(quark::TypeKey t) const noexcept { return t.value; }
};
template <>
struct std::hash<quark::ActorId> {
    std::size_t operator()(const quark::ActorId& a) const noexcept { return a.hash(); }
};
