// Implements 017-Delivery-Guarantees §"Message identity, made deterministic" + §"The mechanism"
// (fenced atomic commit + post-commit transactional outbox) + §"Interaction notes" (per-sender
// high-water-mark dedup + reply cache). This is the std-only CORE of effectively-once EFFECTS:
// the effect (state change + outbound messages + reply) happens exactly once despite duplicate
// delivery, crash-replay, and re-placement under partition.
//
// Composes: 006 (FIFO + message identity), 012 (fenced append-only WAL — persistence.hpp), 016
// (canonical tagged durable records — serialize.hpp/snapshot.hpp), and the 005 `Delivery<Level>`
// policy (policies.hpp). The `AtMostOnce` default pays NOTHING: this header is only pulled in when
// an actor opts into `Delivery<AtLeastOnce|EffectivelyOnce>`; a plain `Actor<T, Sequential>`
// instantiates none of the machinery below.
//
// THE MECHANISM (017 §"The mechanism"), for a message M in an EffectivelyOnce actor:
//   (a) DEDUP  — if `M.seq <= watermark[M.sender]`, M is a duplicate → skip the effect and re-emit
//                the recorded reply/ack (idempotent). Else continue.
//   (b) HANDLE — produce a state change/event, zero+ OUTBOUND messages, an optional reply.
//   (c) COMMIT ATOMICALLY — ONE durable batch (`Store::append_batch` under the activation's fencing
//                token) carrying the event(s) + the advanced `watermark[M.sender]` + the
//                transactional OUTBOX + the reply. A stale token → `append_batch` returns
//                `fenced_error()` → the commit is REJECTED (this activation is a partitioned zombie:
//                abort + escalate 007). State + watermark + outbox commit TOGETHER OR NOT AT ALL.
//   (d) DELIVER AFTER COMMIT — a post-commit dispatcher drains the durable outbox, sending each
//                outbound with AtLeastOnce retry (each dedup'd at ITS receiver by this same
//                mechanism, by its DETERMINISTIC id). The reply is sent post-commit too. A fenced-out
//                zombie's commit never lands → its outbox never becomes durable → the dispatcher
//                never runs → the zombie SENDS NOTHING. Fencing protects state AND output with one
//                gate.
//
// Because the whole commit is one atomic fenced batch, the chain is effectively-once BY INDUCTION.
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (this header does NOT implement these — each names its downstream owner):
//   * A real DURABLE / linearizable-network `StateStore` (the CP consistency anchor) — 019/012
//     adapter. `InMemoryStore` (persistence.hpp) is the reference substrate reused here; it is a
//     LOGICAL fence gate + in-RAM WAL, NOT durable across process exit. `store_is_linearizable_v`
//     below is the runtime CP gate a real adapter opts out of.
//   * Cross-node retry TRANSPORT for AtLeastOnce / outbox delivery over the network — 010
//     (`Transport`). In-process, `drain_outbox` hands each pending outbound to a local sink/router;
//     the sink IS the transport seam.
//   * Watermark GC / idle-sender TTL reclamation (017 open question) — a permanently-silent sender
//     leaves a stale watermark entry. The hook is `watermark_` being a plain map keyed by sender;
//     the reaper is NOT built here.
//   * Durable OUTBOX ack-watermark / shard-batched outbox for throughput (017 open question) —
//     `drain_outbox` re-sends every committed outbox record on each call (honest AtLeastOnce from a
//     durable record); marking an entry delivered needs its own fenced commit. The per-actor outbox
//     is the honest core; a shard-batched outbox and a durable ack-cursor are deferred.
// ============================================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/describe.hpp"     // Described (016)
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"          // ActorId, TypeKey
#include "quark/core/persistence.hpp"  // Store, FenceToken, SeqNo, EventRecord, fenced_error
#include "quark/core/policies.hpp"     // DeliveryLevel (the policy this composes)
#include "quark/core/serialize.hpp"    // encode_record/decode_record, peek_record_header, fingerprint_v
#include "quark/core/snapshot.hpp"     // encode_durable / decode_durable

