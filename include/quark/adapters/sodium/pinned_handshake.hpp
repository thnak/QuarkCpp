// Implements 020-Security §"Pinned-key mode (Noise IK, #40)", 021 §"HELLO and dial dedup in pinned mode" and ADR-068 —
// `PinnedHandshakeEngine` / `PinnedHandshakeEngineFactory`, the Noise_IK_25519_ChaChaPoly_BLAKE2b handshake behind the
// `HandshakeEngine` seam (handshake.hpp), the second mode beside the mbedTLS CA chain. SecureTransport drives it exactly
// as it drives the TLS engine; the seam is unchanged.
//
// WIRE (both messages are FIXED length, so a capture shows nothing but fixed lengths and bytes with no identifier in them):
//   Msg1  initiator -> responder  (154 bytes)   -> e, es, s, ss     payload (58 B, encrypted):
//           u16 version | u64 cluster/scope id | u64 initiator NodeId | u64 timestamp (unix ns) | 32 B Ed25519 public key
//   Msg2  responder -> initiator  (58 bytes)    <- e, ee, se        payload (10 B, encrypted):
//           u16 version | u64 responder NodeId
//   There is NO cleartext HELLO in this mode: the version and the NodeId used for dial dedup travel in these payloads,
//   encrypted, so a device reveals no stable identifier, scope id or NodeId to anyone who has not proven it is admitted.
//   Prologue = "quark-pinned-key/v1" || TrustMode::PinnedKey (0x01): a CaChain peer and a PinnedKey peer cannot complete
//   a handshake with each other (no downgrade, nothing to negotiate).
//
// SILENT REFUSAL: a responder that cannot authenticate and admit the initiator sends NOTHING (advance() returns Failed
// with `out` untouched) and the caller closes the connection. That covers: Msg1 does not decrypt (a prober without our
// static key), a wrong length, an initiator that is not admitted, a revoked key, a wrong cluster, an out-of-window version,
// a NodeId that is not the one derived from the key, a stale or replayed timestamp. What an attacker CAN still learn: that
// some TCP service listens on the port, and (by timing) that a connection was closed after N bytes.
//
// REPLAY RULE (WireGuard's TAI64N idea): Msg1's timestamp must be (a) within `max_clock_skew_ns` of the responder's clock
// and (b) strictly greater than the latest timestamp already accepted from that initiator key (a bounded per-responder
// record). A captured Msg1 replayed later is therefore refused silently. Residual: the record is in memory, so a responder
// RESTART forgets it, and a replay inside the skew window right after a restart (or after an eviction) is accepted once;
// the replayer gets Msg2 but cannot derive any key (it lacks the initiator's ephemeral and static secrets) — it learns only
// that the responder is alive. ADR-068 lists this.
//
// SESSION KEYS: Noise Split() yields the two directional transport keys; each is turned into a Quark envelope key by
// HMAC-BLAKE2b(split_key, "quark-pinned/v1 c2s|s2c" || handshake_hash) — a label domain of its own, disjoint from TLS mode's
// exporter labels ("quark c2s"/"quark s2c"), and bound to this very handshake. They feed `SodiumAeadChaCha20Poly1305`
// inside the existing per-lane-counter envelope (secure_transport.hpp) unchanged.
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "quark/adapters/sodium/noise.hpp"
#include "quark/adapters/sodium/pinned_identity.hpp"
#include "quark/adapters/sodium/sodium_aead.hpp"
#include "quark/core/handshake.hpp"
#include "quark/core/pinned_key.hpp"

namespace quark::adapters {

inline constexpr std::uint16_t kPinnedVersion = 1;
inline constexpr std::size_t kPinnedMsg1PayloadLen = 2 + 8 + 8 + 8 + 32;  // 58
inline constexpr std::size_t kPinnedMsg2PayloadLen = 2 + 8;               // 10
inline constexpr std::size_t kPinnedMsg1Len = noise::kMsg1Overhead + kPinnedMsg1PayloadLen;  // 154
inline constexpr std::size_t kPinnedMsg2Len = noise::kMsg2Overhead + kPinnedMsg2PayloadLen;  // 58

// "quark-pinned-key/v1" || mode byte. The mode byte is the TrustMode, so the prologue differs per mode by construction.
[[nodiscard]] inline noise::Bytes pinned_prologue(TrustMode mode = TrustMode::PinnedKey) {
    constexpr std::string_view label = "quark-pinned-key/v1";
    noise::Bytes p;
    for (char c : label) p.push_back(static_cast<std::byte>(c));
    p.push_back(static_cast<std::byte>(mode));
    return p;
}

struct PinnedMsg1Payload {
    std::uint16_t version = kPinnedVersion;
    ClusterId cluster{};
    NodeId node{};
    std::uint64_t timestamp_ns = 0;
    Ed25519PublicKey ed25519{};
};
struct PinnedMsg2Payload {
    std::uint16_t version = kPinnedVersion;
    NodeId node{};
};

namespace detail_pinned {
inline void put(noise::Bytes& b, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
[[nodiscard]] inline std::uint64_t get(std::span<const std::byte> b, std::size_t off, int n) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[off + static_cast<std::size_t>(i)])) << (8 * i);
    return v;
}
}  // namespace detail_pinned

