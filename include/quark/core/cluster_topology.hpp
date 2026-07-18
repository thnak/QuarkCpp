// Implements 026-Large-Scale-Cluster-Topology (the std-only core), design pinned by
// ADR-006 (D3 — VirtualBins + Bounded Partial-View + DHT-Relay) and the FIFO-under-relay
// Hard gate proven by ADR-011 (gate-026-fifo-under-variable-hop-relay → CORRECT).
//
// This header adds the LARGE-SCALE topology machinery ALONGSIDE the settled 010 seams
// (placement.hpp `place()`, membership.hpp `Membership`, cluster.hpp `roster_digest`) —
// it does NOT touch the 010 hot path. `Flat / FullMesh / Direct` instantiates NONE of the
// bins / partial-view / relay types (026 "zero-cost when unused"): a small cluster pays
// nothing and `DistributedRouter` (distribution.hpp) stays byte-for-byte unchanged. The
// large-scale path is a compile+config selection, never a runtime branch on the drain path.
//
// WHAT THIS FILE IS (026 std-only core):
//   * VirtualBins        — O(1), N-independent placement cache. `bin = splitmix64(id.hash())&(B-1)`,
//                          `owner = bin_table[bin]` (one load). `B = next_pow2(16*max_nodes)`.
//                          The table is a FAITHFUL cache of per-bin HRW (place_hash on the bin's
//                          representative key), refilled once per epoch off the drain path. Its
//                          identity is the roster CONTENT-digest (cluster.hpp `roster_digest`) —
//                          every node with the same membership content builds a byte-identical table.
//   * VirtualBinsCache   — the epoch-swap holder: a single membership thread builds a new table on a
//                          digest change and publishes a `shared_ptr` snapshot; readers get a pinned
//                          snapshot (UAF-free, TSan-clean), exactly like SwimMembership::republish.
//   * KademliaTable /    — maintained XOR-distance routing (k-buckets), SEPARATE from the gossip
//     RelayOverlay         view, greedy next-hop toward an owner within ≤ceil(log2 N) hops (with
//                          empty-bucket fallback). RELAY ROUTING (next-hop + hop bound) only.
//   * BoundedPartialView — active view (~c*log(max_nodes) peers) + passive backup view, O(log N).
//   * PathPin            — the FIFO-under-relay discipline (ADR-011): a (sender,receiver) stream's
//                          relay path is a PURE FUNCTION of the roster digest, so it changes only at a
//                          digest transition, and a change is applied only at a DRAIN BOUNDARY (between
//                          messages), never mid-stream. This preserves 006 per-(sender,receiver) FIFO
//                          when a hop is promoted / a bucket repairs.
//   * ClusterTopologyConfig — the three orthogonal 013 policy axes (topology / connections /
//                          placement_cache) with the spec defaults and the `B >= 16*max_nodes` (008)
//                          validation. `Direct` selects `place()` directly and holds no bin table.
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (documented, NOT implemented here — each names the downstream owner):
//   * FULL QSBR RECLAMATION (sentinel per-worker beacons, reclaim-at-min, park/unpark reload,
//     alignas(64))  → optimization OVER the shared_ptr snapshot-swap. The snapshot swap here is the
//     honest std-only form: a superseded table is freed when the last pinned reader drops it. The
//     per-worker-beacon QSBR of ADR-006 S1 is a latency/RSS optimization layered on this seam.
//   * WEIGHTED / proportional log-WRH bins, `Require`/`Prefer`/`AntiAffinity`  → 025 (ADR-013).
//     VirtualBins here caches ONLY the unconstrained `HashById` path (uniform per-bin HRW). A
//     per-eligibility-class table or an exact per-actor-HRW bypass (AntiAffinity) is 025's.
//   * D2 PARTITIONED tier (>~10^4 nodes, `Partitioned<GroupByLabel<zone>>`, gateways)  → opt-in
//     tier, not this default core. `Topology::Partitioned` / `Conn::Gateway` are enum placeholders;
//     the group resolver + inter-group summary gossip live behind that opt-in.
//   * DATAGRAM SWIM control plane, per-datagram AEAD  → 020 / 021 opt-in.
//   * HOT-PEER PROMOTION to a REAL direct connection, real sockets, dialing, jittered reconnect
//     → 019 / 021. PathPin models the promotion's ORDERING discipline; the socket upgrade is 019/021.
//   * BULK-JOIN batched admission (one bin-table sweep for M joiners)  → the 021 admission FSM owns
//     batching; the hook here is that `VirtualBinsCache::rebuild_for` IS the single sweep (one table
//     rebuild covers M joiners at once — never a per-joiner O(B) repair).
//   * REAL SWIM / HyParView-style shuffle view repair (active/passive maintenance)  → 021. The
//     BoundedPartialView selection here is deterministic-from-roster; gossip-maintained repair is 021.
// ============================================================================================
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "quark/core/cluster.hpp"      // roster_digest — the content-digest we key the bin table on
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"   // MembershipView (pinned node vector)
#include "quark/core/placement.hpp"    // place_hash / rendezvous_weight — the O(N) HRW oracle bins cache
#include "quark/detail/hash.hpp"       // splitmix64