namespace quark {

// ============================================================================================
// 017 §"Message identity, made deterministic" — MsgKey = (sender, seq).
// ============================================================================================
// The stable delivery identity of a message: WHO sent it (`sender`, the sender's `ActorId::hash()`)
// and a per-`(sender, receiver)` monotonic `seq`. Combined with 006 FIFO, a receiver dedups with a
// per-sender HIGH-WATER-MARK over `seq` — O(active senders), not O(messages). This is the (sender,
// seq) pair 017 names; it is distinct from the packed 64-bit `descriptor.hpp` `MessageId` the hot
// path carries on the pooled descriptor (that word can hold a `derive`d id; the split (sender, seq)
// form here is what the dedup/outbox machinery reasons over off the hot path).
struct MsgKey {
    std::uint64_t sender = 0;  // sender ActorId hash
    std::uint64_t seq = 0;     // per-(sender, receiver) monotonic counter
    friend constexpr bool operator==(MsgKey, MsgKey) = default;
};

// OUTBOUND-id derivation stride (017 §"made deterministic"). Bounds the outbound fan-out of a single
// handler while keeping the derived per-(sender, receiver) `seq` STRICTLY MONOTONIC in the inbound
// `seq` order — so a receiver's high-water-mark recognizes a replayed duplicate. `derive` is a PURE
// function of `(inbound, index)`, so a REPLAYED handler (crash before commit) re-produces the SAME
// outbound ids, and their receivers recognize the duplicates. Only id-derivation is deterministic —
// not the whole handler.
inline constexpr std::uint64_t outbound_derivation_stride = std::uint64_t{1} << 20;  // ≤ ~1M / handler

// Deterministically derive an outbound message's identity from the inbound id + its emission index.
// `self_hash` is the deriving actor's `ActorId::hash()` (the outbound's sender). The derived `seq`
// depends ONLY on `inbound.seq` and `index`, so it is reproducible on replay AND monotonic in the
// inbound FIFO order for a SINGLE upstream.
//
// LIMITATION (documented, matches the 017 open-question family): the derived `seq` does not fold in
// `inbound.sender`, so at a FAN-IN receiver (the deriving actor consuming from multiple upstreams)
// two upstreams' overlapping `inbound.seq` ranges can produce non-globally-monotonic outbound seqs
// against a single downstream — the high-water-mark could then wrongly skip a genuine message.
// Robust fan-in monotonicity needs a per-(sender, receiver) DURABLE outbound counter (committed with
// the outbox); the pure derivation here is correct for the linear Client→Order→Inventory chain that
// is 017's acceptance test. Cross-`type_key` derivation stability across binaries/platforms carries
// the same 008/016 `type_key`-stability caveat.
[[nodiscard]] constexpr MsgKey derive_outbound(std::uint64_t self_hash, MsgKey inbound,
                                               std::uint32_t index) noexcept {
    return MsgKey{self_hash, inbound.seq * outbound_derivation_stride + index + 1};
}

// ============================================================================================
// Durable record types committed ATOMICALLY alongside the domain event in the ONE fenced batch.
// Each is a distinct 016 Described type (its own fingerprint / durable type_key), so recovery routes
// a log record by peeking its header type_key. The domain EVENT is opaque to this header — it is
// just another durable record in the same batch (the atomic state change on the EventSourced WAL).
// ============================================================================================

// A durable per-sender high-water-mark advance: after processing an inbound with `seq`, the receiver
// records `watermark[sender] = seq`. Recovery replays these (last-wins per sender) to rebuild dedup.
struct WatermarkRecord {
    std::uint64_t sender = 0;  // inbound sender ActorId hash
    std::uint64_t seq = 0;     // new high-water-mark (highest processed seq from `sender`)
};
QUARK_SERIALIZE(WatermarkRecord, (1, sender), (2, seq))

// A durable transactional-outbox entry: a pending outbound message to deliver AFTER commit. Carries
// the target actor id, the derived (sender, seq) identity, and the 016-durable-encoded outbound body
// (opaque — the dispatcher hands it to the local sink / 010 transport). `body` is a raw byte blob
// stored as a length-delimited string (may contain NULs).
struct OutboxRecord {
    std::uint64_t target_type = 0;  // target ActorId.type.value
    std::uint64_t target_key = 0;   // target ActorId.key
    std::uint64_t out_sender = 0;   // derived MsgKey.sender (this actor's hash)
    std::uint64_t out_seq = 0;      // derived MsgKey.seq (deterministic)
    std::string body;               // encode_durable(outbound) — 016 canonical tagged durable bytes
};
QUARK_SERIALIZE(OutboxRecord, (1, target_type), (2, target_key), (3, out_sender), (4, out_seq),
                (5, body))

// A durable recorded reply (the reply cache entry). A duplicate `ask` from `reply_to` re-emits this
// identical reply WITHOUT reprocessing (017 §"Reply cache"). Keyed by sender; carries the inbound
// `seq` it answers so a re-ask of the same seq gets exactly its reply.
struct ReplyRecord {
    std::uint64_t reply_to = 0;  // inbound sender ActorId hash the reply is addressed to
    std::uint64_t in_seq = 0;    // the inbound seq this reply answers
    std::string body;            // encode_durable(reply) — 016 canonical tagged durable bytes
};
QUARK_SERIALIZE(ReplyRecord, (1, reply_to), (2, in_seq), (3, body))

namespace detail {

// Raw-byte blob <-> std::string (the durable record `body` field). std::string holds arbitrary
// bytes (NULs included); the tagged codec length-prefixes it, so it round-trips exactly.
[[nodiscard]] inline std::string bytes_to_blob(std::span<const std::byte> b) {
    std::string s;
    s.resize(b.size());
    if (!b.empty()) std::memcpy(s.data(), b.data(), b.size());
    return s;
}
[[nodiscard]] inline std::vector<std::byte> blob_to_bytes(const std::string& s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

// Detect a store that DECLARES its consistency: a real distributed/eventually-consistent adapter
// sets `static constexpr bool is_linearizable = false;`. The reference in-process stores are CP by
// construction (a single logical fence gate + a total WAL order), so an absent flag defaults to CP.
template <class S>
concept declares_linearizable = requires {
    { S::is_linearizable } -> std::convertible_to<bool>;
};

}  // namespace detail

// Runtime CP gate (017 §"The consistency price"). EffectivelyOnce makes the `StateStore` the
// linearizable consistency anchor; an eventually-consistent store cannot order fenced commits and is
// REJECTED. Consistency is a RUNTIME property of the chosen adapter, so this is a store-type trait a
// real eventually-consistent adapter opts out of via `is_linearizable = false` (documented seam).
template <class S>
inline constexpr bool store_is_linearizable_v = [] {
    if constexpr (detail::declares_linearizable<S>) {
        return static_cast<bool>(S::is_linearizable);
    } else {
        return true;  // reference in-process CP store (InMemoryStore); a distributed AP adapter MUST
                      // declare `is_linearizable = false` to be rejected here.
    }
}();

// ============================================================================================
// EffectivelyOnceLane — the per-activation effectively-once mechanism over a 012 `Store`.
//
// Domain-AGNOSTIC w.r.t. state: the domain event is committed as an opaque 016 durable record in the
// SAME atomic batch as the watermark/outbox/reply (the EventSourced WAL naturally makes the whole
// commit one transaction — 012 §"Two models"). The state FOLD (`apply(State&, Event)`) is owned by
// event_log.hpp; on recovery this lane hands each domain-event record back to a caller callback.
//
// A lane is created per ACTIVATION with the activation's fencing token (`store.acquire_fence(id)`).
// Under partition two activations hold two tokens; only the HIGHER token's `commit()` is accepted —
// the lower is fenced out and produces NOTHING durable (no state, no outbox, no reply).
// ============================================================================================
template <Store S>
class EffectivelyOnceLane {
    static_assert(store_is_linearizable_v<S>,
                  "Delivery<EffectivelyOnce> requires a linearizable (CP) StateStore: an "
                  "eventually-consistent store cannot order fenced commits (017 §\"The consistency "
                  "price\"). A distributed AP adapter must declare `is_linearizable = false`.");

public:
    // A recorded reply held in the in-memory reply cache (mirror of the durable ReplyRecord).
    struct CachedReply {
        std::uint64_t in_seq = 0;
        std::string body;  // encode_durable(reply) bytes
    };

