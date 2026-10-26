// Implements 021-Cluster-Formation-and-Lifecycle §1 (cluster identity + epoch), §2 (Discovery seam,
// dial-dedup), §3 (SWIM failure detection, incarnation refutation, gossip, stabilization / anti-flap).
// SWIM precedence, tombstones, rejoin and the stateless indirect-ping relay: ADR-048.
// This is the std-only, in-process-testable CORE — the control-plane lifecycle that sits ON the 010
// distribution seams (membership.hpp `Membership`, transport.hpp `Transport`) exactly as 010's own
// core (distribution.hpp) split "std-only core + explicit adapter seams".
//
// WHAT THIS FILE IS:
//   * `SwimMembership` — a REAL SWIM failure detector implementing the 010 `Membership` interface
//     (self() + epoch-stamped view()). It runs the SWIM protocol over the `Transport` seam: periodic
//     direct ping → indirect ping (ping-req via k relays) → suspicion → dead, with incarnation-numbered
//     refutation and gossip dissemination (no coordinator). All protocol periods read an INJECTABLE
//     `ClockFn` (mirrors Activation::set_clock, 014) and the protocol is driven by explicit `tick()`
//     calls — so a test advances VIRTUAL time and ticks to trigger every transition deterministically,
//     with NO std::this_thread::sleep_for and NO real wall-clock wait anywhere.
//   * Cluster identity — a `ClusterId` carried in every control frame + a monotonic membership `epoch`
//     that advances on each admitted roster change (join / leave / dead). A join presenting a mismatched
//     cluster id is rejected (§1 "preventing accidental merges").
//   * `Discovery` seam (§2) + `SeedListDiscovery` static-seed-list default + a minimal `Endpoint`.
//   * `dial_winner` / `keep_local_dial` — the deterministic lower-NodeId-wins dial-dedup RULE (§2), a
//     pure function both ends compute identically.
//   * `StabilizationWindow` (§3 anti-flap) — a membership change must hold for a configurable settle
//     interval before it drives re-placement; a flap within the window causes NO reshuffle. Pure +
//     virtual-clock driven, separately testable.
//   * `control_data_demux` — composes a 010 data sink and a 021 control sink over ONE Transport
//     endpoint, keyed on `MessageFrame::kind`, so the two planes share a transport without the data
//     path changing (its stream is `Data`-only, byte-for-byte the 010 path).
//   * `SwimMembership::set_capability_gossip` (ADR-045) — a bounded piggyback of gossiped
//     `CapabilityDigestEntry` values riding the SAME frame as `updates`/ADR-040 `revocations`, not a
//     new side-channel. SwimMembership stays capability-ignorant (opaque bytes only); the real
//     025-static-capability model, encode/decode, and the live network-backed `CapabilityView`
//     producer are `CapabilityRegistry` (capability_registry.hpp).
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (documented, NOT implemented here — they depend on specs not yet distribution-
// wired; each names the downstream owner):
//   * AUTHENTICATE / crypto handshake / trust root / SecretSource  → 020 (`NodeAuthority`,
//     `SecureTransport`). The join FSM's AUTHENTICATE step is a 020 seam; here it is an in-process
//     no-op acceptor (the cluster-id check rides where the 020 handshake will). `admit()` is the join
//     gate; the crypto proof-of-identity that must precede it is 020's, not built here.
//   * Hand-off / fencing-token reload / graceful drain of live actors  → 012 / 015 / 017. A planned
//     move invokes 017's fencing + 015's quiesce(Drain); this file adds nothing to that guarantee and
//     builds no cross-node actor hand-off. `StabilizationWindow` only decides WHEN re-placement fires.
//   * Real sockets, lazy dial mechanics, jittered reconnect backoff, keepalive-reuses-SWIM-RTT wiring
//     → 019 PAL. The loopback transport (transport.hpp) is the test double; `dial_winner` is the RULE,
//     there being no real socket here to race.
//   * VirtualBins bulk-join batching (one bin-table sweep for M joiners)  → 026. Uniform HRW re-place
//     (placement.hpp) is the honest small-cluster form used here.
//   * DNS / K8s / cloud Discovery adapters, and a linearizable (Raft/etcd) membership store  → optional
//     adapters behind `Discovery` / `Membership`, never linked into a default build (§Dependencies).
//   * The per-transport-endpoint one-way "link filter" (`set_link_reachable`) is a TEST seam standing
//     in for a real network partition (a partial partition that exercises indirect-ping); a real
//     partition is observed by the 019 PAL socket layer, not injected here.
// ============================================================================================
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/tls_identity.hpp"  // Fingerprint — ADR-040 Phase 5 revocation-gossip piggyback
#include "quark/core/transport.hpp"
#include "quark/detail/hash.hpp"
#include "quark/detail/rate_bucket.hpp"
#include "pal/pal.hpp"  // pal::now() — the canonical suspend-counting clock (018/019)

namespace quark {

// ClusterId (021 §1) now lives in ids.hpp — the mTLS handshake seam (handshake.hpp, ADR-040) binds it
// from a verified leaf certificate and needs it without pulling in this whole membership header.

// A contact point where a peer MIGHT be reached (021 §2 Discovery). Minimal value type: the NodeId is
// the stable identity; `addr`/`port` are an opaque, PAL-interpreted locator (a real Endpoint carries a
// resolved socket address — that resolution is 019, not here). Seeds are contact points, not
// coordinators: once joined a node has no standing dependency on them.
struct Endpoint {
    NodeId node{};
    std::uint64_t addr = 0;  // opaque locator (019 interprets); 0 = unspecified
    std::uint16_t port = 0;
    friend constexpr bool operator==(const Endpoint&, const Endpoint&) = default;
};

// ============================================================================================
// The Discovery seam (021 §2) — where might peers be, for a fresh node that knows no one. EXACTLY the
// spec's snippet. Default adapter below is a static seed list (std-only, works everywhere). DNS SRV /
// K8s endpoints / cloud tags / multicast are environment-specific adapters BEHIND this seam.
// ============================================================================================
struct Discovery {
    virtual ~Discovery() = default;
    virtual std::vector<Endpoint> contacts() = 0;  // where might peers be?
};

// The default Discovery: a static seed list from config (013 `cluster.seeds`). A joiner reaches ANY
// ONE live seed to pull the snapshot, then SWIM takes over.
class SeedListDiscovery final : public Discovery {
public:
    SeedListDiscovery() = default;
    explicit SeedListDiscovery(std::vector<Endpoint> seeds) : seeds_(std::move(seeds)) {}
    [[nodiscard]] std::vector<Endpoint> contacts() override { return seeds_; }
    void add(Endpoint e) { seeds_.push_back(e); }

private:
    std::vector<Endpoint> seeds_;
};

// ============================================================================================
// Dial deduplication RULE (021 §2 "the concurrency hazard"). When two connections form between the
// same pair (both ends dialed simultaneously), the one INITIATED BY THE LOWER NodeId is kept; the
// other is closed. Both ends compute the same winner from ids alone — no negotiation round-trip. Pure;
// this is the whole rule (real socket teardown is 019, there is no socket here to race).
// ============================================================================================

// The NodeId whose *initiated* connection survives between `a` and `b` (the lower id).
[[nodiscard]] constexpr NodeId dial_winner(NodeId a, NodeId b) noexcept {
    return a.value <= b.value ? a : b;
}

// From `self`'s vantage: keep the connection I initiated to `peer` (true) or keep the one the peer
// initiated to me (false)? True iff I am the lower id. `self != peer` by construction (no self-dial).
[[nodiscard]] constexpr bool keep_local_dial(NodeId self, NodeId peer) noexcept {
    return self.value < peer.value;
}

// ============================================================================================
// SWIM control-message model (021 §3). These are CONTROL messages, not actor messages — they ride a
// `FrameKind::Control` frame with their own std-only little-endian codec (below), never 016. Every
// frame carries the sender's own current incarnation (`from` / `from_incarnation`) so a recipient
// always has the sender's latest self-view (the substrate for refutation), plus a bounded piggybacked
// gossip digest (`updates`).
// ============================================================================================
// Dead and Left are TOMBSTONES (ADR-048): kept for `tombstone_ttl_ns` so stale gossip cannot resurrect
// the node, then erased. Dead is adopted only first-hand (this node's own suspicion timer); Left is an
// authoritative graceful departure (leave()), lifted only by the subject's own Join.
enum class MemberStatus : std::uint8_t { Alive = 0, Suspect = 1, Dead = 2, Left = 3 };

enum class ControlKind : std::uint8_t {
    Ping = 1,        // direct probe: are you alive?
    Ack = 2,         // ping reply (carries the acker's incarnation ⇒ can refute)
    PingReq = 3,     // indirect probe: relay, please ping `subject` for me
    PingReqAck = 4,  // relay's report that `subject` acked (carries subject's incarnation)
    Gossip = 5,      // standalone dissemination of the member digest
    Join = 6,        // joiner → seed: admit me (carries my ClusterId to validate)
    JoinAck = 7,     // seed → joiner: admitted; snapshot rides the `updates` digest
    JoinReject = 8,  // seed → joiner: cluster-id mismatch, refused (§1 accidental-merge guard)
    Congested = 9,   // ADR-046: edge-triggered cross-node mailbox backpressure signal
};

// One gossiped member fact: node, incarnation, status. Higher incarnation wins on merge; a node
// refutes a false Suspect/Dead of ITSELF by re-broadcasting Alive at a higher incarnation.
struct MemberUpdate {
    std::uint64_t node = 0;
    std::uint64_t incarnation = 0;
    std::uint8_t status = 0;  // MemberStatus
};

// ADR-045: one gossiped node-capability fact, opaque to SwimMembership. `blob` is an
// encode_node_capabilities()-produced byte string (capability_registry.hpp) — SwimMembership never
// decodes it, only carries it, exactly as `revocations` carries opaque Fingerprints. Freshness is
// `(incarnation, local_seq)`: incarnation strictly-greater wins (the same freshness axis SWIM already
// uses for membership, no second mechanism); within an unchanged incarnation, `local_seq` — a
// monotonic counter owned by the publishing node, bumped on every local republish — breaks the tie.
// `blob` is compared byte-lexicographically ONLY as a last resort for a genuine
// (node, incarnation, local_seq) collision (a forged/duplicated entry), never on the normal path.
struct CapabilityDigestEntry {
    NodeId node{};
    std::uint64_t incarnation = 0;
    std::uint32_t local_seq = 0;
    std::vector<std::byte> blob;
};

// ADR-046: one gossiped node-load fact — a node's own aggregate `resident_total` (governed-mailbox
// depth summed across every activation it hosts, `Engine::resident_total()`), advertised to direct
// peers every gossip round. Purely informational/observability here (a peer MAY use it to tune its
// own `PeerCongestionTable` bucket sizing in a future refinement — this ADR does not auto-derive one
// from the other, per its own "unvalidated operational config" residual risk). The ACTUAL admission
// throttle is driven exclusively by the separate, edge-triggered `ControlKind::Congested` frame
// below, never by this periodic gauge.
struct MailboxPressureEntry {
    NodeId node{};
    std::uint64_t incarnation = 0;
    std::uint64_t resident_total = 0;
};

// A defensive, decode-side-only ceiling on the TOTAL capability-blob bytes accepted out of one control
// frame, independent of what any individual entry's declared length claims (ADR-045 S3) — a malformed
// or adversarial frame cannot force a large allocation just by declaring large per-entry lengths. Set
// well above the default per-tick send budget (`Config::max_capability_gossip_bytes`, 4096) so a
// legitimate frame from a node configured with a larger budget is never itself rejected.
inline constexpr std::size_t kMaxDecodedCapabilityBytes = 65536;

struct ControlMsg {
    ControlKind kind{};
    ClusterId cluster{};
    NodeId from{};
    std::uint64_t from_incarnation = 0;
    NodeId subject{};  // PingReq/PingReqAck target; Join subject
    std::uint64_t subject_incarnation = 0;
    std::uint64_t seq = 0;  // probe correlation
    std::vector<MemberUpdate> updates;
    // ADR-040 Phase 5: a bounded piggyback of revoked-certificate fingerprints, riding the SAME gossip
    // channel as `updates` (no new one — matches the ADR record's proven C3 convergence-ratio evidence).
    // SwimMembership moves these as opaque 32-byte blobs (set_revocation_gossip); it does not interpret
    // them — that stays 020/RevocationRegistry's job.
    std::vector<Fingerprint> revocations;
    // ADR-045: a bounded piggyback of gossiped node capabilities, riding the SAME channel as `updates`/
    // `revocations` above (NOT a new side-channel — the explicit design mandate). SwimMembership moves
    // these opaque entries (set_capability_gossip); it does not interpret them — that stays
    // 025/CapabilityRegistry's job (capability_registry.hpp).
    std::vector<CapabilityDigestEntry> capabilities;
    // ADR-046: a bounded piggyback of the sender's own advertised mailbox pressure (at most one entry
    // in practice — a node only ever advertises ITS OWN resident_total), riding the SAME channel.
    std::vector<MailboxPressureEntry> pressure;
    // ADR-046: for `kind == Congested` only — how much longer (from THIS frame's arrival, in the
    // receiver's OWN clock domain) the sender should keep throttling `from`. A relative duration, not
    // an absolute deadline, so no cross-node clock-skew travels on the wire (018-style deadline-travel
    // reconstruction, reusing 006's existing vocabulary rather than inventing a new one). Unused
    // (left 0) for every other `ControlKind`.
    std::int64_t congestion_remaining_ns = 0;
};

namespace detail {

// A tiny little-endian byte writer/reader for the control codec. Deterministic, std-only, bounds-
// checked on read (a malformed control frame is dropped, never UB) — this is not the hot path.
inline void put_u8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(std::byte{v}); }
inline void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
    put_u8(b, static_cast<std::uint8_t>(v));
    put_u8(b, static_cast<std::uint8_t>(v >> 8));
}
inline void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) put_u8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}
inline void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) put_u8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}