namespace quark {

// ============================================================================================
// Sizing helpers (008 validation): B = next_pow2(16 * max_nodes), power-of-two so `& (B-1)` masks.
// ============================================================================================

// The smallest power of two >= x (x==0 -> 1). Constexpr, 64-bit.
[[nodiscard]] constexpr std::uint64_t next_pow2(std::uint64_t x) noexcept {
    if (x <= 1) return 1;
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

// The validated VirtualBins bin count for a cluster provisioned for `max_nodes` (026: B >= 16*N).
[[nodiscard]] constexpr std::uint64_t virtual_bin_count(std::uint64_t max_nodes) noexcept {
    return next_pow2(16 * (max_nodes == 0 ? 1 : max_nodes));
}

// The representative HRW key for virtual bin `b`. Independent of any actor id: a bin's owner is the
// HRW winner for THIS key, so the bin table is a per-bin HRW cache. Well-mixed (splitmix64) so bin
// ownership is uniform across nodes and inherits HRW's minimal-disruption on a roster change.
[[nodiscard]] constexpr std::uint64_t bin_key(std::uint64_t bin) noexcept {
    return detail::splitmix64(bin ^ 0xD1B54A32D192ED03ULL);
}

// ============================================================================================
// VirtualBins (026 §"VirtualBins") — O(1) N-independent placement.
//
//   bin   = splitmix64(actor_id.hash()) & (B-1);     // one mix + one mask
//   owner = bin_table[bin];                           // one load, N-independent
//
// The bin_table maps each of B fixed virtual bins -> owning NodeId, filled ONCE at build time by
// per-bin HRW over the live node set (reusing placement.hpp `place_hash`). Two nodes with the same
// membership CONTENT (same node set) build a byte-identical table with the same `digest()`.
// ============================================================================================
class VirtualBins {
public:
    VirtualBins() = default;

    // Build the bin table for `nodes` with `bucket_count` bins (must be a power of two, >= 16*maxN).
    // Off the hot path (control-plane). Empty node set -> empty table (owner_of is then nullopt).
    VirtualBins(std::span<const NodeId> nodes, std::uint64_t bucket_count) {
        rebuild(nodes, bucket_count);
    }

    void rebuild(std::span<const NodeId> nodes, std::uint64_t bucket_count) {
        // bucket_count must be a power of two; round up defensively so `& mask_` is always valid.
        const std::uint64_t b = next_pow2(bucket_count);
        mask_ = b - 1;
        digest_ = roster_digest(nodes);
        table_.assign(static_cast<std::size_t>(b), NodeId{});
        empty_ = nodes.empty();
        if (empty_) return;
        for (std::uint64_t bin = 0; bin < b; ++bin) {
            // Per-bin HRW: the owner is the HRW winner for the bin's representative key (place_hash is
            // the exact O(N) oracle; the table caches it). Same node set -> same winner on every node.
            const std::optional<NodeId> w = place_hash(bin_key(bin), nodes);
            table_[static_cast<std::size_t>(bin)] = w ? *w : NodeId{};
        }
    }

    // O(1) placement: one mix, one mask, one load. N-INDEPENDENT (the 026 hot-path claim).
    [[nodiscard]] std::optional<NodeId> owner_of(ActorId id) const noexcept {
        if (empty_) return std::nullopt;
        return table_[static_cast<std::size_t>(detail::splitmix64(id.hash()) & mask_)];
    }

    // The virtual bin an actor lands in (exposed for the oracle-faithfulness test / observability).
    [[nodiscard]] std::uint64_t bin_of(ActorId id) const noexcept {
        return detail::splitmix64(id.hash()) & mask_;
    }
    // The owner stored for a raw bin index (the per-bin HRW winner).
    [[nodiscard]] NodeId owner_of_bin(std::uint64_t bin) const noexcept {
        return table_[static_cast<std::size_t>(bin & mask_)];
    }

    [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }         // content identity
    [[nodiscard]] std::uint64_t bucket_count() const noexcept { return mask_ + 1; }
    [[nodiscard]] bool empty() const noexcept { return empty_; }
    [[nodiscard]] std::span<const NodeId> table() const noexcept { return table_; }  // for byte-compare

private:
    std::vector<NodeId> table_;   // B entries: bin -> owning NodeId
    std::uint64_t mask_ = 0;      // B-1 (B is a power of two)
    std::uint64_t digest_ = 0;    // roster content-digest this table was built at
    bool empty_ = true;
};

// ============================================================================================
// VirtualBinsCache — the epoch-swap holder. A single membership thread calls `rebuild_for(view)` on a
// roster change; if the content-digest moved it builds a fresh VirtualBins and publishes it as a
// `shared_ptr` snapshot. Readers call `snapshot()` and hold a pinned copy — a concurrent rebuild never
// invalidates a snapshot already handed out (UAF-free, TSan-clean), exactly as SwimMembership does.
//
// This shared_ptr snapshot-swap IS the std-only honest form of reclamation. Full QSBR (per-worker
// beacons, reclaim-at-min) is a documented latency/RSS optimization SEAM layered over this — see the
// file header. `rebuild_for` is also the 021 BULK-JOIN sweep hook: one rebuild covers M joiners.
// ============================================================================================
class VirtualBinsCache {
public:
    explicit VirtualBinsCache(std::uint64_t bucket_count) : bucket_count_(next_pow2(bucket_count)) {}

    // Build (once) for `view`'s node set. Returns true iff a NEW table was published (digest changed).
    // Idempotent under an unchanged roster: same content-digest -> no rebuild, no swap (0 work).
    bool rebuild_for(const MembershipView& view) {
        const std::uint64_t d = roster_digest(view.nodes());
        {
            std::lock_guard<std::mutex> g(mu_);
            if (snapshot_ && snapshot_->digest() == d) return false;  // roster content unchanged
        }
        auto fresh = std::make_shared<const VirtualBins>(view.nodes(), bucket_count_);
        std::lock_guard<std::mutex> g(mu_);
        snapshot_ = std::move(fresh);
        ++epoch_;
        return true;
    }

    // A pinned snapshot for readers (per-send placement). Cheap: one refcount bump under the lock.
    [[nodiscard]] std::shared_ptr<const VirtualBins> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return snapshot_;
    }

    [[nodiscard]] std::uint64_t epoch() const {
        std::lock_guard<std::mutex> g(mu_);
        return epoch_;
    }
    [[nodiscard]] std::uint64_t bucket_count() const noexcept { return bucket_count_; }

private:
    std::uint64_t bucket_count_;
    mutable std::mutex mu_;
    std::shared_ptr<const VirtualBins> snapshot_;
    std::uint64_t epoch_ = 0;
};

// ============================================================================================
// KademliaTable (026 §"DHT-Relay") — a maintained XOR-distance routing table (k-buckets), kept
// SEPARATE from the gossip/membership view. Deterministic build from the known roster (the maintained
// form); greedy next-hop toward a target reduces XOR distance every hop, so delivery completes within
// ceil(log2 N) hops with empty-bucket fallback. Real dialing / hot-peer promotion is a 019/021 seam.
// ============================================================================================
[[nodiscard]] constexpr std::uint64_t xor_distance(NodeId a, NodeId b) noexcept {
    return a.value ^ b.value;
}

class KademliaTable {
public:
    KademliaTable() = default;