    // The result of a successful atomic commit.
    struct CommitResult {
        SeqNo last_seq = 0;            // highest commit seq written by this batch
        std::size_t record_count = 0;  // number of durable records in the batch
    };

    // `next_seq` is the seq the next commit will assign — a recovering activation passes
    // `store.last_seq(id) + 1` so the count continues strictly increasing across restarts.
    EffectivelyOnceLane(S& store, ActorId self, FenceToken fence, SeqNo next_seq) noexcept
        : store_(&store), self_(self), self_hash_(self.hash()), fence_(fence), next_seq_(next_seq) {}

    [[nodiscard]] ActorId self() const noexcept { return self_; }
    [[nodiscard]] std::uint64_t self_hash() const noexcept { return self_hash_; }
    [[nodiscard]] FenceToken fence() const noexcept { return fence_; }
    [[nodiscard]] SeqNo next_seq() const noexcept { return next_seq_; }

    // --- (a) DEDUP (017 §"The mechanism" step 1) ------------------------------------------------
    // A message from `sender` with `in_seq` is a DUPLICATE iff `in_seq <= watermark[sender]`.
    [[nodiscard]] bool is_duplicate(std::uint64_t sender, std::uint64_t in_seq) const noexcept {
        const auto it = watermark_.find(sender);
        return it != watermark_.end() && in_seq <= it->second;
    }
    [[nodiscard]] bool is_duplicate(MsgKey m) const noexcept { return is_duplicate(m.sender, m.seq); }

