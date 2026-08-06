// Implements 025-Placement-Policies-and-Stateless-Workers §Part A's documented wire seam ("CAPABILITY
// WIRE GOSSIP... a Membership/Transport adapter behind the 010/021 seam") — the real, network-backed
// `CapabilityView` producer. Design pinned by ADR-045 ("Full-Snapshot Capability Piggyback on
// SwimMembership's Bounded Gossip Digest"): a node's advertised `NodeCapabilities` (capabilities.hpp)
// rides SwimMembership's EXISTING bounded piggyback-digest gossip channel (cluster.hpp
// `set_capability_gossip`, the same shape as ADR-040's revocation-fingerprint piggyback) — not a new
// side-channel. `SwimMembership` itself stays capability-ignorant: this file is the only place that
// knows how to turn a `NodeCapabilities` into bytes and back.
//
// FRESHNESS / CONFLICT RESOLUTION (ADR-045 C2/C3): a node's entry is superseded by a strictly higher
// SWIM incarnation — the SAME freshness axis SwimMembership already uses for membership itself, no
// second competing mechanism. Within an unchanged incarnation, `local_seq` (a monotonic counter owned
// by the publishing node, bumped on every accepted local republish) breaks the tie, so a node's own
// live republish is never lost to reordering. A byte-lexicographic blob compare is a last resort for a
// genuine `(node, incarnation, local_seq)` collision (a forged/duplicated entry) — never the normal
// path.
//
// THREADING: `table_` (the raw entry map) and `local_seq_` are PROTOCOL-THREAD OWNED — `pull()`/
// `merge()`/`evict_dead()` must only be called from the thread driving `SwimMembership::tick()`/
// `on_frame()`, mirroring `members_` in cluster.hpp. Cross-thread exposure is two atomics: an
// `atomic<shared_ptr<const CapMap>>` published snapshot (`view()`, safe from ANY thread — same COW
// discipline `Topic<M>`/`FanOut` already use, topic.hpp/fanout.hpp) and an
// `atomic<shared_ptr<const NodeCapabilities>> pending_local_`, the SOLE cross-thread write entry point
// (`publish_local()`), claimed into the protocol-thread-owned table by `pull()`.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"

namespace quark {

namespace detail {

// A tiny little-endian byte writer for a NodeCapabilities blob — the SAME wire discipline as cluster.
// hpp's control codec (u8/u16/u64 helpers), scoped to just this one node's advertised facts. Opaque to
// SwimMembership; only CapabilityRegistry ever calls this.
inline void put_cap_str(std::vector<std::byte>& b, std::string_view s) {
    put_u16(b, static_cast<std::uint16_t>(s.size()));
    for (char c : s) put_u8(b, static_cast<std::uint8_t>(c));
}

// Encode one node's advertised capability set into an opaque blob (flags, then labels, then scalars —
// each string length-prefixed so decode is bounds-checked exactly like the outer control codec).
[[nodiscard]] inline std::vector<std::byte> encode_node_capabilities(const NodeCapabilities& caps) {
    std::vector<std::byte> b;
    put_u16(b, static_cast<std::uint16_t>(caps.flags().size()));
    for (std::string_view f : caps.flags()) put_cap_str(b, f);
    put_u16(b, static_cast<std::uint16_t>(caps.labels().size()));
    for (const auto& [k, v] : caps.labels()) {
        put_cap_str(b, k);
        put_cap_str(b, v);
    }
    put_u16(b, static_cast<std::uint16_t>(caps.scalars().size()));
    for (const auto& [name, value] : caps.scalars()) {
        put_cap_str(b, name);
        put_u64(b, std::bit_cast<std::uint64_t>(value));
    }
    return b;
}

// Decode a blob produced by `encode_node_capabilities`. Bounds-checked (SwimByteReader): a malformed
// or truncated blob (should be unreachable past the outer control-frame decode, which already bounds
// total capability bytes — ADR-045 S3) fails closed by returning false rather than reading OOB.
[[nodiscard]] inline bool decode_node_capabilities(const std::vector<std::byte>& blob,
                                                    NodeCapabilities& out) {
    SwimByteReader r{blob.data(), blob.data() + blob.size()};
    auto read_str = [&r]() -> std::string {
        const std::uint16_t len = r.u16();
        std::string s;
        if (!r.ok) return s;
        s.resize(len);
        for (char& c : s) c = static_cast<char>(r.u8());
        return s;
    };
    out = NodeCapabilities{};
    const std::uint16_t flag_count = r.u16();
    for (std::uint16_t i = 0; i < flag_count && r.ok; ++i) out.add(Flag{read_str()});
    const std::uint16_t label_count = r.u16();
    for (std::uint16_t i = 0; i < label_count && r.ok; ++i) {
        std::string k = read_str();
        std::string v = read_str();
        if (r.ok) out.add(Label{std::move(k), std::move(v)});
    }
    const std::uint16_t scalar_count = r.u16();
    for (std::uint16_t i = 0; i < scalar_count && r.ok; ++i) {
        std::string name = read_str();
        const std::uint64_t bits = r.u64();
        if (r.ok) out.add(Scalar{std::move(name), std::bit_cast<double>(bits)});
    }
    return r.ok;
}

}  // namespace detail

// ============================================================================================
// CapabilityRegistry (ADR-045, Design 1 — Full-Snapshot Capability Piggyback). Owns the gossiped
// `{NodeId -> capability entry}` table and produces a live, network-backed `CapabilityView`. See the
// file banner for the threading contract.
// ============================================================================================
class CapabilityRegistry {
public:
    using CapMap = CapabilityView::CapMap;