struct SwimByteReader {
    const std::byte* p;
    const std::byte* end;
    bool ok = true;
    [[nodiscard]] std::uint8_t u8() noexcept {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint8_t>(*p++);
    }
    [[nodiscard]] std::uint16_t u16() noexcept {
        const std::uint16_t lo = u8();
        const std::uint16_t hi = u8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }
    [[nodiscard]] std::uint32_t u32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        return v;
    }
    [[nodiscard]] std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
    [[nodiscard]] Fingerprint bytes32() noexcept {
        Fingerprint fp{};
        for (std::byte& x : fp) x = static_cast<std::byte>(u8());
        return fp;
    }
};

inline std::vector<std::byte> encode_control(const ControlMsg& m) {
    std::vector<std::byte> b;
    put_u8(b, static_cast<std::uint8_t>(m.kind));
    put_u64(b, m.cluster.value);
    put_u64(b, m.from.value);
    put_u64(b, m.from_incarnation);
    put_u64(b, m.subject.value);
    put_u64(b, m.subject_incarnation);
    put_u64(b, m.seq);
    put_u16(b, static_cast<std::uint16_t>(m.updates.size()));
    for (const MemberUpdate& u : m.updates) {
        put_u64(b, u.node);
        put_u64(b, u.incarnation);
        put_u8(b, u.status);
    }
    put_u16(b, static_cast<std::uint16_t>(m.revocations.size()));
    for (const Fingerprint& fp : m.revocations)
        for (std::byte x : fp) put_u8(b, static_cast<std::uint8_t>(x));
    put_u16(b, static_cast<std::uint16_t>(m.capabilities.size()));
    for (const CapabilityDigestEntry& e : m.capabilities) {
        put_u64(b, e.node.value);
        put_u64(b, e.incarnation);
        put_u32(b, e.local_seq);
        put_u16(b, static_cast<std::uint16_t>(e.blob.size()));
        for (std::byte x : e.blob) put_u8(b, static_cast<std::uint8_t>(x));
    }
    put_u16(b, static_cast<std::uint16_t>(m.pressure.size()));
    for (const MailboxPressureEntry& e : m.pressure) {
        put_u64(b, e.node.value);
        put_u64(b, e.incarnation);
        put_u64(b, e.resident_total);
    }
    put_u64(b, static_cast<std::uint64_t>(m.congestion_remaining_ns));
    return b;
}

[[nodiscard]] inline bool decode_control(const std::byte* data, std::size_t n, ControlMsg& out) {
    SwimByteReader r{data, data + n};
    out.kind = static_cast<ControlKind>(r.u8());
    out.cluster.value = r.u64();
    out.from.value = r.u64();
    out.from_incarnation = r.u64();
    out.subject.value = r.u64();
    out.subject_incarnation = r.u64();
    out.seq = r.u64();
    const std::uint16_t count = r.u16();
    out.updates.clear();
    for (std::uint16_t i = 0; i < count && r.ok; ++i) {
        MemberUpdate u;
        u.node = r.u64();
        u.incarnation = r.u64();
        u.status = r.u8();
        out.updates.push_back(u);
    }
    const std::uint16_t revocation_count = r.u16();
    out.revocations.clear();
    for (std::uint16_t i = 0; i < revocation_count && r.ok; ++i) out.revocations.push_back(r.bytes32());

    const std::uint16_t capability_count = r.u16();
    out.capabilities.clear();
    std::size_t decoded_capability_bytes = 0;
    for (std::uint16_t i = 0; i < capability_count && r.ok; ++i) {
        CapabilityDigestEntry e;
        e.node.value = r.u64();
        e.incarnation = r.u64();
        e.local_seq = r.u32();
        const std::uint16_t blob_len = r.u16();
        // Defensive cap BEFORE allocating (ADR-045 S3): a malformed/adversarial declared length can
        // never force a large allocation, independent of what any single entry claims.
        decoded_capability_bytes += blob_len;
        if (!r.ok || decoded_capability_bytes > kMaxDecodedCapabilityBytes) {
            r.ok = false;
            break;
        }
        e.blob.resize(blob_len);
        for (std::byte& x : e.blob) x = static_cast<std::byte>(r.u8());
        out.capabilities.push_back(std::move(e));
    }

    const std::uint16_t pressure_count = r.u16();
    out.pressure.clear();
    for (std::uint16_t i = 0; i < pressure_count && r.ok; ++i) {
        MailboxPressureEntry e;
        e.node.value = r.u64();
        e.incarnation = r.u64();
        e.resident_total = r.u64();
        out.pressure.push_back(e);
    }
    out.congestion_remaining_ns = static_cast<std::int64_t>(r.u64());
    return r.ok;
}

}  // namespace detail