    // Build `self`'s k-buckets from the known roster: bucket i (0..63) holds nodes whose XOR-with-self
    // has its most-significant set bit at position i; each bucket keeps the k CLOSEST-to-self nodes
    // (the maintained k-bucket). `k==0` -> unbounded (keep all) — used to model a fully-maintained
    // table; a small `known` subset models an UNMAINTAINED view (dead-ends).
    void build(NodeId self, std::span<const NodeId> known, unsigned k) {
        self_ = self;
        for (auto& b : buckets_) b.clear();
        for (NodeId n : known) {
            if (n == self_) continue;
            const std::uint64_t d = xor_distance(self_, n);
            const int idx = 63 - std::countl_zero(d);  // d != 0 here (n != self)
            buckets_[static_cast<std::size_t>(idx)].push_back(n);
        }
        for (auto& b : buckets_) {
            std::sort(b.begin(), b.end(), [&](NodeId x, NodeId y) {
                return xor_distance(self_, x) < xor_distance(self_, y);
            });
            if (k != 0 && b.size() > k) b.resize(k);
        }
    }

    // Greedy next hop toward `target`: the known peer strictly closer to `target` than self. `target`
    // itself if it is a direct peer. nullopt = dead-end (no peer gets closer — the unmaintained case).
    [[nodiscard]] std::optional<NodeId> next_hop(NodeId target) const noexcept {
        if (target == self_) return self_;
        const std::uint64_t self_d = xor_distance(self_, target);
        bool have = false;
        NodeId best{};
        std::uint64_t best_d = 0;
        for (const auto& b : buckets_) {
            for (NodeId n : b) {
                const std::uint64_t d = xor_distance(n, target);
                if (!have || d < best_d) {
                    have = true;
                    best = n;
                    best_d = d;
                }
            }
        }
        if (have && best_d < self_d) return best;
        return std::nullopt;  // empty-bucket / no-progress fallback -> caller bounces on digest mismatch
    }