    CapabilityRegistry() { publish_snapshot(); }

    // Advertise (or replace) THIS node's capability set. Safe from ANY thread — the actual encode and
    // claim into the protocol-thread-owned table happens inside `pull()`. "Last store wins" if called
    // more than once before the next `pull()` — 025's documented usage pattern is one publish at
    // startup, but a live republish is safe and deterministic (ADR-045 C3) if a caller does need one.
    void publish_local(NodeCapabilities caps) {
        pending_local_.store(std::make_shared<const NodeCapabilities>(std::move(caps)),
                              std::memory_order_release);
    }

    // PROTOCOL-THREAD ONLY. Claims a pending local publish (if any), then packs up to `max_bytes` of
    // gossip entries for one outbound control frame. Iterates with `continue`, never `break`, past an
    // entry that would not fit the remaining budget — an early large entry must never starve a later
    // small one that WOULD fit (the starvation bug ADR-045's prover caught and fixed). A node's own
    // blob that alone exceeds `max_bytes` is silently omitted from this round (never crowds out other
    // nodes' entries) rather than truncated mid-blob — matches this codec's "never UB, just drop"
    // discipline for a malformed/oversized unit.
    [[nodiscard]] std::vector<CapabilityDigestEntry> pull(NodeId self, std::uint64_t self_incarnation,
                                                           std::uint64_t max_bytes) {
        claim_local(self, self_incarnation);
        std::vector<CapabilityDigestEntry> out;
        std::uint64_t budget = max_bytes;
        for (const auto& [node_value, e] : table_) {
            const std::uint64_t cost = e.blob.size();
            if (cost > budget) continue;  // does not fit THIS round; a smaller entry after it still can
            out.push_back(CapabilityDigestEntry{NodeId{node_value}, e.incarnation, e.local_seq, e.blob});
            budget -= cost;
        }
        return out;
    }

    // PROTOCOL-THREAD ONLY. Absorb an inbound peer's piggybacked digest (incarnation/local_seq
    // precedence, see the file banner). Republishes the decoded `CapMap` at most ONCE for the whole
    // batch, not once per accepted entry, bounding protocol-thread cost per frame.
    void merge(const std::vector<CapabilityDigestEntry>& incoming) {
        bool changed = false;
        for (const CapabilityDigestEntry& e : incoming)
            if (merge_one(e)) changed = true;
        if (changed) publish_snapshot();
    }