    [[nodiscard]] std::uint64_t watermark_of(std::uint64_t sender) const noexcept {
        const auto it = watermark_.find(sender);
        return it == watermark_.end() ? std::uint64_t{0} : it->second;
    }

    // The recorded reply for `sender` (for a duplicate `ask` to re-emit WITHOUT reprocessing).
    [[nodiscard]] std::optional<CachedReply> cached_reply(std::uint64_t sender) const {
        const auto it = reply_cache_.find(sender);
        if (it == reply_cache_.end()) return std::nullopt;
        return it->second;
    }

    // Decode a cached/committed reply body back into its typed form.
    template <Described Reply>
    [[nodiscard]] static result<Reply> decode_reply(const CachedReply& c) {
        return decode_durable<Reply>(detail::blob_to_bytes(c.body));
    }

    // --- (b)+(c) BUILD ONE COMMIT (017 §"The mechanism" steps 2–3) ------------------------------
    // Scoped to ONE inbound message. The handler stages its domain event(s), its OUTBOUND messages
    // (each getting a DETERMINISTIC derived id), and its optional reply; `commit()` assembles them —
    // plus the watermark advance for the inbound sender — into ONE atomic fenced `append_batch`.
    class Commit {
    public:
        // Stage a domain EVENT (the state change) — committed atomically with the rest.
        template <Described Event>
        [[nodiscard]] result<void> event(const Event& ev) {
            auto bytes = encode_durable(ev);
            if (!bytes) return std::unexpected(bytes.error());
            events_.push_back(std::move(*bytes));
            return {};
        }

        // Stage an OUTBOUND message to `target`. Returns its DETERMINISTIC derived id (available even
        // if the commit is later fenced out — proving the id is a pure function, not a side effect of
        // committing). The outbound is durably recorded in the transactional outbox.
        template <Described Out>
        [[nodiscard]] result<MsgKey> send(ActorId target, const Out& msg) {
            const MsgKey oid = derive_outbound(lane_->self_hash_, inbound_, out_index_++);
            auto bytes = encode_durable(msg);
            if (!bytes) return std::unexpected(bytes.error());
            OutboxRecord rec;
            rec.target_type = target.type.value;
            rec.target_key = target.key;
            rec.out_sender = oid.sender;
            rec.out_seq = oid.seq;
            rec.body = detail::bytes_to_blob(*bytes);
            outbox_.push_back(std::move(rec));
            return oid;
        }