// ============================================================================================
// Demux one Transport endpoint into the 010 data plane and the 021 control plane on `MessageFrame::kind`.
// The data sink sees a `Data`-only stream (unchanged from 010); control frames go to `control_sink`.
// Attach the result to `Transport::on_receive` in a node that runs both planes.
// ============================================================================================
[[nodiscard]] inline std::function<void(MessageFrame)> control_data_demux(
    std::function<void(MessageFrame)> data_sink, std::function<void(MessageFrame)> control_sink) {
    return [d = std::move(data_sink), c = std::move(control_sink)](MessageFrame f) {
        if (f.kind == FrameKind::Control) {
            if (c) c(std::move(f));
        } else if (d) {
            d(std::move(f));
        }
    };
}

// ============================================================================================
// StabilizationWindow (021 §3 anti-flap) — a membership change must HOLD for `settle_ns` before it
// drives re-placement of healthy actors. A node that flaps within the window causes NO reshuffle; a
// genuine change sustained past the window commits exactly once. Pure + virtual-clock driven: feed it
// the current roster digest and `now_ns`; it returns whether re-placement should fire NOW.
//
//   observe(digest, now):
//     digest == committed  → nothing pending; flap resolved, return false.
//     digest != committed, new pending → arm pending@now, return false (inside the window).
//     digest != committed, pending unchanged, now-since ≥ settle → COMMIT digest, return true (once).
// ============================================================================================
class StabilizationWindow {
public:
    explicit StabilizationWindow(std::int64_t settle_ns) noexcept : settle_ns_(settle_ns) {}

    // Seed the committed roster (no re-placement fires for the initial view).
    void set_committed(std::uint64_t digest) noexcept {
        committed_ = digest;
        pending_active_ = false;
    }

    // Returns true exactly once when `digest` has been stably different from the committed roster for
    // the full settle interval — the moment re-placement should run.
    [[nodiscard]] bool observe(std::uint64_t digest, std::int64_t now_ns) noexcept {
        if (digest == committed_) {
            pending_active_ = false;  // flapped back before settling ⇒ no reshuffle
            return false;
        }
        if (!pending_active_ || digest != pending_digest_) {
            pending_active_ = true;
            pending_digest_ = digest;
            pending_since_ns_ = now_ns;
            return false;
        }
        if (now_ns - pending_since_ns_ >= settle_ns_) {
            committed_ = digest;
            pending_active_ = false;
            return true;  // settled: re-place now
        }
        return false;  // still inside the window
    }

    [[nodiscard]] std::uint64_t committed() const noexcept { return committed_; }
    [[nodiscard]] bool pending() const noexcept { return pending_active_; }

private:
    std::int64_t settle_ns_;
    std::uint64_t committed_ = 0;
    std::uint64_t pending_digest_ = 0;
    std::int64_t pending_since_ns_ = 0;
    bool pending_active_ = false;
};

// A stable order-independent digest of a live-node set — the "roster digest" the stabilization window
// and 026's path-change boundary key on. Folding is commutative (XOR of per-node mixes) so it does not
// depend on iteration order.
[[nodiscard]] inline std::uint64_t roster_digest(std::span<const NodeId> nodes) noexcept {
    std::uint64_t acc = 0;
    for (NodeId n : nodes) acc ^= detail::splitmix64(n.value + 0x9E3779B97F4A7C15ULL);
    return acc;
}

// ============================================================================================
// SwimMembership (021 §3) — the real failure detector implementing the 010 `Membership` interface over
// the `Transport` seam. Single membership/protocol thread by contract (all mutation happens in tick()
// and inbound frame handling, which run on the driving thread); view()/epoch() publish an immutable
// snapshot under a mutex so a cross-thread reader (placement per-send) is UAF-free and TSan-clean,
// exactly as InProcessMembership does.
// ============================================================================================
class SwimMembership final : public Membership {
public:
    using ClockFn = std::int64_t (*)(void* ctx) noexcept;  // mirrors Activation::set_clock (014)

    struct Config {
        ClusterId cluster_id{};
        std::int64_t ack_timeout_ns = 200'000'000;         // direct ack deadline
        std::int64_t suspicion_timeout_ns = 1'000'000'000;  // Suspect → Dead with no refutation
        std::uint32_t indirect_k = 2;                        // relay peers for a ping-req
        std::uint32_t gossip_fanout = 3;                     // peers a gossip round targets
        std::uint32_t max_gossip_updates = 32;               // bounded piggyback digest
        std::uint64_t max_capability_gossip_bytes = 4096;    // ADR-045: bounded capability piggyback
        std::uint64_t seed = 0;                              // deterministic peer/fanout selection
        // ADR-048 relay correlation secret. 0 ⇒ a fresh random secret per SwimMembership instance.
        // The token must NOT be derivable from the public seed + NodeId (the old derivation
        // splitmix64(seed ^ self ^ constant) let any peer forge relay Acks).
        std::uint64_t relay_secret = 0;
        // ADR-046: cross-node backpressure watermarks (resident-message-count hysteresis) and the
        // Congested signal's TTL. Unvalidated-at-real-workload-scale operational config (ADR's own
        // disclosed residual risk) — tune per deployment, not derived from anything else here.
        std::uint64_t congestion_high_watermark = 10'000;   // resident_total crossing ⇒ broadcast
        std::uint64_t congestion_low_watermark = 5'000;      // must fall below this before a new edge
        std::int64_t congestion_window_ns = 2'000'000'000;   // 2s — TTL stamped on a Congested frame
        std::int64_t congestion_refresh_ns = 500'000'000;    // 500ms — re-broadcast while still congested
        // ADR-048: indirect-ping window, tombstone lifetime/cap, and the per-tick budgets for verify
        // pings (a second-hand Dead re-checked directly) and probe-backs (a Dead tombstone re-pinged so
        // a healed node learns it was declared dead and refutes). All timers use the injected clock.
        // The relay round trip is 4 hops and must fit within the indirect window — raise it on a WAN.
        std::int64_t indirect_timeout_ns = 0;                    // 0 ⇒ ack_timeout_ns
        std::int64_t tombstone_ttl_ns = 60'000'000'000;          // must exceed the longest tolerated partition
        std::uint32_t max_tombstones = 256;                      // hard cap; Dead evicted before Left
        // Hard cap on member-table entries. Gossip-created (unverified) entries are dropped once
        // reached; first-hand/seed-admitted joins are still allowed so a legitimate joiner is not
        // locked out by a ghost flood.
        std::uint32_t max_live_members = 4096;
        std::int64_t tombstone_probe_interval_ns = 500'000'000;  // min spacing per tombstone
        std::uint32_t tombstone_probe_budget = 2;                // probe-backs per tick
        std::uint32_t verify_ping_budget = 16;                   // verify pings per tick
        // P2: pre-auth/control-plane budget. A peer cannot flood Control/Gossip frames faster than
        // this (fixed 256-bucket table; spoofed ids collide rather than growing memory). 0 disables.
        std::uint32_t control_rate_per_sec = 4096;
        std::uint32_t control_burst = 1024;
        // P2: gossip-only (unverified) members that never answer directly are erased after this.
        // 0 disables the reaper. Placement already excludes them; this bounds the convergence view.
        std::int64_t unverified_ttl_ns = 30'000'000'000;
    };

    // A graceful leave raises the incarnation by this band, so a Left outranks every Alive/Suspect the
    // node could have had before it (ADR-048 R6'). u64 allows ~2^32 leave/rejoin cycles per identity.
    static constexpr std::uint64_t kLeftBand = std::uint64_t{1} << 32;

    SwimMembership(NodeId self, Transport& transport, Config cfg)
        : self_(self), transport_(&transport), cfg_(sanitize(cfg)) {
        std::uint64_t secret = cfg_.relay_secret;
        if (secret == 0) {
            std::random_device rd;
            secret = (static_cast<std::uint64_t>(rd()) << 32) ^
                     static_cast<std::uint64_t>(rd());
            if (secret == 0) secret = 0x5E1A7C0FFEEULL;  // pathological RNG; still non-public
        }
        relay_key_ = detail::splitmix64(cfg_.seed ^ self_.value ^ secret ^ 0x5E1A7ULL);
        Member me{};
        me.cluster = cfg_.cluster_id;
        me.incarnation = self_incarnation_;
        me.verified = true;
        members_[self_.value] = me;
        republish();
        transport_->on_receive([this](MessageFrame f) { on_frame(std::move(f)); });
    }

    SwimMembership(const SwimMembership&) = delete;
    SwimMembership& operator=(const SwimMembership&) = delete;

    // --- 010 Membership interface -------------------------------------------------------------
    [[nodiscard]] NodeId self() const noexcept override { return self_; }
    [[nodiscard]] MembershipView view() const noexcept override {
        std::lock_guard<std::mutex> g(mu_);
        return MembershipView{snapshot_, epoch_};
    }
    // Placement sees only members with first-hand evidence (or self). Gossip-created ghosts stay in
    // `view()` for convergence/probing but can never win an HRW placement.
    [[nodiscard]] MembershipView placement_view() const noexcept override {
        std::lock_guard<std::mutex> g(mu_);
        return MembershipView{verified_snapshot_, epoch_};
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return epoch_;
    }

    // --- Clock injection (014). Default: real steady clock; a sim/test overrides via set_clock(). ---
    void set_clock(ClockFn fn, void* ctx) noexcept {
        clock_fn_ = fn;
        clock_ctx_ = ctx;
    }
    [[nodiscard]] std::int64_t now_ns() const noexcept { return clock_fn_(clock_ctx_); }