    // PROTOCOL-THREAD ONLY. Drop a node's capability entry once its membership status is confirmed
    // Dead — wire this onto `SwimMembership::set_sweep_hook` (the same idiom ADR-040 uses for
    // revocation/rotation sweeps) from bootstrap code that also watches `status_of(node)`. Nothing
    // here enforces that wiring (a documented residual risk, ADR-045) — an unwired node silently
    // leaks dead peers' entries.
    void evict_dead(NodeId node) {
        if (table_.erase(node.value) > 0) publish_snapshot();
    }

    // A live, network-backed `CapabilityView` over `base` (typically `swim.view()`). Safe from ANY
    // thread — one atomic shared_ptr load, the same COW-snapshot discipline `MembershipView` and
    // `Topic<M>`/`FanOut`'s subscriber snapshots already use; never a dangling reference into a
    // function-local `shared_ptr` (the UAF ADR-045's prover found and this shape structurally avoids).
    [[nodiscard]] CapabilityView view(MembershipView base) const {
        return CapabilityView{std::move(base), published_.load(std::memory_order_acquire)};
    }

private:
    struct Entry {
        std::uint64_t incarnation = 0;
        std::uint32_t local_seq = 0;
        std::vector<std::byte> blob;  // encode_node_capabilities() output — opaque to SwimMembership
    };

    void claim_local(NodeId self, std::uint64_t self_incarnation) {
        std::shared_ptr<const NodeCapabilities> pending =
            pending_local_.exchange(nullptr, std::memory_order_acq_rel);
        if (!pending) return;
        std::vector<std::byte> blob = detail::encode_node_capabilities(*pending);
        if (blob.size() > kMaxDecodedCapabilityBytes) return;  // pathological local input; drop, never crash
        Entry& e = table_[self.value];
        e.incarnation = self_incarnation;
        e.local_seq = ++local_seq_;
        e.blob = std::move(blob);
        publish_snapshot();  // self capabilities visible locally immediately, not just after a round trip
    }

    [[nodiscard]] bool merge_one(const CapabilityDigestEntry& in) {
        const auto it = table_.find(in.node.value);
        if (it == table_.end()) {
            table_.emplace(in.node.value, Entry{in.incarnation, in.local_seq, in.blob});
            return true;
        }
        Entry& cur = it->second;
        bool supersedes;
        if (in.incarnation != cur.incarnation) {
            supersedes = in.incarnation > cur.incarnation;
        } else if (in.local_seq != cur.local_seq) {
            supersedes = in.local_seq > cur.local_seq;
        } else {
            supersedes = in.blob > cur.blob;  // last-resort tiebreak, genuine collisions only
        }
        if (!supersedes) return false;
        cur.incarnation = in.incarnation;
        cur.local_seq = in.local_seq;
        cur.blob = in.blob;
        return true;
    }

    // Rebuild the whole published CapMap from `table_`. Not incremental — control-plane cadence makes
    // a full rebuild per (batched) merge/claim negligible; matches `SwimMembership::republish()`'s own
    // whole-snapshot-per-change shape.
    void publish_snapshot() {
        auto m = std::make_shared<CapMap>();
        for (const auto& [node_value, e] : table_) {
            NodeCapabilities caps;
            if (detail::decode_node_capabilities(e.blob, caps)) m->emplace(node_value, std::move(caps));
            // A stored blob that fails to decode should be unreachable (both claim_local and merge_one
            // only ever store bytes this file itself encoded or that already passed the outer control
            // decode's bounds check) — omitted rather than propagated as a corrupt CapabilityView entry.
        }
        published_.store(std::move(m), std::memory_order_release);
    }

    std::unordered_map<std::uint64_t, Entry> table_;  // protocol-thread owned, mirrors SwimMembership::members_
    std::uint32_t local_seq_ = 0;
    std::atomic<std::shared_ptr<const NodeCapabilities>> pending_local_;
    std::atomic<std::shared_ptr<const CapMap>> published_;
};

}  // namespace quark
