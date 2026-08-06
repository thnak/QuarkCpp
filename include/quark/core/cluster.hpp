// Implements 021-Cluster-Formation-and-Lifecycle §1 (cluster identity + epoch), §2 (Discovery seam,
// dial-dedup), §3 (SWIM failure detection, incarnation refutation, gossip, stabilization / anti-flap).
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
enum class MemberStatus : std::uint8_t { Alive = 0, Suspect = 1, Dead = 2 };

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
        std::int64_t ack_timeout_ns = 200'000'000;         // direct/indirect ack deadline
        std::int64_t suspicion_timeout_ns = 1'000'000'000;  // Suspect → Dead with no refutation
        std::uint32_t indirect_k = 2;                        // relay peers for a ping-req
        std::uint32_t gossip_fanout = 3;                     // peers a gossip round targets
        std::uint32_t max_gossip_updates = 32;               // bounded piggyback digest
        std::uint64_t max_capability_gossip_bytes = 4096;    // ADR-045: bounded capability piggyback
        std::uint64_t seed = 0;                              // deterministic peer/fanout selection
        // ADR-046: cross-node backpressure watermarks (resident-message-count hysteresis) and the
        // Congested signal's TTL. Unvalidated-at-real-workload-scale operational config (ADR's own
        // disclosed residual risk) — tune per deployment, not derived from anything else here.
        std::uint64_t congestion_high_watermark = 10'000;   // resident_total crossing ⇒ broadcast
        std::uint64_t congestion_low_watermark = 5'000;      // must fall below this before a new edge
        std::int64_t congestion_window_ns = 2'000'000'000;   // 2s — TTL stamped on a Congested frame
        std::int64_t congestion_refresh_ns = 500'000'000;    // 500ms — re-broadcast while still congested
    };

    SwimMembership(NodeId self, Transport& transport, Config cfg)
        : self_(self), transport_(&transport), cfg_(cfg) {
        members_[self_.value] = Member{cfg.cluster_id, self_incarnation_, MemberStatus::Alive, 0};
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
    // and advances the epoch on admission; false on cluster-id mismatch. Unit-testable directly. -----
    [[nodiscard]] bool admit(NodeId joiner, ClusterId presented) {
        if (presented != cfg_.cluster_id) return false;  // §1 reject: wrong cluster
        apply_update(joiner, 0, MemberStatus::Alive);     // new Alive member ⇒ roster grows, epoch++
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
    void leave(NodeId who) { apply_update(who, self_incarnation_ + 1, MemberStatus::Dead); }

    // --- The SWIM protocol period (021 §3). One call = one protocol tick. Reads the injected clock; a
    // test advances virtual time between ticks to drive ping→miss→indirect→suspect→dead deterministic-
    // ally. NO sleeping, NO wall-clock wait. ------------------------------------------------------
    void tick() {
        if (!online_) return;
        const std::int64_t now = now_ns();
        reap_suspects(now);   // Suspect → Dead once the suspicion timeout lapses with no refutation
        drive_probe(now);     // resolve/escalate the in-flight probe by virtual time
        start_probe(now);     // begin a new direct probe of the next peer
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

private:
    struct Member {
        ClusterId cluster{};
        std::uint64_t incarnation = 0;
        MemberStatus status = MemberStatus::Alive;
        std::int64_t suspect_deadline_ns = 0;  // when Suspect becomes Dead
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
        fill_digest(m.updates);
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

    // Bounded gossip digest: the current member table (small for the target scale here). Real SWIM
    // gossips a decaying recent-update list; full-table digest converges cleanly for small clusters and
    // the decay/priority list is a documented optimization (026 path-change boundary).
    void fill_digest(std::vector<MemberUpdate>& out) {
        out.clear();
        for (const auto& [id, m] : members_) {
            if (out.size() >= cfg_.max_gossip_updates) break;
            out.push_back(MemberUpdate{id, m.incarnation, static_cast<std::uint8_t>(m.status)});
        }
    }

    // --- inbound ------------------------------------------------------------------------------
    void on_frame(MessageFrame f) {
        if (f.kind != FrameKind::Control) {
            if (data_sink_) data_sink_(std::move(f));  // 010 data plane, untouched
            return;
        }
        if (!online_) return;  // crashed node ignores control (stops acking)
        ControlMsg m;
        if (!detail::decode_control(f.payload.data(), f.payload.size(), m)) return;  // malformed → drop
        handle_control(m);
    }

    void handle_control(const ControlMsg& m) {
        // Join is the ONE control that legitimately crosses a cluster-id boundary (it is how a mismatch
        // is detected). Every other control from a foreign cluster is dropped (accidental-merge guard).
        if (m.kind != ControlKind::Join && m.cluster != cfg_.cluster_id) return;

        // Absorb piggybacked gossip — but ONLY from a same-cluster frame, so a mismatched Join cannot
        // smuggle its roster in via the digest and defeat the accidental-merge guard below.
        if (m.cluster == cfg_.cluster_id) {
            merge_digest(m.updates);  // may trigger self-refutation
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
                send_control(m.from, ack);  // carries our (possibly just-refuted) incarnation
                break;
            }
            case ControlKind::Ack: {
                if (probe_.active && probe_.target == m.from && m.seq == probe_.seq) probe_.acked = true;
                // If we are mid-relay (handling a PingReq for someone else), THIS ack from the subject
                // is what we forward back as a PingReqAck — record it for the enclosing PingReq handler.
                if (relay_active_ && m.from == relay_subject_ && m.seq == relay_seq_)
                    relay_got_ack_ = true;
                apply_update(m.from, m.from_incarnation, MemberStatus::Alive);
                break;
            }
            case ControlKind::PingReq: {
                // Relay: ping `subject` on the requester's behalf; if it acks (synchronously over the
                // loopback), report back with PingReqAck. Correlate the requester+seq for the report.
                relay_active_ = true;
                relay_requester_ = m.from;
                relay_seq_ = m.seq;
                relay_subject_ = m.subject;
                relay_got_ack_ = false;
                ControlMsg ping;
                ping.kind = ControlKind::Ping;
                ping.seq = m.seq;
                send_control(m.subject, ping);  // subject's Ack lands in this same call stack (loopback)
                if (relay_got_ack_) {
                    ControlMsg rep;
                    rep.kind = ControlKind::PingReqAck;
                    rep.subject = m.subject;
                    rep.subject_incarnation = incarnation_of(m.subject);
                    rep.seq = m.seq;
                    send_control(m.from, rep);
                }
                relay_active_ = false;
                break;
            }
            case ControlKind::PingReqAck: {
                if (probe_.active && probe_.target == m.subject && m.seq == probe_.seq)
                    probe_.acked = true;
                apply_update(m.subject, m.subject_incarnation, MemberStatus::Alive);
                break;
            }
            case ControlKind::Gossip:
                break;  // digest already merged above
            case ControlKind::Join: {
                ControlMsg reply;
                if (m.cluster != cfg_.cluster_id) {
                    reply.kind = ControlKind::JoinReject;  // §1 reject: wrong cluster id
                } else {
                    apply_update(m.subject, 0, MemberStatus::Alive);  // admit ⇒ roster grows, epoch++
                    reply.kind = ControlKind::JoinAck;                // snapshot rides fill_digest()
                }
                reply.subject = m.subject;
                send_control(m.from, reply);
                break;
            }
            case ControlKind::JoinAck:
            case ControlKind::JoinReject:
                break;  // joiner side: snapshot already merged (Ack) / mismatch observed (Reject)
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
        if (now < probe_.deadline_ns) return;  // still within the ack window
        if (probe_.phase == Phase::Direct) {
            // Missed direct ack → indirect ping-req via k random relays BEFORE any suspicion (SWIM's
            // core false-positive reducer). A relay that reaches the target reports PingReqAck, which
            // sets probe_.acked in the same call stack over the loopback.
            probe_.phase = Phase::Indirect;
            probe_.deadline_ns = now + cfg_.ack_timeout_ns;
            send_indirect(probe_.target, probe_.seq, now);
            if (probe_.acked) probe_.active = false;  // a relay already vouched for it
        } else {
            // Missed indirect ack too → declare Suspect (not yet Dead). Refutation or a later ack can
            // still rescue it before suspicion_timeout.
            apply_update(probe_.target, incarnation_of(probe_.target), MemberStatus::Suspect);
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
    void merge_digest(const std::vector<MemberUpdate>& updates) {
        for (const MemberUpdate& u : updates)
            apply_update(NodeId{u.node}, u.incarnation, static_cast<MemberStatus>(u.status));
    }

    // The SWIM precedence + refutation core. Applies (node, incarnation, status) iff it supersedes what
    // we know. Refutation: a Suspect/Dead of OURSELVES with incarnation ≥ ours makes us re-broadcast
    // Alive at a strictly higher incarnation (higher incarnation wins ⇒ the false suspicion loses).
    void apply_update(NodeId node, std::uint64_t incarnation, MemberStatus status) {
        if (node == self_) {
            if (status != MemberStatus::Alive && incarnation >= self_incarnation_) {
                self_incarnation_ = incarnation + 1;  // refute: strictly higher incarnation wins
                members_[self_.value].incarnation = self_incarnation_;
                members_[self_.value].status = MemberStatus::Alive;
            }
            return;  // we never mark ourselves Suspect/Dead
        }

        const auto it = members_.find(node.value);
        const bool known = it != members_.end();
        if (known && it->second.status == MemberStatus::Dead) return;  // Dead is terminal (rejoin only)

        bool supersedes;
        if (!known) {
            supersedes = true;  // first we hear of this node
        } else {
            const Member& cur = it->second;
            switch (status) {
                case MemberStatus::Alive:
                    supersedes = incarnation > cur.incarnation;
                    break;
                case MemberStatus::Suspect:
                    supersedes = incarnation > cur.incarnation ||
                                 (incarnation == cur.incarnation && cur.status == MemberStatus::Alive);
                    break;
                case MemberStatus::Dead:
                    supersedes = incarnation >= cur.incarnation;
                    break;
                default:
                    supersedes = false;
                    break;
            }
        }
        if (!supersedes) return;

        const bool was_live = known && it->second.status != MemberStatus::Dead;
        Member& m = members_[node.value];
        m.cluster = cfg_.cluster_id;
        m.incarnation = incarnation;
        m.status = status;
        if (status == MemberStatus::Suspect)
            m.suspect_deadline_ns = now_ns() + cfg_.suspicion_timeout_ns;

        // Roster (non-Dead set) membership changed? join (new live) or dead (live→dead) ⇒ epoch++.
        const bool now_live = status != MemberStatus::Dead;
        if (!known || was_live != now_live) republish();
    }

    void reap_suspects(std::int64_t now) {
        bool changed = false;
        for (auto& [id, m] : members_) {
            if (m.status == MemberStatus::Suspect && now >= m.suspect_deadline_ns) {
                m.status = MemberStatus::Dead;  // suspicion timeout lapsed with no refutation
                changed = true;
            }
        }
        if (changed) republish();
    }

    // --- peer selection (deterministic; seeded splitmix, never wall time / random_device) --------
    [[nodiscard]] std::vector<NodeId> live_peers_excluding(NodeId excl) const {
        std::vector<NodeId> v;
        for (const auto& [id, m] : members_) {
            const NodeId n{id};
            if (n == self_ || n == excl) continue;
            if (m.status == MemberStatus::Dead) continue;
            v.push_back(n);
        }
        std::sort(v.begin(), v.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        return v;
    }

    // Shuffled round-robin over the live peers: every peer is probed once per round before any repeat
    // (SWIM's time-bounded-detection property), the per-round order seeded so it is deterministic but
    // not a fixed bias. Rebuilds when the round is exhausted or the live set changed.
    [[nodiscard]] std::optional<NodeId> next_probe_target() {
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
        if (probe_order_.empty()) return std::nullopt;
        return probe_order_[probe_order_idx_++];
    }

    static std::uint64_t mix(std::uint64_t a, std::uint64_t b, std::uint64_t c) noexcept {
        return detail::splitmix64(detail::splitmix64(a ^ (b + 0x9E3779B97F4A7C15ULL)) ^ c);
    }

    // Publish an immutable snapshot of the current live (non-Dead) node set and bump the epoch.
    void republish() {
        std::vector<NodeId> live;
        for (const auto& [id, m] : members_)
            if (m.status != MemberStatus::Dead) live.push_back(NodeId{id});
        std::sort(live.begin(), live.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        std::lock_guard<std::mutex> g(mu_);
        snapshot_ = std::make_shared<const std::vector<NodeId>>(std::move(live));
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

    // Relay correlation for an in-flight PingReq (subject's Ack sets the flag in the same call stack).
    bool relay_active_ = false;
    NodeId relay_requester_{};
    NodeId relay_subject_{};
    std::uint64_t relay_seq_ = 0;
    bool relay_got_ack_ = false;

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
    std::uint64_t epoch_ = 0;
};

}  // namespace quark