    // Attach the 010 data-plane sink for a node that runs both planes over this one endpoint. Control
    // frames are handled internally; data frames are forwarded here (default: dropped). Setting this
    // after construction re-registers a demuxing receiver on the transport.
    void set_data_sink(std::function<void(MessageFrame)> sink) {
        data_sink_ = std::move(sink);
        transport_->on_receive([this](MessageFrame f) { on_frame(std::move(f)); });
    }

    // --- ADR-040 Phase 5: revocation gossip piggyback + sweep hook -----------------------------
    // `pull` supplies a bounded set of fingerprints to attach to each OUTBOUND control frame (from
    // this node's RevocationRegistry); `merge` absorbs an INBOUND frame's piggybacked set into it.
    // SwimMembership stays security-ignorant: it moves 32-byte blobs, it does not interpret them —
    // the header banner above and revocation_registry.hpp's own banner own that distinction.
    using RevocationPullFn = std::function<std::vector<Fingerprint>(std::size_t max)>;
    using RevocationMergeFn = std::function<void(const std::vector<Fingerprint>&)>;
    void set_revocation_gossip(RevocationPullFn pull, RevocationMergeFn merge) {
        revocation_pull_ = std::move(pull);
        revocation_merge_ = std::move(merge);
    }

    // --- ADR-045: capability gossip piggyback + a live network-backed CapabilityView -----------
    // `pull` supplies a bounded set of `CapabilityDigestEntry` to attach to each OUTBOUND control frame
    // (from this node's CapabilityRegistry, up to `Config::max_capability_gossip_bytes`); `merge`
    // absorbs an INBOUND frame's piggybacked set into it. Identical shape to `set_revocation_gossip`
    // above — SwimMembership stays capability-ignorant: it moves `CapabilityDigestEntry` values, it
    // does not interpret the blobs inside them — that stays 025/CapabilityRegistry's job
    // (capability_registry.hpp).
    using CapabilityPullFn =
        std::function<std::vector<CapabilityDigestEntry>(NodeId self, std::uint64_t self_incarnation,
                                                          std::uint64_t max_bytes)>;
    using CapabilityMergeFn = std::function<void(const std::vector<CapabilityDigestEntry>&)>;
    void set_capability_gossip(CapabilityPullFn pull, CapabilityMergeFn merge) {
        capability_pull_ = std::move(pull);
        capability_merge_ = std::move(merge);
    }

    // --- ADR-046: mailbox-pressure gossip piggyback + the Congested watermark trigger -----------
    // `query` reports THIS node's own current `resident_total` (typically `Engine::resident_total()`)
    // — used both to fill the periodic piggyback below and, internally, to decide when to broadcast a
    // `Congested` frame (monitor_congestion). `merge` absorbs an INBOUND frame's piggybacked entries
    // (a peer's own advertised load) — purely observational here, exposed via `pressure_of`. Identical
    // shape to `set_capability_gossip`/`set_revocation_gossip` above; SwimMembership does not act on
    // the periodic gauge itself, only on the separate edge-triggered Congested signal (handle_control).
    using PressureQueryFn = std::function<std::uint64_t()>;
    using PressureMergeFn = std::function<void(const std::vector<MailboxPressureEntry>&)>;
    void set_pressure_gossip(PressureQueryFn query, PressureMergeFn merge) {
        pressure_query_ = std::move(query);
        pressure_merge_ = std::move(merge);
    }

    // Last-advertised resident_total for a peer (021 §9 observability; mirrors incarnation_of). 0 for
    // an unknown peer or one that has never gossiped pressure.
    [[nodiscard]] std::uint64_t pressure_of(NodeId n) const {
        const auto it = last_pressure_.find(n.value);
        return it == last_pressure_.end() ? 0 : it->second;
    }

    // A generic per-tick injection point (same idiom as set_clock/set_link_reachable), called at the
    // end of every tick() with the current virtual time. Node bootstrap wires this to
    // SecureTransport::sweep_revocations(registry.snapshot()) (Phase 5) and sweep_rotation(...)
    // (Phase 6) — SwimMembership itself has no idea what the hook does.
    using SweepHookFn = std::function<void(std::int64_t now_ns)>;
    void set_sweep_hook(SweepHookFn fn) { sweep_hook_ = std::move(fn); }

    // --- TEST seams (documented; stand in for 019/020 mechanics) -------------------------------
    // A one-way reachability filter for THIS node's outbound control sends — a partial partition. A
    // real partition is observed by the 019 PAL socket layer; this injects it for indirect-ping tests.
    void set_link_reachable(std::function<bool(NodeId)> pred) { link_reachable_ = std::move(pred); }
    // Crash / revive: an offline node ignores inbound control, sends nothing, and does not tick — the
    // exact observable of a process that stopped acking (no transport detach needed).
    void set_online(bool on) noexcept { online_ = on; }
    [[nodiscard]] bool online() const noexcept { return online_; }

    // --- Join (021 §1/§3). Admit `joiner` iff its presented cluster id matches (accidental-merge
    // guard) — the in-process form of the join gate the 020 AUTHENTICATE step precedes. Returns true
    // and advances the epoch on admission; false on cluster-id mismatch. Unit-testable directly. Uses
    // the same re-admission rule as a Join frame (above any tombstone), so admit after leave()/Dead
    // works (ADR-048). -----------------------------------------------------------------------------
    [[nodiscard]] bool admit(NodeId joiner, ClusterId presented) {
        if (presented != cfg_.cluster_id) return false;  // §1 reject: wrong cluster
        std::uint64_t inc = 0;
        if (auto it = members_.find(joiner.value); it != members_.end() && is_tomb(it->second.status))
            inc = it->second.incarnation + 1;
        apply_update(joiner, inc, MemberStatus::Alive, Src::Join, now_ns());  // roster grows, epoch++
        return true;
    }

    // Drive a join from THIS node against a seed contact: send a Join carrying our cluster id. The seed
    // validates and replies JoinAck (with a snapshot) or JoinReject. Cold, setup-time.
    void request_join(NodeId seed) {
        ControlMsg m;
        m.kind = ControlKind::Join;
        m.subject = self_;
        m.subject_incarnation = self_incarnation_;
        send_control(seed, m);
    }

    // Graceful leave (021 §3 scale-down): remove `who` from the roster (epoch++). Self-leave marks this
    // node departed; a real drain/hand-off (015/012/017) is a documented seam this does not perform.
    // Writes an authoritative Left tombstone at incarnation + kLeftBand (ADR-048 R6'). A self-left
    // node is never refuted; its Left spreads as the sender's self entry in every frame it sends.
    void leave(NodeId who) {
        if (who == self_) {
            Member& me = members_[self_.value];
            self_incarnation_ = me.incarnation + kLeftBand;
            me.incarnation = self_incarnation_;
            me.status = MemberStatus::Left;
            self_left_ = true;
            republish();
            return;
        }
        apply_update(who, incarnation_of(who) + kLeftBand, MemberStatus::Left, Src::Local, now_ns());
    }

    // --- The SWIM protocol period (021 §3). One call = one protocol tick. Reads the injected clock; a
    // test advances virtual time between ticks to drive ping→miss→indirect→suspect→dead deterministic-
    // ally. NO sleeping, NO wall-clock wait. ------------------------------------------------------
    void tick() {
        if (!online_) return;
        const std::int64_t now = now_ns();
        reap_suspects(now);   // Suspect → Dead once the suspicion timeout lapses with no refutation
        reap_tombstones(now);  // erase tombstones older than tombstone_ttl_ns
        reap_unverified(now);  // P2: drop gossip-only members that never answered
        drive_probe(now);     // resolve/escalate the in-flight probe by virtual time
        start_probe(now);     // begin a new direct probe of the next peer
        send_verifies();      // directly re-check peers that gossip (not we) reported Dead
        probe_tombstones(now);  // re-ping Dead tombstones so a healed node learns and refutes
        gossip_round(now);    // disseminate the member digest to a fanout subset
        monitor_congestion(now);  // ADR-046: watermark-crossing ⇒ broadcast Congested
        if (sweep_hook_) sweep_hook_(now);  // ADR-040 Phase 5/6: revocation + rotation sweeps
    }

    // Observe a specific member's status (tests / 009 observability).
    [[nodiscard]] MemberStatus status_of(NodeId n) const {
        const auto it = members_.find(n.value);
        return it == members_.end() ? MemberStatus::Dead : it->second.status;
    }
    [[nodiscard]] std::uint64_t incarnation_of(NodeId n) const {
        const auto it = members_.find(n.value);
        return it == members_.end() ? 0 : it->second.incarnation;
    }
    [[nodiscard]] std::uint64_t self_incarnation() const noexcept { return self_incarnation_; }
    // Tombstone diagnostics (driving thread only; 009 observability / ADR-048 tests).
    [[nodiscard]] std::uint32_t tombstone_count() const noexcept { return tomb_count_; }
    [[nodiscard]] bool is_known(NodeId n) const { return members_.contains(n.value); }
    [[nodiscard]] std::uint64_t control_sheds() const noexcept {
        return control_sheds_.load(std::memory_order_relaxed);
    }

private:
    // Where an update came from — precedence depends on it (ADR-048): only Local may create a Dead
    // tombstone; a Gossip Dead is downgraded to Suspect and verified; Join may lift a tombstone.
    enum class Src : std::uint8_t { Local, Gossip, Direct, Join, JoinSnapshot };
    static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::min();