    [[nodiscard]] NodeId self() const noexcept { return self_; }
    [[nodiscard]] std::size_t known_count() const noexcept {
        std::size_t n = 0;
        for (const auto& b : buckets_) n += b.size();
        return n;
    }

private:
    NodeId self_{};
    std::array<std::vector<NodeId>, 64> buckets_{};
};

// A relay overlay: every roster node holds its own maintained KademliaTable. Pure/deterministic from
// the roster, so a route is a pure function of (sender, target, roster) — hence of the roster DIGEST.
// This is the object the FIFO path-pinning discipline pins on.
class RelayOverlay {
public:
    // `k==0` -> fully-maintained (unbounded buckets). A `known_fn` may restrict each node's known set
    // (model an unmaintained view for the control). Default: every node knows the whole roster.
    RelayOverlay(std::span<const NodeId> roster, unsigned k) : roster_(roster.begin(), roster.end()) {
        std::sort(roster_.begin(), roster_.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        for (NodeId n : roster_) {
            KademliaTable t;
            t.build(n, roster_, k);
            tables_.emplace(n.value, std::move(t));
        }
    }

    // Build with a per-node restricted known set (the unmaintained control): `known_of(node)` returns
    // the (small) subset that node knows. Dead-ends prove maintenance is what holds the hop bound.
    template <class KnownFn>
    RelayOverlay(std::span<const NodeId> roster, unsigned k, KnownFn known_of)
        : roster_(roster.begin(), roster.end()) {
        std::sort(roster_.begin(), roster_.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        for (NodeId n : roster_) {
            KademliaTable t;
            const std::vector<NodeId> kn = known_of(n);
            t.build(n, kn, k);
            tables_.emplace(n.value, std::move(t));
        }
    }

    [[nodiscard]] std::optional<NodeId> next_hop(NodeId cur, NodeId target) const {
        const auto it = tables_.find(cur.value);
        if (it == tables_.end()) return std::nullopt;
        return it->second.next_hop(target);
    }

    // Greedy route src -> dst. Returns the ordered hop list AFTER src (dst last) on success; empty on
    // dead-end / exceeding `max_hops`. Each hop strictly reduces XOR distance -> guaranteed to progress.
    [[nodiscard]] std::vector<NodeId> route(NodeId src, NodeId dst, unsigned max_hops) const {
        std::vector<NodeId> path;
        NodeId cur = src;
        for (unsigned h = 0; h < max_hops; ++h) {
            if (cur == dst) return path;
            const std::optional<NodeId> nh = next_hop(cur, dst);
            if (!nh || *nh == cur) return {};  // dead-end
            cur = *nh;
            path.push_back(cur);
            if (cur == dst) return path;
        }
        return {};  // exceeded the hop bound
    }

    [[nodiscard]] std::span<const NodeId> roster() const noexcept { return roster_; }

private:
    std::vector<NodeId> roster_;
    std::unordered_map<std::uint64_t, KademliaTable> tables_;
};

// ============================================================================================
// BoundedPartialView (026 §"Bounded Partial-View") — a node holds an ACTIVE view (~c*log(max_nodes)
// direct peers) + a larger PASSIVE backup view (no sockets). Deterministic selection from the roster
// (peers ranked by XOR distance to self) is enough for the O(log N) size bound here; gossip-maintained
// HyParView-style shuffle repair is a documented 021 seam.
// ============================================================================================
class BoundedPartialView {
public:
    // active_size == 0 -> c*ceil(log2(max_nodes)) with c=`fanout_c`. passive_size == 0 -> 6*active.
    BoundedPartialView(NodeId self, std::span<const NodeId> roster, std::uint64_t max_nodes,
                       std::size_t active_size = 0, std::size_t passive_size = 0,
                       unsigned fanout_c = 3) {
        self_ = self;
        const std::size_t bound = active_view_bound(max_nodes, fanout_c);
        const std::size_t want_active = active_size ? active_size : bound;
        const std::size_t want_passive = passive_size ? passive_size : 6 * want_active;

        std::vector<NodeId> peers;
        peers.reserve(roster.size());
        for (NodeId n : roster)
            if (!(n == self_)) peers.push_back(n);
        // Rank by XOR distance to self (deterministic, well-spread) — the maintained view keeps the
        // nearest peers active; the shuffle/repair that would rotate this is 021.
        std::sort(peers.begin(), peers.end(), [&](NodeId a, NodeId b) {
            return xor_distance(self_, a) < xor_distance(self_, b);
        });
        const std::size_t na = std::min(want_active, peers.size());
        const auto na_d = static_cast<std::ptrdiff_t>(na);
        active_.assign(peers.begin(), peers.begin() + na_d);
        const std::size_t np = std::min(want_passive, peers.size() - na);
        passive_.assign(peers.begin() + na_d, peers.begin() + na_d + static_cast<std::ptrdiff_t>(np));
    }

    // The O(log N) size bound: c * ceil(log2(max_nodes)) (at least 1).
    [[nodiscard]] static std::size_t active_view_bound(std::uint64_t max_nodes, unsigned c = 3) noexcept {
        const std::uint64_t n = max_nodes < 2 ? 2 : max_nodes;
        const int lg = 64 - std::countl_zero(n - 1);  // ceil(log2 n)
        const std::size_t bound = static_cast<std::size_t>(c) * static_cast<std::size_t>(lg);
        return bound == 0 ? 1 : bound;
    }

    [[nodiscard]] std::span<const NodeId> active() const noexcept { return active_; }
    [[nodiscard]] std::span<const NodeId> passive() const noexcept { return passive_; }
    [[nodiscard]] bool is_active(NodeId n) const noexcept {
        return std::find(active_.begin(), active_.end(), n) != active_.end();
    }

private:
    NodeId self_{};
    std::vector<NodeId> active_;
    std::vector<NodeId> passive_;
};

// ============================================================================================
// PathPin (026 §"Cross-node FIFO under relay" / ADR-011 Gate A) — the LOAD-BEARING FIFO discipline.
//
// A (sender, receiver) stream's relay path is a PURE FUNCTION of the roster digest. PathPin holds the
// path currently pinned for the stream and the digest it was pinned at. `resolve` returns the pinned
// path and applies a path change ONLY when BOTH (a) the digest actually transitioned AND (b) the caller
// is at a DRAIN BOUNDARY (the old-path in-flight of this stream has quiesced). Between messages of the
// same digest the path never moves; a pending change is deferred until the stream drains -> per-(S,R)
// FIFO (006) is preserved across a mid-stream promotion / k-bucket repair.
//
// The CONTROL (ADR-011's mandatory teeth) bypasses this pin — it recomputes and applies the new path
// mid-stream (at_drain_boundary always true / no pin) and MUST invert.
// ============================================================================================
template <class Path>
class PathPin {
public:
    // `compute(digest)` produces the path for a digest (pure). The pin defers a digest change until
    // `at_drain_boundary` — so a message routed under the pinned path keeps that path until the stream
    // has drained, at which point the new digest's path is adopted. Returns the path to route THIS
    // message on.
    template <class ComputeFn>
    const Path& resolve(std::uint64_t digest, bool at_drain_boundary, ComputeFn&& compute) {
        if (!pinned_) {
            path_ = compute(digest);
            digest_ = digest;
            pinned_ = true;
            return path_;
        }
        if (digest != digest_ && at_drain_boundary) {
            path_ = compute(digest);  // promotion applied BETWEEN messages, never mid-stream
            digest_ = digest;
        }
        return path_;  // same digest, or change deferred until the drain boundary -> path stays pinned
    }

    [[nodiscard]] bool pinned() const noexcept { return pinned_; }
    [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
    // True iff a newer digest is available but the change is being HELD until the stream drains.
    [[nodiscard]] bool change_pending(std::uint64_t current_digest) const noexcept {
        return pinned_ && current_digest != digest_;
    }

private:
    Path path_{};
    std::uint64_t digest_ = 0;
    bool pinned_ = false;
};

// ============================================================================================
// Configuration (013 / 026 §"Configuration") — the three orthogonal policy axes. `Flat/FullMesh/Direct`
// is the zero-cost small default; `PartialView/BoundedPartialView/VirtualBins` the large-N default.
// These are OPERATIONAL knobs (speed/path), never which node owns an actor (013 boundary).
// ============================================================================================
enum class Topology : std::uint8_t { Flat = 0, Partitioned = 1, PartialView = 2 };
enum class Conn : std::uint8_t { FullMesh = 0, Gateway = 1, BoundedPartialView = 2 };
enum class Cache : std::uint8_t { Direct = 0, VirtualBins = 1, Resolved = 2 };

struct ClusterTopologyConfig {
    Topology topology = Topology::Flat;             // small default
    Conn connections = Conn::FullMesh;              // small default
    Cache placement_cache = Cache::Direct;          // small default -> place() directly, no bins
    std::uint64_t max_nodes = 1;                    // sizes B = next_pow2(16*max_nodes)
    std::uint32_t active_view = 0;                  // 0 => c*log2(max_nodes)
    std::uint32_t relay_cap = 0;                    // 0 => ceil(log2 max_nodes)
    // Partitioned-only (D2 opt-in tier) — placeholders; the group resolver is a 026 D2 seam.
    std::uint32_t gateways_per_group = 0;

    // The recommended large-N default (N >= ~512): D3.
    [[nodiscard]] static ClusterTopologyConfig large_scale(std::uint64_t max_nodes) noexcept {
        return ClusterTopologyConfig{Topology::PartialView, Conn::BoundedPartialView, Cache::VirtualBins,
                                     max_nodes, 0, 0, 0};
    }

    // The bin count B for a VirtualBins config (0 for Direct — a Direct cluster instantiates no table).
    [[nodiscard]] std::uint64_t buckets() const noexcept {
        return placement_cache == Cache::VirtualBins ? virtual_bin_count(max_nodes) : 0;
    }
    [[nodiscard]] std::uint32_t effective_relay_cap() const noexcept {
        if (relay_cap) return relay_cap;
        const std::uint64_t n = max_nodes < 2 ? 2 : max_nodes;
        return static_cast<std::uint32_t>(64 - std::countl_zero(n - 1));  // ceil(log2 max_nodes)
    }
    [[nodiscard]] std::uint32_t effective_active_view() const noexcept {
        if (active_view) return active_view;
        return static_cast<std::uint32_t>(BoundedPartialView::active_view_bound(max_nodes));
    }

    // 008 validation. `B >= 16*max_nodes` for VirtualBins; zero-cost-when-unused: a Flat cluster must
    // NOT require any large-scale machinery. Returns nullopt on success, else the failing rule.
    [[nodiscard]] std::optional<std::string_view> validation_error() const noexcept {
        if (max_nodes == 0) return "max_nodes must be >= 1";
        if (placement_cache == Cache::VirtualBins) {
            if (buckets() < 16 * max_nodes) return "B must be >= 16 * max_nodes (026/008)";
        }
        // Zero-cost-when-unused: Flat topology may not silently require bins/partial-view machinery.
        if (topology == Topology::Flat &&
            (placement_cache == Cache::VirtualBins || connections == Conn::BoundedPartialView))
            return "Flat topology must instantiate no large-scale cache/connection machinery";
        return std::nullopt;
    }
    [[nodiscard]] bool valid() const noexcept { return !validation_error().has_value(); }
};

// ============================================================================================
// DirectPlacement — the zero-cost `Cache::Direct` resolver: HOLDS NOTHING (is_empty), routes straight
// through the 010 `place()` oracle. A Direct cluster constructs no VirtualBins / bin table at all.
// ============================================================================================
struct DirectPlacement {
    [[nodiscard]] static std::optional<NodeId> owner_of(ActorId id, const MembershipView& v) noexcept {
        return place(id, v);
    }
};
static_assert(std::is_empty_v<DirectPlacement>, "Direct placement must hold no state (zero-cost)");

}  // namespace quark