[[nodiscard]] inline noise::Bytes encode_pinned_msg1_payload(const PinnedMsg1Payload& p) {
    noise::Bytes b;
    detail_pinned::put(b, p.version, 2);
    detail_pinned::put(b, p.cluster.value, 8);
    detail_pinned::put(b, p.node.value, 8);
    detail_pinned::put(b, p.timestamp_ns, 8);
    b.insert(b.end(), p.ed25519.begin(), p.ed25519.end());
    return b;
}
[[nodiscard]] inline std::optional<PinnedMsg1Payload> decode_pinned_msg1_payload(std::span<const std::byte> b) {
    if (b.size() != kPinnedMsg1PayloadLen) return std::nullopt;
    PinnedMsg1Payload p;
    p.version = static_cast<std::uint16_t>(detail_pinned::get(b, 0, 2));
    p.cluster = ClusterId{detail_pinned::get(b, 2, 8)};
    p.node = NodeId{detail_pinned::get(b, 10, 8)};
    p.timestamp_ns = detail_pinned::get(b, 18, 8);
    std::memcpy(p.ed25519.data(), b.data() + 26, 32);
    return p;
}
[[nodiscard]] inline noise::Bytes encode_pinned_msg2_payload(const PinnedMsg2Payload& p) {
    noise::Bytes b;
    detail_pinned::put(b, p.version, 2);
    detail_pinned::put(b, p.node.value, 8);
    return b;
}
[[nodiscard]] inline std::optional<PinnedMsg2Payload> decode_pinned_msg2_payload(std::span<const std::byte> b) {
    if (b.size() != kPinnedMsg2PayloadLen) return std::nullopt;
    return PinnedMsg2Payload{static_cast<std::uint16_t>(detail_pinned::get(b, 0, 2)),
                             NodeId{detail_pinned::get(b, 2, 8)}};
}

// Bounded per-responder record of the latest accepted Msg1 timestamp per initiator key. Only keys that were already
// authenticated AND admitted reach accept(), so an attacker cannot grow it. Thread-safe.
class PinnedReplayGuard {
public:
    explicit PinnedReplayGuard(std::size_t capacity = 4096) : capacity_(capacity == 0 ? 1 : capacity) {}

    // True iff `timestamp_ns` is strictly newer than the latest accepted from `initiator`; records it.
    [[nodiscard]] bool accept(const Ed25519PublicKey& initiator, std::uint64_t timestamp_ns) {
        std::lock_guard g(mu_);
        const auto it = latest_.find(initiator);
        if (it != latest_.end()) {
            if (timestamp_ns <= it->second) return false;
            it->second = timestamp_ns;
            return true;
        }
        if (latest_.size() >= capacity_) {  // evict the oldest-inserted initiator (FIFO)
            latest_.erase(order_.front());
            order_.pop_front();
        }
        latest_.emplace(initiator, timestamp_ns);
        order_.push_back(initiator);
        return true;
    }
    [[nodiscard]] std::size_t size() const {
        std::lock_guard g(mu_);
        return latest_.size();
    }

private:
    mutable std::mutex mu_;
    std::size_t capacity_;
    std::map<Ed25519PublicKey, std::uint64_t> latest_;
    std::deque<Ed25519PublicKey> order_;
};