    struct Member {
        std::uint64_t incarnation = 0;
        std::int64_t suspect_deadline_ns = 0;  // when Suspect becomes Dead (valid iff Suspect)
        std::int64_t death_ns = 0;             // when the tombstone was written (valid iff Dead|Left)
        std::int64_t last_direct_ns = kNever;  // last frame received from this node itself (R2 veto)
        std::int64_t last_probe_ns = kNever;   // last tombstone probe-back (R5' pacing)
        ClusterId cluster{};
        MemberStatus status = MemberStatus::Alive;
        std::uint8_t tx_left = 0;              // frames left to piggyback this entry on (R4')
        bool verify_pending = false;           // a second-hand Suspect awaiting a verify ping (R1b)
        // First-hand evidence (a Direct frame/Ack from the node itself, or a seed-admitted Join).
        // Gossip/JoinSnapshot members start unverified: they may be probed, but they are NOT in the
        // placement view and are NOT re-gossiped until they answer directly. This prevents a peer
        // from minting ghost nodes that receive placed actors (F12).
        bool verified = false;
        std::int64_t first_seen_ns = 0;  // P2: start of the unverified TTL
    };

    enum class Phase : std::uint8_t { Direct, Indirect };
    struct Probe {
        bool active = false;
        Phase phase = Phase::Direct;
        NodeId target{};
        std::int64_t deadline_ns = 0;
        std::uint64_t seq = 0;
        bool acked = false;
    };

    static Config sanitize(Config c) noexcept {
        if (c.ack_timeout_ns <= 0) c.ack_timeout_ns = 1;  // bucket() divides by indirect_ns()
        if (c.indirect_timeout_ns < 0) c.indirect_timeout_ns = 0;
        if (c.max_live_members == 0) c.max_live_members = 4096;
        if (c.control_burst == 0) c.control_burst = 1;
        if (c.unverified_ttl_ns < 0) c.unverified_ttl_ns = 0;
        return c;
    }
    static constexpr bool is_tomb(MemberStatus s) noexcept {
        return s == MemberStatus::Dead || s == MemberStatus::Left;
    }
    [[nodiscard]] std::int64_t indirect_ns() const noexcept {
        return std::max<std::int64_t>(1, cfg_.indirect_timeout_ns ? cfg_.indirect_timeout_ns
                                                                  : cfg_.ack_timeout_ns);
    }
    // How many frames a changed entry is piggybacked on: 3·⌈log2(n+1)⌉, SWIM's dissemination bound.
    [[nodiscard]] std::uint8_t piggyback_count() const noexcept {
        const unsigned v = 3u * static_cast<unsigned>(std::bit_width(members_.size()));
        return static_cast<std::uint8_t>(std::min(v, 255u));
    }