        // Stage the REPLY to the inbound sender (recorded in the durable reply cache; sent post-commit).
        template <Described Reply>
        [[nodiscard]] result<void> reply(const Reply& r) {
            auto bytes = encode_durable(r);
            if (!bytes) return std::unexpected(bytes.error());
            ReplyRecord rec;
            rec.reply_to = inbound_.sender;
            rec.in_seq = inbound_.seq;
            rec.body = detail::bytes_to_blob(*bytes);
            reply_ = std::move(rec);
            return {};
        }

        // ATOMIC FENCED COMMIT (017 §"The mechanism" step 3). Assemble the whole batch — event(s),
        // the advanced watermark[inbound.sender], the outbox, the reply — and hand it to the store's
        // ATOMIC `append_batch` under the activation's fencing token in ONE call. On success: advance
        // next_seq, and mirror the durable watermark/reply/outbox in memory. On a STALE token the
        // store returns `fenced_error()` → the whole commit is rejected, NOTHING becomes durable, and
        // the caller must abort + escalate (007): this activation is a partitioned zombie.
        [[nodiscard]] result<CommitResult> commit() {
            std::vector<EventRecord> batch;
            batch.reserve(events_.size() + outbox_.size() + 2);
            SeqNo seq = lane_->next_seq_;

            for (auto& ev : events_) batch.push_back(EventRecord{seq++, std::move(ev)});

            // The watermark advance — committed in the SAME batch as the effect (dedup marker + state
            // together or not at all). Always emitted for a processed message (idempotent replay).
            {
                WatermarkRecord wm{inbound_.sender, inbound_.seq};
                auto b = encode_durable(wm);
                if (!b) return std::unexpected(b.error());
                batch.push_back(EventRecord{seq++, std::move(*b)});
            }

            for (OutboxRecord& o : outbox_) {
                auto b = encode_durable(o);
                if (!b) return std::unexpected(b.error());
                batch.push_back(EventRecord{seq++, std::move(*b)});
            }

            if (reply_) {
                auto b = encode_durable(*reply_);
                if (!b) return std::unexpected(b.error());
                batch.push_back(EventRecord{seq++, std::move(*b)});
            }

            auto rc = lane_->store_->append_batch(lane_->self_, lane_->fence_,
                                                  std::span<const EventRecord>(batch));
            if (!rc) return std::unexpected(rc.error());  // fenced/failed: NONE durable — zombie inert

            // Durable — now advance the sequence and reflect the commit in the in-memory mirrors so
            // subsequent dedup/dispatch see it without re-reading the store.
            lane_->next_seq_ = seq;
            std::uint64_t& wm = lane_->watermark_[inbound_.sender];
            if (inbound_.seq > wm) wm = inbound_.seq;
            if (reply_) lane_->reply_cache_[inbound_.sender] = CachedReply{reply_->in_seq, reply_->body};
            for (OutboxRecord& o : outbox_) lane_->outbox_.push_back(std::move(o));

            return CommitResult{seq == 0 ? SeqNo{0} : seq - 1, batch.size()};
        }

    private:
        friend class EffectivelyOnceLane;
        Commit(EffectivelyOnceLane* lane, MsgKey inbound) noexcept : lane_(lane), inbound_(inbound) {}

        EffectivelyOnceLane* lane_;
        MsgKey inbound_;
        std::uint32_t out_index_ = 0;
        std::vector<std::vector<std::byte>> events_;
        std::vector<OutboxRecord> outbox_;
        std::optional<ReplyRecord> reply_;
    };

    // Begin building the commit for an inbound message identified by `inbound = (sender, seq)`.
    [[nodiscard]] Commit begin(MsgKey inbound) noexcept { return Commit{this, inbound}; }
    [[nodiscard]] Commit begin(std::uint64_t sender, std::uint64_t in_seq) noexcept {
        return Commit{this, MsgKey{sender, in_seq}};
    }