struct PinnedOptions {
    std::uint16_t version = kPinnedVersion;
    // Mirrors ADR-053's HELLO window: a peer is accepted iff |peer - ours| <= window.
    std::uint16_t version_window = 1;
    // Absolute bound on |Msg1 timestamp - responder clock|. LAN devices have unsynchronised clocks, so the default is
    // generous; <= 0 disables the absolute bound (the per-initiator monotonic rule still applies).
    std::int64_t max_clock_skew_ns = 120'000'000'000;
    std::size_t replay_capacity = 4096;
    // false ONLY for the test control "with the rule disabled the responder answers a replay". Never in production.
    bool replay_guard = true;
    // Unix-epoch nanoseconds. Empty = std::chrono::system_clock. Injectable so tests control time (014 idiom).
    std::function<std::int64_t()> clock_unix_ns;
};

// Shared by every engine one factory creates; engines hold it by shared_ptr so they may outlive the factory.
struct PinnedContext {
    PinnedContext(PinnedIdentity id, std::shared_ptr<const PinnedTrust> t,
                  std::shared_ptr<const PinnedKeyDirectory> d, PinnedOptions o)
        : identity(std::move(id)), trust(std::move(t)), directory(std::move(d)), options(std::move(o)),
          replay(options.replay_capacity) {}

    [[nodiscard]] std::int64_t now_ns() const {
        if (options.clock_unix_ns) return options.clock_unix_ns();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    // Strictly increasing across this node's handshakes even if the clock steps backwards, so a responder's
    // per-initiator monotonic rule never refuses an honest retry.
    [[nodiscard]] std::uint64_t next_timestamp() {
        const std::int64_t n = now_ns();
        std::uint64_t want = n < 0 ? 0 : static_cast<std::uint64_t>(n);
        std::uint64_t prev = last_timestamp.load(std::memory_order_relaxed);
        for (;;) {
            const std::uint64_t use = want > prev ? want : prev + 1;
            if (last_timestamp.compare_exchange_weak(prev, use, std::memory_order_relaxed)) return use;
        }
    }

    PinnedIdentity identity;
    std::shared_ptr<const PinnedTrust> trust;
    std::shared_ptr<const PinnedKeyDirectory> directory;
    PinnedOptions options;
    PinnedReplayGuard replay;
    std::atomic<std::uint64_t> last_timestamp{0};
};

class PinnedHandshakeEngine final : public HandshakeEngine {
public:
    // `expected_peer`: the NodeId the caller believes it talks to. NodeId{0} means "unspecified" (a standalone
    // responder that accepts whichever admitted key dials it; 0 is never a valid derived id). `self` must be the
    // NodeId derived from this node's key, or the engine fails (a NodeId is never self-claimed in this mode).
    PinnedHandshakeEngine(bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster,
                          std::shared_ptr<PinnedContext> ctx,
                          std::shared_ptr<const std::unordered_set<Fingerprint>> revoked)
        : is_client_(is_client), self_(self), expected_peer_(expected_peer), cluster_(cluster), ctx_(std::move(ctx)),
          revoked_(std::move(revoked)) {}

    Step advance(std::span<const std::byte> in, std::vector<std::byte>& out) override {
        if (state_ == State::Dead || state_ == State::Done) return Step::Failed;
        if (!(self_ == ctx_->identity.node_id())) return fail("self NodeId is not the one derived from this key");
        return is_client_ ? advance_initiator(in, out) : advance_responder(in, out);
    }

    [[nodiscard]] Result take_result() override { return std::move(result_); }
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

private:
    enum class State { Start, AwaitMsg2, Done, Dead };

    Step fail(std::string_view why) {
        failure_ = why;
        state_ = State::Dead;
        hs_.reset();
        return Step::Failed;
    }

    [[nodiscard]] bool revoked_key(const Ed25519PublicKey& k) const {
        if (!revoked_) return false;
        Fingerprint fp{};
        std::memcpy(fp.data(), k.data(), fp.size());
        return revoked_->contains(fp);
    }
    [[nodiscard]] bool version_ok(std::uint16_t peer) const noexcept {
        const std::uint16_t ours = ctx_->options.version, w = ctx_->options.version_window;
        return static_cast<unsigned>(peer) + w >= ours && static_cast<unsigned>(peer) <= static_cast<unsigned>(ours) + w;
    }