    // Canonical PAL monotonic clock (018/019 — CLOCK_BOOTTIME class, counts suspend). SWIM ping
    // deadlines ride pal::now() so a node suspended mid-probe treats the probe as timed out on
    // resume (re-probe the peer) — the same clock domain as every other engine deadline.
    static std::int64_t real_steady_ns(void*) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   pal::now().time_since_epoch())
            .count();
    }

    // --- outbound -----------------------------------------------------------------------------
    void send_control(NodeId to, ControlMsg& m) {
        if (!online_) return;
        if (link_reachable_ && !link_reachable_(to)) return;  // partial partition (test seam)
        m.cluster = cfg_.cluster_id;
        m.from = self_;
        m.from_incarnation = self_incarnation_;
        fill_digest(to, m.updates);
        if (revocation_pull_) m.revocations = revocation_pull_(cfg_.max_gossip_updates);
        if (capability_pull_)
            m.capabilities = capability_pull_(self_, self_incarnation_, cfg_.max_capability_gossip_bytes);
        if (pressure_query_)
            m.pressure = {MailboxPressureEntry{self_, self_incarnation_, pressure_query_()}};
        MessageFrame f;
        f.from = self_;
        f.to = to;
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(m);
        transport_->send(to, std::move(f));
    }

    // Bounded gossip digest (ADR-048 R4'), in priority order:
    //   1. the receiver's own entry — an accusation reaches its subject first, so it can refute;
    //   2. the sender's self entry;
    //   3. recently changed entries (Suspect first), each piggybacked on piggyback_count() frames;
    //   4. a rotating cursor over the sorted roster, so every entry is eventually re-sent.
    // At least ⌈(cap-2)/2⌉ slots are reserved for (4), so a burst of changes cannot starve coverage.
    void fill_digest(NodeId to, std::vector<MemberUpdate>& out) {
        out.clear();
        const std::size_t cap = cfg_.max_gossip_updates;
        if (cap == 0) return;
        auto push = [&](std::uint64_t id, Member& m) {
            out.push_back(MemberUpdate{id, m.incarnation, static_cast<std::uint8_t>(m.status)});
            if (m.tx_left) set_tx(m, static_cast<std::uint8_t>(m.tx_left - 1));
        };
        if (to != self_)
            if (auto it = members_.find(to.value); it != members_.end()) push(it->first, it->second);
        if (out.size() < cap) push(self_.value, members_[self_.value]);
        const std::size_t fixed = out.size();
        const std::size_t others = members_.size() - fixed;
        const std::size_t reserved = std::min(others, cap > 2 ? (cap - 1) / 2 : std::size_t{0});
        const std::size_t piggy_cap = cap - reserved;
        for (int pass = 0; pass < 2 && tx_pending_ > 0 && out.size() < piggy_cap; ++pass) {
            for (auto& [id, m] : members_) {
                if (out.size() >= piggy_cap) break;
                if (id == to.value || id == self_.value || m.tx_left == 0) continue;
                if (!m.verified) continue;  // never amplify an unverified ghost
                if ((m.status == MemberStatus::Suspect) != (pass == 0)) continue;
                push(id, m);
            }
        }
        if (out.size() >= cap) return;
        refresh_sorted_ids();
        const std::size_t n = sorted_ids_.size();
        const std::size_t start = static_cast<std::size_t>(
            std::upper_bound(sorted_ids_.begin(), sorted_ids_.end(), digest_cursor_) - sorted_ids_.begin());
        for (std::size_t k = 0; k < n && out.size() < cap; ++k) {
            const std::uint64_t id = sorted_ids_[(start + k) % n];
            if (id == to.value || id == self_.value) continue;
            bool already = false;
            for (std::size_t i = fixed; i < out.size() && !already; ++i) already = out[i].node == id;
            if (already) continue;
            const Member& m = members_.find(id)->second;
            if (!m.verified) continue;  // rotating digest covers verified members only
            out.push_back(MemberUpdate{id, m.incarnation, static_cast<std::uint8_t>(m.status)});
            digest_cursor_ = id;
        }
    }

    // Every tx_left write goes through here so tx_pending_ (entries still to piggyback) stays exact;
    // when it is 0 — the steady state — fill_digest skips the piggyback passes entirely.
    void set_tx(Member& m, std::uint8_t v) noexcept {
        if (m.tx_left == 0 && v != 0) ++tx_pending_;
        else if (m.tx_left != 0 && v == 0) --tx_pending_;
        m.tx_left = v;
    }

    // The cursor's sorted id list, rebuilt only when an entry is added or erased (not per frame).
    void refresh_sorted_ids() {
        if (!ids_dirty_) return;
        sorted_ids_.clear();
        for (const auto& [id, m] : members_)
            if (!is_tomb(m.status) && (m.verified || id == self_.value)) sorted_ids_.push_back(id);
        std::sort(sorted_ids_.begin(), sorted_ids_.end());
        ids_dirty_ = false;
    }

    // --- inbound ------------------------------------------------------------------------------
    void on_frame(MessageFrame f) {
        if (f.to != self_) return;  // a transport may hand us frames addressed elsewhere; never act on them
        if (f.kind != FrameKind::Control) {
            if (data_sink_) data_sink_(std::move(f));  // 010 data plane, untouched
            return;
        }
        if (!online_) return;  // crashed node ignores control (stops acking)
        // P2: a fixed, per-sender Control-frame budget before decode/handle. Gossip and probe traffic
        // is far below this; a flood is shed here rather than driving republish/probe work.
        if (!admit_control(f.from, now_ns())) {
            control_sheds_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ControlMsg m;
        if (!detail::decode_control(f.payload.data(), f.payload.size(), m)) return;  // malformed → drop
        handle_control(m, f.from);
    }

    // Stateless relay correlation (ADR-048): a relay remembers nothing about a PingReq. It stamps the
    // Ping it forwards with the requester id (`subject`) and a keyed token (`subject_incarnation`); the
    // subject echoes both in its Ack, and the relay re-derives the token to authenticate the echo before
    // forwarding a PingReqAck. The token is bucketed by the indirect window (current or previous bucket
    // accepted), so a late Ack older than ~2 windows is ignored. Bit 0 is forced on: a token is never 0,
    // which is what a reply to this node's OWN probe carries.
    [[nodiscard]] std::uint64_t relay_token(NodeId req, NodeId subj, std::uint64_t seq,
                                            std::int64_t bucket) const noexcept {
        return detail::splitmix64(relay_key_ ^
                                  mix(req.value, subj.value, seq ^ static_cast<std::uint64_t>(bucket))) |
               1u;
    }
    [[nodiscard]] std::int64_t bucket(std::int64_t now) const noexcept { return now / indirect_ns(); }

    void handle_control(const ControlMsg& m, NodeId authenticated_from) {
        // A peer may only speak for ITSELF. The authenticated transport identity (a mTLS session's
        // peer, or the HELLO identity on a plaintext link) is `authenticated_from`; the payload's
        // `m.from` is attacker-controlled. Without this check one admitted node could impersonate
        // any other node in SWIM gossip (forge Left/Dead/Suspect, mint members, trigger relays).
        if (m.from != authenticated_from) return;
        // A node may join only ITSELF. Without this, a peer could present a Join for a third-party
        // id and have it admitted as a verified (first-hand) member, bypassing the live-member cap
        // and the unverified-gossip placement guard.
        if (m.kind == ControlKind::Join && m.subject != m.from) return;
        // Join is the ONE control that legitimately crosses a cluster-id boundary (it is how a mismatch
        // is detected). Every other control from a foreign cluster is dropped (accidental-merge guard).
        if (m.kind != ControlKind::Join && m.cluster != cfg_.cluster_id) return;
        const std::int64_t now = now_ns();

        // Absorb piggybacked gossip — but ONLY from a same-cluster frame, so a mismatched Join cannot
        // smuggle its roster in via the digest and defeat the accidental-merge guard below.
        if (m.cluster == cfg_.cluster_id) {
            // A frame from a known peer is first-hand evidence it is alive (R2 veto), recorded BEFORE
            // the digest is merged so a stale accusation in this same frame cannot override it.
            if (m.from != self_) {
                if (auto it = members_.find(m.from.value); it != members_.end()) {
                    it->second.last_direct_ns = now;
                    apply_update(m.from, m.from_incarnation, MemberStatus::Alive, Src::Direct, now);
                }
            }
            for (const MemberUpdate& u : m.updates) {  // may trigger self-refutation
                // A node may announce its OWN graceful Left (the sender is authenticated above), but
                // it may not tombstone a THIRD PARTY: a gossip Left is otherwise authoritative and
                // cannot be refuted, which would let one peer permanently evict any other node.
                if (static_cast<MemberStatus>(u.status) == MemberStatus::Left &&
                    u.node != m.from.value)
                    continue;
                apply_update(NodeId{u.node}, u.incarnation, static_cast<MemberStatus>(u.status),
                             Src::Gossip, now);
            }
            if (revocation_merge_ && !m.revocations.empty()) revocation_merge_(m.revocations);
            if (capability_merge_ && !m.capabilities.empty()) capability_merge_(m.capabilities);
            for (const MailboxPressureEntry& e : m.pressure) last_pressure_[e.node.value] = e.resident_total;
            if (pressure_merge_ && !m.pressure.empty()) pressure_merge_(m.pressure);
        }

        switch (m.kind) {
            case ControlKind::Ping: {
                ControlMsg ack;
                ack.kind = ControlKind::Ack;
                ack.seq = m.seq;
                ack.subject = m.subject;  // echo the relay correlation verbatim (opaque to us)
                ack.subject_incarnation = m.subject_incarnation;
                send_control(m.from, ack);  // carries our (possibly just-refuted) incarnation
                break;
            }
            case ControlKind::Ack: {
                apply_update(m.from, m.from_incarnation, MemberStatus::Alive, Src::Direct, now);
                if (m.subject_incarnation == 0) {  // reply to OUR probe (or a seq-0 verify/probe-back)
                    if (probe_.active && probe_.target == m.from && m.seq == probe_.seq) probe_.acked = true;
                } else {  // reply to a Ping we forwarded as a relay: authenticate, then report back
                    const std::int64_t b = bucket(now);
                    const NodeId requester = m.subject;
                    if (requester != self_ &&
                        (m.subject_incarnation == relay_token(requester, m.from, m.seq, b) ||
                         m.subject_incarnation == relay_token(requester, m.from, m.seq, b - 1))) {
                        ControlMsg rep;
                        rep.kind = ControlKind::PingReqAck;
                        rep.subject = m.from;
                        rep.subject_incarnation = m.from_incarnation;
                        rep.seq = m.seq;
                        send_control(requester, rep);
                    }
                }
                break;
            }
            case ControlKind::PingReq: {
                // Relay: ping `subject` on the requester's behalf. Nothing is remembered — the subject's
                // Ack may arrive on any later on_frame and carries the correlation back (relay_token).
                ControlMsg ping;
                ping.kind = ControlKind::Ping;
                ping.seq = m.seq;
                ping.subject = m.from;
                ping.subject_incarnation = relay_token(m.from, m.subject, m.seq, bucket(now));
                send_control(m.subject, ping);
                break;
            }
            case ControlKind::PingReqAck: {
                if (probe_.active && probe_.target == m.subject && m.seq == probe_.seq)
                    probe_.acked = true;
                apply_update(m.subject, m.subject_incarnation, MemberStatus::Alive, Src::Gossip, now);
                break;
            }
            case ControlKind::Gossip:
                break;  // digest already merged above
            case ControlKind::Join: {
                ControlMsg reply;
                if (m.cluster != cfg_.cluster_id) {
                    reply.kind = ControlKind::JoinReject;  // §1 reject: wrong cluster id
                } else if (m.subject != m.from) {
                    return;  // a node may only (re)admit itself
                } else {
                    // Re-admit above any tombstone (ADR-048): max(presented, tombstone + 1). The
                    // assigned incarnation rides back in JoinAck so the joiner adopts it.
                    std::uint64_t inc = m.subject_incarnation;
                    if (auto it = members_.find(m.subject.value);
                        it != members_.end() && is_tomb(it->second.status))
                        inc = std::max(inc, it->second.incarnation + 1);
                    apply_update(m.subject, inc, MemberStatus::Alive, Src::Join, now);  // epoch++
                    reply.kind = ControlKind::JoinAck;  // snapshot rides fill_digest()
                    reply.subject_incarnation = incarnation_of(m.subject);
                }
                reply.subject = m.subject;
                send_control(m.from, reply);
                break;
            }
            case ControlKind::JoinAck: {
                // Joiner side: adopt the incarnation the seed assigned, and take the seed's Alive
                // entries from the snapshot (a Dead we hold for one of them is re-tested, not trusted).
                if (m.subject == self_ && !self_left_ && m.subject_incarnation > self_incarnation_) {
                    self_incarnation_ = m.subject_incarnation;
                    members_[self_.value].incarnation = self_incarnation_;
                }
                for (const MemberUpdate& u : m.updates)
                    if (static_cast<MemberStatus>(u.status) == MemberStatus::Alive)
                        apply_update(NodeId{u.node}, u.incarnation, MemberStatus::Alive, Src::JoinSnapshot, now);
                break;
            }
            case ControlKind::JoinReject:
                break;  // joiner side: mismatch observed
            case ControlKind::Congested: {
                // C1 epoch gen-gate (proven): a stale pre-restart signal never advances the throttle —
                // mirrors apply_update's Alive-branch precedence (strictly-not-older incarnation wins).
                // `transport_` is the SAME shared instance DistributedRouter sends through (one socket,
                // one connection per peer, unaffected — 021), so this reaches the real admission gate.
                if (m.from_incarnation >= incarnation_of(m.from))
                    transport_->mark_congested(m.from, m.congestion_remaining_ns);
                break;
            }
        }
    }

    // --- probe FSM ----------------------------------------------------------------------------
    void drive_probe(std::int64_t now) {
        if (!probe_.active) return;
        if (probe_.acked) {  // a live peer answered (direct or via relay) ⇒ resolved, no escalation
            probe_.active = false;
            return;
        }
        // Target tombstoned or erased meanwhile: abandon (a Suspect here would re-create it).
        if (auto it = members_.find(probe_.target.value); it == members_.end() || is_tomb(it->second.status)) {
            probe_.active = false;
            return;
        }
        if (now < probe_.deadline_ns) return;  // still within the ack window
        if (probe_.phase == Phase::Direct) {
            // Missed direct ack → indirect ping-req via k random relays BEFORE any suspicion (SWIM's
            // core false-positive reducer). A relay that reaches the target reports PingReqAck on a
            // later frame (asynchronously over a real transport; inline over the loopback).
            probe_.phase = Phase::Indirect;
            probe_.deadline_ns = now + indirect_ns();
            send_indirect(probe_.target, probe_.seq, now);
            if (probe_.acked) probe_.active = false;  // a relay already vouched for it
        } else {
            // Missed indirect ack too → declare Suspect (not yet Dead). Refutation or a later ack can
            // still rescue it before suspicion_timeout.
            apply_update(probe_.target, incarnation_of(probe_.target), MemberStatus::Suspect, Src::Local, now);
            probe_.active = false;
        }
    }

    void start_probe(std::int64_t now) {
        if (probe_.active) return;
        const std::optional<NodeId> target = next_probe_target();
        if (!target) return;
        probe_ = Probe{};
        probe_.active = true;
        probe_.phase = Phase::Direct;
        probe_.target = *target;
        probe_.deadline_ns = now + cfg_.ack_timeout_ns;
        probe_.seq = ++probe_seq_;
        probe_.acked = false;
        ControlMsg ping;
        ping.kind = ControlKind::Ping;
        ping.seq = probe_.seq;
        send_control(*target, ping);  // a live target's Ack lands inline (loopback) ⇒ acked next tick
        if (probe_.acked) probe_.active = false;  // resolved immediately if the ack came back inline
    }

    void send_indirect(NodeId target, std::uint64_t seq, std::int64_t /*now*/) {
        std::uint32_t sent = 0;
        std::uint64_t r = mix(self_.value, seq, 0x51D3);
        std::vector<NodeId> relays = live_peers_excluding(target);
        for (std::uint32_t i = 0; i < relays.size() && sent < cfg_.indirect_k; ++i) {
            const NodeId relay = relays[(r + i) % relays.size()];
            ControlMsg req;
            req.kind = ControlKind::PingReq;
            req.subject = target;
            req.seq = seq;
            send_control(relay, req);
            ++sent;
            if (probe_.acked) break;  // a relay already reported success
        }
    }

    // R1b: one direct verification Ping per second-hand Suspect, at most verify_ping_budget per tick.
    // The pending flag is the queue — no allocation beyond the per-tick target list.
    void send_verifies() {
        if (cfg_.verify_ping_budget == 0) return;
        std::vector<NodeId> targets;
        for (auto& [id, m] : members_) {
            if (targets.size() >= cfg_.verify_ping_budget) break;
            if (!m.verify_pending) continue;
            m.verify_pending = false;
            if (m.status == MemberStatus::Suspect && id != self_.value) targets.push_back(NodeId{id});
        }
        for (NodeId t : targets) {  // ids collected first: inline (loopback) delivery may mutate members_
            ControlMsg ping;
            ping.kind = ControlKind::Ping;
            ping.seq = 0;  // probe seqs start at 1, so this can never satisfy a probe
            send_control(t, ping);
        }
    }

    // R5': up to tombstone_probe_budget probe-backs per tick to Dead (not Left) tombstones, least
    // recently probed first, each at least tombstone_probe_interval_ns apart. The Ping's digest leads
    // with the receiver's own entry — "you are Dead@inc" — so a healed node refutes at inc+1.
    void probe_tombstones(std::int64_t now) {
        if (tomb_count_ == 0 || cfg_.tombstone_probe_budget == 0) return;
        std::vector<std::pair<std::int64_t, std::uint64_t>> cand;
        for (const auto& [id, m] : members_) {
            if (m.status != MemberStatus::Dead || id == self_.value) continue;
            if (m.last_probe_ns != kNever && now - m.last_probe_ns < cfg_.tombstone_probe_interval_ns) continue;
            cand.emplace_back(m.last_probe_ns, id);
        }
        if (cand.empty()) return;
        const std::size_t k = std::min<std::size_t>(cand.size(), cfg_.tombstone_probe_budget);
        std::partial_sort(cand.begin(), cand.begin() + static_cast<std::ptrdiff_t>(k), cand.end());
        cand.resize(k);
        for (const auto& [lp, id] : cand) members_.find(id)->second.last_probe_ns = now;
        for (const auto& [lp, id] : cand) {
            ControlMsg ping;
            ping.kind = ControlKind::Ping;
            ping.seq = 0;
            send_control(NodeId{id}, ping);
        }
    }

    void gossip_round(std::int64_t now) {
        std::vector<NodeId> peers = live_peers_excluding(self_);
        if (peers.empty()) return;
        std::uint64_t r = mix(self_.value, static_cast<std::uint64_t>(now), 0xF00D);
        for (std::uint32_t i = 0; i < peers.size() && i < cfg_.gossip_fanout; ++i) {
            const NodeId p = peers[(r + i) % peers.size()];
            ControlMsg g;
            g.kind = ControlKind::Gossip;
            send_control(p, g);
        }
    }

    // ADR-046: watermark-crossing detection (hysteresis: a new edge cannot re-fire until resident_total
    // has fallen back below the LOW watermark) plus a periodic refresh while congestion is sustained —
    // a node stuck above the high watermark for longer than one `congestion_window_ns` re-broadcasts
    // every `congestion_refresh_ns` so the sender's own TTL-based recovery never fires prematurely.
    // Edge-triggered, not level-triggered: exactly one broadcast per rising edge, then refreshes only.
    void monitor_congestion(std::int64_t now) {
        if (!pressure_query_) return;
        const std::uint64_t total = pressure_query_();
        if (total >= cfg_.congestion_high_watermark) {
            if (!congested_edge_active_ || now >= next_congestion_refresh_ns_) {
                congested_edge_active_ = true;
                next_congestion_refresh_ns_ = now + cfg_.congestion_refresh_ns;
                broadcast_congested(cfg_.congestion_window_ns);
            }
        } else if (total < cfg_.congestion_low_watermark) {
            congested_edge_active_ = false;  // may re-fire on the NEXT rising edge past the high mark
        }
    }

    // Fan out a `Congested` signal to every direct, live peer (021: direct-connection-only — not
    // gossip-relayed the way capability digests are, a named scope limit, not an oversight).
    void broadcast_congested(std::int64_t remaining_ns) {
        for (NodeId peer : live_peers_excluding(self_)) {
            ControlMsg m;
            m.kind = ControlKind::Congested;
            m.congestion_remaining_ns = remaining_ns;
            send_control(peer, m);
        }
    }

    // --- merge / state transitions ------------------------------------------------------------
    // The SWIM precedence + refutation core (ADR-048 R1–R3, R6', R7). Applies (node, incarnation,
    // status) iff it supersedes what we know. Refutation: a Suspect/Dead of OURSELVES with
    // incarnation ≥ ours makes us re-broadcast Alive at a strictly higher incarnation (higher
    // incarnation wins ⇒ the false suspicion loses). Tombstone rules:
    //   * a Dead/Left is never CREATED second-hand — only by this node (Local);
    //   * a second-hand Dead about a live peer is downgraded to Suspect and verified directly (R1/R1b),
    //     unless we heard from that peer within suspicion_timeout at the same incarnation (R2 veto);
    //   * a tombstone is lifted only by a strictly higher incarnation (R3) or a Join;
    //   * a Left is authoritative and is never refuted by its own node.
    void apply_update(NodeId node, std::uint64_t inc, MemberStatus st, Src src, std::int64_t now) {
        if (static_cast<std::uint8_t>(st) > static_cast<std::uint8_t>(MemberStatus::Left)) return;  // wire range
        if (node == self_) {
            if (self_left_ || st == MemberStatus::Left) return;  // a departure is never refuted
            if (st != MemberStatus::Alive && inc >= self_incarnation_) {
                self_incarnation_ = inc + 1;  // refute: strictly higher incarnation wins
                Member& me = members_[self_.value];
                me.incarnation = self_incarnation_;
                me.status = MemberStatus::Alive;
            }
            return;  // we never mark ourselves Suspect/Dead
        }

        auto it = members_.find(node.value);
        if (it == members_.end()) {
            if (is_tomb(st) && src != Src::Local) return;  // never create a tombstone second-hand
            if (st == MemberStatus::Suspect) return;       // never create an unknown node as Suspect
            // Verification needs positive evidence of the node: a Direct/Join Alive. A LOCAL
            // Suspect/Dead is only our own failed probe — it must never make a ghost placeable.
            const bool first_hand = (src == Src::Direct || src == Src::Join) ||
                                    (src == Src::Local && st == MemberStatus::Alive);
            if (!first_hand && members_.size() >= cfg_.max_live_members) return;  // ghost-flood cap
            Member m{};
            m.cluster = cfg_.cluster_id;
            m.incarnation = inc;
            m.status = st;
            m.verified = first_hand;
            m.first_seen_ns = now;
            set_tx(m, m.verified ? piggyback_count() : 0);  // do not re-gossip unverified ghosts
            if (is_tomb(st)) {
                m.death_ns = now;
                m.last_probe_ns = now;
                ++tomb_count_;
            }
            members_.emplace(node.value, m);
            ids_dirty_ = true;
            // The convergence view includes every live member (verified or not); the separate
            // placement_view() filters unverified ones. Republish for either kind.
            if (!is_tomb(st)) republish();
            enforce_tomb_cap();
            return;
        }

        Member& cur = it->second;
        const bool was_live = !is_tomb(cur.status);
        const bool was_verified = cur.verified;
        if (src == Src::Direct || src == Src::Join ||
            (src == Src::Local && st == MemberStatus::Alive))
            cur.verified = true;
        MemberStatus next = st;
        bool take = false;
        switch (st) {
            case MemberStatus::Alive:
                if (src == Src::JoinSnapshot && cur.status == MemberStatus::Dead && inc >= cur.incarnation) {
                    next = MemberStatus::Suspect;  // the seed says alive: re-test, don't trust blindly
                    take = true;
                } else {
                    take = inc > cur.incarnation;  // a tombstone needs STRICTLY higher (R3)
                }
                break;
            case MemberStatus::Suspect:
                take = inc > cur.incarnation || (inc == cur.incarnation && cur.status == MemberStatus::Alive);
                break;
            case MemberStatus::Dead:
                if (src == Src::Local) {
                    take = cur.status != MemberStatus::Left && inc >= cur.incarnation;
                    break;
                }
                if (!was_live) break;  // a second-hand Dead never touches a tombstone
                if (inc == cur.incarnation && cur.status == MemberStatus::Alive &&
                    cur.last_direct_ns != kNever && now - cur.last_direct_ns < cfg_.suspicion_timeout_ns)
                    break;  // R2: we heard from it directly, recently — veto the accusation
                next = MemberStatus::Suspect;  // R1: downgrade, re-derive locally
                take = inc > cur.incarnation || (inc == cur.incarnation && cur.status == MemberStatus::Alive);
                break;
            case MemberStatus::Left:
                take = cur.status == MemberStatus::Left ? inc > cur.incarnation : inc >= cur.incarnation;
                break;
        }
        if (!take) {
            // A first-hand frame can verify a member without changing its incarnation/status (e.g.
            // an Ack at the same incarnation as the gossip). Publish that verification.
            if (!was_verified && cur.verified) {
                set_tx(cur, piggyback_count());
                republish();
            }
            return;
        }
        if (cur.incarnation != inc || cur.status != next) set_tx(cur, piggyback_count());
        cur.incarnation = inc;
        cur.status = next;
        cur.verify_pending = false;
        if (next == MemberStatus::Suspect) {
            cur.suspect_deadline_ns = now + cfg_.suspicion_timeout_ns;
            cur.verify_pending = (src == Src::Gossip || src == Src::JoinSnapshot);  // R1b
        }
        const bool now_live = !is_tomb(next);
        if (!now_live) {
            cur.death_ns = now;
            if (was_live) cur.last_probe_ns = now;
        }
        if (was_live && !now_live) ++tomb_count_;
        if (!was_live && now_live) --tomb_count_;
        // Roster (non-tombstone set) membership changed? join (new live) or dead (live→dead) ⇒ epoch++.
        if (was_live != now_live) republish();
        if (!was_verified && cur.verified) {  // now eligible for placement + gossip
            set_tx(cur, piggyback_count());
            republish();
        }
        enforce_tomb_cap();  // may erase entries: `cur` is NOT touched after this line
    }

    // Suspect → Dead once the suspicion timeout lapses with no refutation — the ONLY producer of an
    // inferred Dead (gossip never writes one).
    void reap_suspects(std::int64_t now) {
        bool changed = false;
        const std::uint8_t tx = piggyback_count();
        for (auto& [id, m] : members_) {
            if (m.status == MemberStatus::Suspect && now >= m.suspect_deadline_ns) {
                m.status = MemberStatus::Dead;
                m.death_ns = now;
                m.last_probe_ns = now;
                set_tx(m, tx);
                m.verify_pending = false;
                ++tomb_count_;
                changed = true;
            }
        }
        if (changed) republish();
        enforce_tomb_cap();
    }

    // R7: erase tombstones older than tombstone_ttl_ns (injected clock). After this a stale Alive can
    // re-admit the node, so the TTL must exceed the longest partition the deployment tolerates.
    void reap_tombstones(std::int64_t now) {
        if (tomb_count_ == 0) return;
        const std::uint64_t self_id = self_.value;
        const std::size_t erased = std::erase_if(members_, [&](const auto& kv) {
            const Member& m = kv.second;
            const bool expired = kv.first != self_id && is_tomb(m.status) && now - m.death_ns >= cfg_.tombstone_ttl_ns;
            if (expired && m.tx_left) --tx_pending_;
            return expired;
        });
        if (erased) {
            tomb_count_ -= static_cast<std::uint32_t>(erased);
            ids_dirty_ = true;
        }
    }

    // P2: erase member-table entries learned only from gossip that never produced first-hand
    // evidence within the TTL. They remain absent from placement anyway; this bounds the convergence
    // view and the probe/digest work a ghost flood can create.
    void reap_unverified(std::int64_t now) {
        if (cfg_.unverified_ttl_ns <= 0) return;
        const std::size_t erased = std::erase_if(members_, [&](const auto& kv) {
            const Member& m = kv.second;
            return kv.first != self_.value && !m.verified && !is_tomb(m.status) &&
                   now - m.first_seen_ns >= cfg_.unverified_ttl_ns;
        });
        if (erased == 0) return;
        ids_dirty_ = true;
        republish();
    }

    // Hard cap on tombstones: evict Dead before Left, oldest death first (ties by lower id).
    void enforce_tomb_cap() {
        while (tomb_count_ > cfg_.max_tombstones) {
            auto victim = members_.end();
            for (auto it = members_.begin(); it != members_.end(); ++it) {
                if (it->first == self_.value || !is_tomb(it->second.status)) continue;
                if (victim == members_.end()) {
                    victim = it;
                    continue;
                }
                const bool victim_dead = victim->second.status == MemberStatus::Dead;
                const bool it_dead = it->second.status == MemberStatus::Dead;
                if (it_dead != victim_dead) {
                    if (it_dead) victim = it;
                    continue;
                }
                if (it->second.death_ns < victim->second.death_ns ||
                    (it->second.death_ns == victim->second.death_ns && it->first < victim->first))
                    victim = it;
            }
            if (victim == members_.end()) break;
            if (victim->second.tx_left) --tx_pending_;
            members_.erase(victim);
            --tomb_count_;
            ids_dirty_ = true;
        }
    }

    // --- peer selection (deterministic; seeded splitmix, never wall time / random_device) --------
    [[nodiscard]] std::vector<NodeId> live_peers_excluding(NodeId excl) const {
        std::vector<NodeId> v;
        for (const auto& [id, m] : members_) {
            const NodeId n{id};
            if (n == self_ || n == excl) continue;
            if (is_tomb(m.status)) continue;
            v.push_back(n);
        }
        std::sort(v.begin(), v.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        return v;
    }

    // Shuffled round-robin over the live peers: every peer is probed once per round before any repeat
    // (SWIM's time-bounded-detection property), the per-round order seeded so it is deterministic but
    // not a fixed bias. Rebuilds when the round is exhausted; peers tombstoned or erased since the
    // round was built are skipped (one rebuild at most per call).
    using ControlBudget = detail::RateBucket;

    [[nodiscard]] bool admit_control(NodeId from, std::int64_t now) noexcept {
        if (cfg_.control_rate_per_sec == 0) return true;
        const std::size_t idx = static_cast<std::size_t>(
            (from.value * 0x9E3779B97F4A7C15ULL) >> 56) & 255;
        return control_budgets_[idx].admit(now, cfg_.control_rate_per_sec, cfg_.control_burst);
    }

    [[nodiscard]] std::optional<NodeId> next_probe_target() {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (probe_order_idx_ >= probe_order_.size()) {
                probe_order_ = live_peers_excluding(self_);
                // seed-shuffle (Fisher–Yates with a seeded splitmix stream)
                std::uint64_t r = mix(cfg_.seed, self_.value, probe_round_++);
                for (std::size_t i = probe_order_.size(); i > 1; --i) {
                    r = detail::splitmix64(r);
                    const std::size_t j = static_cast<std::size_t>(r % i);
                    std::swap(probe_order_[i - 1], probe_order_[j]);
                }
                probe_order_idx_ = 0;
            }
            while (probe_order_idx_ < probe_order_.size()) {
                const NodeId n = probe_order_[probe_order_idx_++];
                const auto it = members_.find(n.value);
                if (it != members_.end() && !is_tomb(it->second.status)) return n;
            }
        }
        return std::nullopt;
    }

    static std::uint64_t mix(std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
        return detail::splitmix64(detail::splitmix64(a ^ (b + 0x9E3779B97F4A7C15ULL)) ^ c);
    }

    // Publish an immutable snapshot of the current live (non-tombstone) node set and bump the epoch.
    void republish() {
        std::vector<NodeId> live;
        std::vector<NodeId> verified;
        for (const auto& [id, m] : members_) {
            if (is_tomb(m.status)) continue;
            live.push_back(NodeId{id});
            if (m.verified || id == self_.value) verified.push_back(NodeId{id});
        }
        const auto by_id = [](NodeId a, NodeId b) { return a.value < b.value; };
        std::sort(live.begin(), live.end(), by_id);
        std::sort(verified.begin(), verified.end(), by_id);
        std::lock_guard<std::mutex> g(mu_);
        snapshot_ = std::make_shared<const std::vector<NodeId>>(std::move(live));
        verified_snapshot_ = std::make_shared<const std::vector<NodeId>>(std::move(verified));
        ++epoch_;
    }

    NodeId self_;
    Transport* transport_;
    Config cfg_;
    std::uint64_t self_incarnation_ = 1;

    std::unordered_map<std::uint64_t, Member> members_;  // node.value → Member (protocol-thread owned)
    Probe probe_{};
    std::uint64_t probe_seq_ = 0;
    std::vector<NodeId> probe_order_;
    std::size_t probe_order_idx_ = 0;
    std::uint64_t probe_round_ = 0;

    // ADR-048 protocol state (protocol-thread owned). The relay itself keeps no per-request state.
    std::uint64_t relay_key_ = 0;             // keys relay_token()
    std::uint64_t digest_cursor_ = 0;         // last id the rotating digest cursor sent
    std::vector<std::uint64_t> sorted_ids_;   // cursor's sorted roster, rebuilt only when ids_dirty_
    bool ids_dirty_ = true;
    std::uint32_t tomb_count_ = 0;            // Dead + Left entries, excluding self
    std::uint32_t tx_pending_ = 0;            // entries with tx_left > 0 (see set_tx)
    bool self_left_ = false;

    std::function<void(MessageFrame)> data_sink_;
    std::function<bool(NodeId)> link_reachable_;
    bool online_ = true;

    RevocationPullFn revocation_pull_;
    RevocationMergeFn revocation_merge_;
    CapabilityPullFn capability_pull_;
    CapabilityMergeFn capability_merge_;
    SweepHookFn sweep_hook_;

    PressureQueryFn pressure_query_;
    PressureMergeFn pressure_merge_;
    std::unordered_map<std::uint64_t, std::uint64_t> last_pressure_;  // node.value → last-advertised total
    bool congested_edge_active_ = false;
    std::int64_t next_congestion_refresh_ns_ = 0;

    ClockFn clock_fn_ = &real_steady_ns;
    void* clock_ctx_ = nullptr;

    mutable std::mutex mu_;
    std::shared_ptr<const std::vector<NodeId>> snapshot_;
    std::shared_ptr<const std::vector<NodeId>> verified_snapshot_;
    std::array<ControlBudget, 256> control_budgets_{};
    std::atomic<std::uint64_t> control_sheds_{0};
    std::uint64_t epoch_ = 0;
};

}  // namespace quark