    // --- (d) POST-COMMIT DISPATCH (017 §"The mechanism" step 4) ---------------------------------
    // Drain the DURABLE outbox: hand each pending outbound to `sink` (the local router / 010
    // transport seam). AtLeastOnce — every committed outbox record is (re-)sent on each call, since a
    // zombie's commit never lands here (its outbox is not durable) and a crash-after-commit recovery
    // re-drains the SAME durable records (with the SAME deterministic ids, dedup'd downstream). The
    // sink returns `result<void>`; a failed send leaves the record pending for a later drain. Returns
    // the number of records successfully handed off.
    //
    // `sink(const OutboxRecord&) -> result<void>`.
    template <class Sink>
    [[nodiscard]] result<std::size_t> drain_outbox(Sink&& sink) {
        std::size_t sent = 0;
        for (const OutboxRecord& o : outbox_) {
            auto rc = sink(o);
            if (!rc) return std::unexpected(rc.error());
            ++sent;
        }
        return sent;
    }

    [[nodiscard]] std::size_t outbox_pending() const noexcept { return outbox_.size(); }
    [[nodiscard]] const std::vector<OutboxRecord>& outbox() const noexcept { return outbox_; }
    [[nodiscard]] std::size_t watermark_count() const noexcept { return watermark_.size(); }

    // --- RECOVERY (017 §"crashes … on recovery") -----------------------------------------------
    // Rebuild the dedup watermark, reply cache, and pending outbox from the durable WAL, and hand
    // each DOMAIN-EVENT record back to `on_event(std::span<const std::byte>)` (the caller folds it
    // into domain state via its `apply`). Routes each log record by peeking its 016 header type_key.
    // Call on a fresh activation AFTER `acquire_fence` (which bumps the token, fencing the old owner).
    //
    // `on_event(std::span<const std::byte> durable_event_bytes) -> result<void>`.
    template <class OnEvent>
    [[nodiscard]] result<void> recover(OnEvent&& on_event) {
        auto cur = store_->read_log(self_, 0);  // all records (seqs start at 1)
        if (!cur) return std::unexpected(cur.error());

        const TypeKey wm_key{fingerprint_v<WatermarkRecord>};
        const TypeKey ob_key{fingerprint_v<OutboxRecord>};
        const TypeKey rp_key{fingerprint_v<ReplyRecord>};

        for (const EventRecord& rec : *cur) {
            auto hdr = peek_record_header(rec.record.data(), rec.record.size());
            if (!hdr) return std::unexpected(hdr.error());

            if (hdr->type == wm_key) {
                auto wm = decode_durable<WatermarkRecord>(rec.record);
                if (!wm) return std::unexpected(wm.error());
                std::uint64_t& hi = watermark_[wm->sender];
                if (wm->seq > hi) hi = wm->seq;
            } else if (hdr->type == ob_key) {
                auto ob = decode_durable<OutboxRecord>(rec.record);
                if (!ob) return std::unexpected(ob.error());
                outbox_.push_back(std::move(*ob));
            } else if (hdr->type == rp_key) {
                auto rp = decode_durable<ReplyRecord>(rec.record);
                if (!rp) return std::unexpected(rp.error());
                reply_cache_[rp->reply_to] = CachedReply{rp->in_seq, rp->body};
            } else {
                // A domain event — hand its opaque durable bytes to the caller's state fold.
                auto rc = on_event(std::span<const std::byte>(rec.record.data(), rec.record.size()));
                if (!rc) return std::unexpected(rc.error());
            }
        }
        next_seq_ = store_->last_seq(self_) + 1;
        return {};
    }

private:
    S* store_;
    ActorId self_;
    std::uint64_t self_hash_;
    FenceToken fence_;
    SeqNo next_seq_;

    // In-memory MIRRORS of durable dedup state (rebuilt by `recover`). Watermark GC / idle-sender TTL
    // (017 open question) would prune permanently-silent senders from `watermark_`; not built here.
    std::unordered_map<std::uint64_t, std::uint64_t> watermark_;    // sender hash -> high-water-mark
    std::unordered_map<std::uint64_t, CachedReply> reply_cache_;    // sender hash -> recorded reply
    std::vector<OutboxRecord> outbox_;                              // committed pending outbound
};

}  // namespace quark