    // ---- initiator ----
    Step advance_initiator(std::span<const std::byte> in, std::vector<std::byte>& out) {
        if (state_ == State::Start) {
            if (!in.empty()) return fail("initiator expects to speak first");
            // Nothing leaves this node unless the peer is one the application currently trusts: no bytes to an
            // unknown or un-admitted key, so a removed device is not even dialled with an encrypted Msg1.
            const auto key = ctx_->directory ? ctx_->directory->key_of(cluster_, expected_peer_) : std::nullopt;
            if (!key) return fail("no pinned key for peer");
            if (!(derive_pinned_node_id(*key) == expected_peer_)) return fail("pinned key does not derive the NodeId");
            if (!ctx_->trust || !ctx_->trust->admitted(cluster_, *key)) return fail("peer key not admitted");
            if (revoked_key(*key)) return fail("peer key revoked");
            noise::Key32 rs{};
            if (!ed25519_to_x25519(*key, rs)) return fail("peer key is not a valid curve point");
            peer_key_ = *key;
            hs_ = std::make_unique<noise::IkHandshake>(noise::Role::Initiator, pinned_prologue(),
                                                       ctx_->identity.noise_static(), &rs);
            PinnedMsg1Payload p;
            p.version = ctx_->options.version;
            p.cluster = cluster_;
            p.node = self_;
            p.timestamp_ns = ctx_->next_timestamp();
            p.ed25519 = ctx_->identity.public_key();
            const noise::Bytes payload = encode_pinned_msg1_payload(p);
            noise::Bytes msg;
            if (!hs_->write_msg1(payload, msg)) return fail("Noise message 1 could not be built");
            out.insert(out.end(), msg.begin(), msg.end());
            state_ = State::AwaitMsg2;
            return Step::WantWrite;
        }
        // AwaitMsg2
        if (in.size() != kPinnedMsg2Len) return fail("malformed Msg2 length");
        noise::Bytes payload;
        if (!hs_->read_msg2(in, payload)) return fail("Msg2 did not authenticate");
        const auto p2 = decode_pinned_msg2_payload(payload);
        if (!p2) return fail("malformed Msg2 payload");
        if (!version_ok(p2->version)) return fail("peer version outside the supported window");
        if (!(p2->node == expected_peer_)) return fail("Msg2 NodeId is not the pinned key's NodeId");
        if (!ctx_->trust->admitted(cluster_, peer_key_)) return fail("peer key no longer admitted");
        return finish(peer_key_, expected_peer_, cluster_);
    }

    // ---- responder ----
    Step advance_responder(std::span<const std::byte> in, std::vector<std::byte>& out) {
        // MESSAGE-oriented: SecureTransport hands over one whole Authenticate payload, and a byte-stream caller reassembles
        // the fixed 154 bytes itself (a partial write is its business, not a parked engine's: buffering here let one injected
        // 1-byte frame park a responder engine for the handshake timeout and poison the real Msg1).
        if (in.size() != kPinnedMsg1Len) return fail("Msg1 length wrong");
        const std::span<const std::byte> in_buf_ = in;

        hs_ = std::make_unique<noise::IkHandshake>(noise::Role::Responder, pinned_prologue(),
                                                   ctx_->identity.noise_static(), nullptr);
        noise::Bytes payload;
        if (!hs_->read_msg1(in_buf_, payload)) return fail("Msg1 did not authenticate");  // a prober: stay silent
        const auto p = decode_pinned_msg1_payload(payload);
        if (!p) return fail("malformed Msg1 payload");
        if (!version_ok(p->version)) return fail("peer version outside the supported window");
        if (!(p->cluster == cluster_)) return fail("wrong cluster");
        // The Ed25519 key in the payload must be the one whose conversion authenticated the message.
        noise::Key32 x{};
        if (!ed25519_to_x25519(p->ed25519, x) ||
            ::sodium_memcmp(x.data(), hs_->remote_static().data(), x.size()) != 0)
            return fail("payload key does not match the Noise static key");
        const NodeId derived = derive_pinned_node_id(p->ed25519);
        if (derived.value == 0 || !(p->node == derived)) return fail("claimed NodeId is not derived from the key");
        if (expected_peer_.value != 0 && !(expected_peer_ == derived)) return fail("peer is not the expected NodeId");
        if (!ctx_->trust || !ctx_->trust->admitted(cluster_, p->ed25519)) return fail("initiator not admitted");
        if (revoked_key(p->ed25519)) return fail("initiator key revoked");
        const std::int64_t skew = ctx_->options.max_clock_skew_ns;
        if (skew > 0) {
            const std::int64_t now_s = ctx_->now_ns();
            const std::uint64_t now = now_s < 0 ? 0 : static_cast<std::uint64_t>(now_s);
            const std::uint64_t ts = p->timestamp_ns;
            const std::uint64_t delta = ts > now ? ts - now : now - ts;  // unsigned: no signed overflow
            if (delta > static_cast<std::uint64_t>(skew)) return fail("Msg1 timestamp outside the allowed clock skew");
        }
        if (ctx_->options.replay_guard && !ctx_->replay.accept(p->ed25519, p->timestamp_ns))
            return fail("Msg1 replayed or stale");

        noise::Bytes msg;
        if (!hs_->write_msg2(encode_pinned_msg2_payload({ctx_->options.version, self_}), msg))
            return fail("Noise message 2 could not be built");
        if (finish(p->ed25519, derived, cluster_) != Step::Done) return Step::Failed;
        out.insert(out.end(), msg.begin(), msg.end());  // Done carries Msg2; a failure above sent nothing
        return Step::Done;
    }

    // Shared tail: Split, derive the two Quark envelope keys, fill the Result.
    Step finish(const Ed25519PublicKey& peer_key, NodeId peer_node, ClusterId cluster) {
        noise::CipherState send, recv;
        if (!hs_->split(send, recv)) return fail("Noise split failed");
        const noise::Hash64& h = hs_->handshake_hash();
        const char* send_label = is_client_ ? kLabelC2S : kLabelS2C;
        const char* recv_label = is_client_ ? kLabelS2C : kLabelC2S;
        auto derive = [&](const noise::CipherState& cs, const char* label) {
            noise::Hash64 d = noise::hmac(cs.key(), std::as_bytes(std::span<const char>(label, std::strlen(label))), h);
            noise::Key32 k{};
            std::memcpy(k.data(), d.data(), k.size());
            auto c = std::make_shared<SodiumAeadChaCha20Poly1305>(std::span<const std::byte, noise::kKeyLen>(k));
            noise::wipe(k);
            noise::wipe(d);
            return c;
        };
        result_.send_cipher = derive(send, send_label);
        result_.recv_cipher = derive(recv, recv_label);
        std::memcpy(result_.peer_fingerprint.data(), peer_key.data(), result_.peer_fingerprint.size());
        result_.peer_node_id = peer_node;
        result_.peer_cluster_id = cluster;
        state_ = State::Done;
        hs_.reset();
        return Step::Done;
    }

    static constexpr const char* kLabelC2S = "quark-pinned/v1 c2s";
    static constexpr const char* kLabelS2C = "quark-pinned/v1 s2c";

    bool is_client_;
    NodeId self_, expected_peer_;
    ClusterId cluster_;
    std::shared_ptr<PinnedContext> ctx_;
    std::shared_ptr<const std::unordered_set<Fingerprint>> revoked_;
    State state_ = State::Start;
    std::unique_ptr<noise::IkHandshake> hs_;
    Ed25519PublicKey peer_key_{};
    Result result_{};
    std::string_view failure_{};
};

// One factory per node: holds the node's identity, the application's trust provider and key directory, and the replay
// record. `create()` ignores the TLS-shaped arguments (own_identity, trust) — pinned mode has no certificates; it honours
// `revoked` by treating its Fingerprints as Ed25519 public keys (a pinned peer's `peer_fingerprint` IS its full key).
class PinnedHandshakeEngineFactory final : public HandshakeEngineFactory {
public:
    PinnedHandshakeEngineFactory(PinnedIdentity identity, std::shared_ptr<const PinnedTrust> trust,
                                 std::shared_ptr<const PinnedKeyDirectory> directory, PinnedOptions options = {})
        : ctx_(std::make_shared<PinnedContext>(std::move(identity), std::move(trust), std::move(directory),
                                               std::move(options))) {}

    [[nodiscard]] std::unique_ptr<HandshakeEngine> create(
        bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster_id, IdentityMaterial::Snapshot,
        TrustStore::Snapshot, std::shared_ptr<const std::unordered_set<Fingerprint>> revoked) override {
        return std::make_unique<PinnedHandshakeEngine>(is_client, self, expected_peer, cluster_id, ctx_,
                                                       std::move(revoked));
    }

    [[nodiscard]] const PinnedIdentity& identity() const noexcept { return ctx_->identity; }
    [[nodiscard]] PinnedReplayGuard& replay_guard() noexcept { return ctx_->replay; }

private:
    std::shared_ptr<PinnedContext> ctx_;
};

}  // namespace quark::adapters
