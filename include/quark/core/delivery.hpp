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
//   * Watermark GC is a CAP, not a TTL: `set_limits` bounds the watermark and reply-cache maps with
//     oldest-touched-first eviction (ADR-051; trade-off documented there and on `set_limits`).
//   * The durable OUTBOX delivered marker (`DeliveredRecord`, written by `mark_delivered` when the
//     receiver acks; `drain_outbox` skips delivered entries; `recover` rebuilds it) - ADR-051.
//   * AtLeastOnce retry+ack for plain remote sends: `ReliableSender` / `ReliableReceiver` below
//     (transport-agnostic go-back-N over a caller-supplied sink; 010 owns the real wire).
// ============================================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <list>
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
    // #18: internal-record identity salt — a user event with the same layout must not be routed here.
    static constexpr std::uint64_t quark_type_salt = 0x51574D5231ULL;
    std::uint64_t sender = 0;  // inbound sender ActorId hash
    std::uint64_t seq = 0;     // new high-water-mark (highest processed seq from `sender`)
};
QUARK_SERIALIZE(WatermarkRecord, (1, sender), (2, seq))

// A durable transactional-outbox entry: a pending outbound message to deliver AFTER commit. Carries
// the target actor id, the derived (sender, seq) identity, and the 016-durable-encoded outbound body
// (opaque — the dispatcher hands it to the local sink / 010 transport). `body` is a raw byte blob
// stored as a length-delimited string (may contain NULs).
struct OutboxRecord {
    // #18: internal-record identity salt — a user event with the same layout must not be routed here.
    static constexpr std::uint64_t quark_type_salt = 0x51574F5231ULL;
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
    // #18: internal-record identity salt — a user event with the same layout must not be routed here.
    static constexpr std::uint64_t quark_type_salt = 0x5157525031ULL;
    std::uint64_t reply_to = 0;  // inbound sender ActorId hash the reply is addressed to
    std::uint64_t in_seq = 0;    // the inbound seq this reply answers
    std::string body;            // encode_durable(reply) — 016 canonical tagged durable bytes
};
QUARK_SERIALIZE(ReplyRecord, (1, reply_to), (2, in_seq), (3, body))

// A durable "outbox entry delivered" marker (#19): written (fenced) when the receiver ACKs the
// outbound `(out_sender, out_seq)`. After it, `drain_outbox` skips the entry and `recover` does not
// resurrect it as pending.
struct DeliveredRecord {
    // #18/ADR-050: internal-record identity salt.
    static constexpr std::uint64_t quark_type_salt = 0x5157444C31ULL;
    std::uint64_t out_sender = 0;
    std::uint64_t out_seq = 0;
};
QUARK_SERIALIZE(DeliveredRecord, (1, out_sender), (2, out_seq))

namespace detail {

// Bounded map keyed by uint64 with oldest-touched-first eviction (cap 0 = unbounded). `find` does NOT
// refresh recency (keeps const lookups const); `touch` does. Off the hot path (durable-commit side).
template <class V>
class BoundedMap {
public:
    void set_cap(std::size_t cap) {
        cap_ = cap;
        trim();
    }
    [[nodiscard]] const V* find(std::uint64_t k) const noexcept {
        const auto it = map_.find(k);
        return it == map_.end() ? nullptr : &it->second.v;
    }
    // Insert-if-absent, mark most recent, enforce the cap (k is newest so is never the victim).
    V& touch(std::uint64_t k) {
        auto it = map_.find(k);
        if (it == map_.end()) {
            order_.push_back(k);
            it = map_.emplace(k, Entry{V{}, std::prev(order_.end())}).first;
        } else {
            order_.splice(order_.end(), order_, it->second.pos);
        }
        V& ref = it->second.v;
        trim();
        return ref;
    }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] std::size_t evictions() const noexcept { return evictions_; }

private:
    struct Entry {
        V v;
        std::list<std::uint64_t>::iterator pos;
    };
    void trim() {
        while (cap_ != 0 && map_.size() > cap_) {
            map_.erase(order_.front());
            order_.pop_front();
            ++evictions_;
        }
    }
    std::size_t cap_ = 0;
    std::size_t evictions_ = 0;
    std::unordered_map<std::uint64_t, Entry> map_;
    std::list<std::uint64_t> order_;
};

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
        const std::uint64_t* w = watermark_.find(sender);
        return w != nullptr && in_seq <= *w;
    }
    [[nodiscard]] bool is_duplicate(MsgKey m) const noexcept { return is_duplicate(m.sender, m.seq); }

    [[nodiscard]] std::uint64_t watermark_of(std::uint64_t sender) const noexcept {
        const std::uint64_t* w = watermark_.find(sender);
        return w == nullptr ? std::uint64_t{0} : *w;
    }

    // The recorded reply for `sender` (for a duplicate `ask` to re-emit WITHOUT reprocessing).
    [[nodiscard]] std::optional<CachedReply> cached_reply(std::uint64_t sender) const {
        const CachedReply* r = reply_cache_.find(sender);
        if (r == nullptr) return std::nullopt;
        return *r;
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
            std::uint64_t& wm = lane_->watermark_.touch(inbound_.sender);
            if (inbound_.seq > wm) wm = inbound_.seq;
            if (reply_) lane_->reply_cache_.touch(inbound_.sender) = CachedReply{reply_->in_seq, reply_->body};
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
    // Drain the DURABLE outbox: hand each PENDING (not yet delivered-marked) outbound to `sink`.
    // AtLeastOnce - a pending record is (re-)sent on each call until `mark_delivered` records the
    // receiver's ack (a zombie's commit never lands; a crash-after-commit recovery re-drains the SAME
    // durable records with the SAME deterministic ids, dedup'd downstream). A failed send leaves the
    // record pending. Returns the number of records handed off. `sink(const OutboxRecord&) ->
    // result<void>`.
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

    // ACK-AWARE dispatch (#19): `sink(const OutboxRecord&) -> result<bool>`; `true` means the receiver
    // ACKED, so the entry is durably marked delivered (fenced commit) and never re-sent, even across
    // crash+recover. `false` = no ack yet (lost frame) -> stays pending for the next call. A sink error
    // or a fenced mark aborts. Returns the number of entries acked+marked this call.
    template <class Sink>
    [[nodiscard]] result<std::size_t> deliver_outbox(Sink&& sink) {
        std::size_t acked = 0;
        // Snapshot the ids: mark_delivered mutates outbox_.
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ids;
        ids.reserve(outbox_.size());
        for (const OutboxRecord& o : outbox_) ids.emplace_back(o.out_sender, o.out_seq);
        for (const auto& [snd, seq] : ids) {
            const OutboxRecord* rec = nullptr;
            for (const OutboxRecord& o : outbox_) {
                if (o.out_sender == snd && o.out_seq == seq) { rec = &o; break; }
            }
            if (rec == nullptr) continue;
            auto r = sink(*rec);
            if (!r) return std::unexpected(r.error());
            if (*r) {
                auto m = mark_delivered(snd, seq);
                if (!m) return std::unexpected(m.error());
                ++acked;
            }
        }
        return acked;
    }

    // Durably record that outbound `(out_sender, out_seq)` was acked, and drop it from the pending
    // outbox. Idempotent: an unknown / already-delivered id is a no-op (duplicate acks are normal).
    // Fenced like every commit: a stale token is rejected and the entry stays pending.
    [[nodiscard]] result<void> mark_delivered(std::uint64_t out_sender, std::uint64_t out_seq) {
        const auto it = std::find_if(outbox_.begin(), outbox_.end(), [&](const OutboxRecord& o) {
            return o.out_sender == out_sender && o.out_seq == out_seq;
        });
        if (it == outbox_.end()) return {};
        auto b = encode_durable(DeliveredRecord{out_sender, out_seq});
        if (!b) return std::unexpected(b.error());
        EventRecord er{next_seq_, std::move(*b)};
        auto rc = store_->append_batch(self_, fence_, std::span<const EventRecord>(&er, 1));
        if (!rc) return std::unexpected(rc.error());
        ++next_seq_;
        outbox_.erase(it);
        return {};
    }

    // Bound the in-memory dedup watermark and reply-cache maps (0 = unbounded, the default).
    // Oldest-TOUCHED-first eviction. TRADE-OFF (ADR-051): an evicted sender's next duplicate is no
    // longer recognized by RAM state, so effectively-once degrades to at-least-once for senders idle
    // beyond the cap; an evicted reply is not re-emitted. The durable log keeps everything; the cap
    // only bounds memory (recover() re-applies it). Size the cap >= active senders.
    void set_limits(std::size_t watermark_cap, std::size_t reply_cache_cap) {
        watermark_.set_cap(watermark_cap);
        reply_cache_.set_cap(reply_cache_cap);
    }
    [[nodiscard]] std::size_t watermark_evictions() const noexcept { return watermark_.evictions(); }
    [[nodiscard]] std::size_t reply_cache_size() const noexcept { return reply_cache_.size(); }
    [[nodiscard]] std::size_t reply_cache_evictions() const noexcept { return reply_cache_.evictions(); }

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
        const TypeKey dl_key{fingerprint_v<DeliveredRecord>};

        for (const EventRecord& rec : *cur) {
            auto hdr = peek_record_header(rec.record.data(), rec.record.size());
            if (!hdr) return std::unexpected(hdr.error());

            if (hdr->type == wm_key) {
                auto wm = decode_durable<WatermarkRecord>(rec.record);
                if (!wm) return std::unexpected(wm.error());
                std::uint64_t& hi = watermark_.touch(wm->sender);
                if (wm->seq > hi) hi = wm->seq;
            } else if (hdr->type == ob_key) {
                auto ob = decode_durable<OutboxRecord>(rec.record);
                if (!ob) return std::unexpected(ob.error());
                outbox_.push_back(std::move(*ob));
            } else if (hdr->type == rp_key) {
                auto rp = decode_durable<ReplyRecord>(rec.record);
                if (!rp) return std::unexpected(rp.error());
                reply_cache_.touch(rp->reply_to) = CachedReply{rp->in_seq, rp->body};
            } else if (hdr->type == dl_key) {
                // The delivered marker always follows its outbox record in log order.
                auto dl = decode_durable<DeliveredRecord>(rec.record);
                if (!dl) return std::unexpected(dl.error());
                std::erase_if(outbox_, [&](const OutboxRecord& o) {
                    return o.out_sender == dl->out_sender && o.out_seq == dl->out_seq;
                });
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
    detail::BoundedMap<std::uint64_t> watermark_;    // sender hash -> high-water-mark (capped, ADR-051)
    detail::BoundedMap<CachedReply> reply_cache_;    // sender hash -> recorded reply (capped)
    std::vector<OutboxRecord> outbox_;                              // committed pending outbound
};

// ============================================================================================
// AtLeastOnce retry + ack for remote sends (#19, ADR-051). Transport-agnostic go-back-N over a
// per-(sender, receiver) consecutive seq space starting at 1 (NOT the outbox's strided ids).
//   Sender  : enqueue() -> seq; pump(sink) re-sends unacked frames in order each call;
//             on_ack(cum) drops all frames <= cum (cumulative ack).
//   Receiver: accept(frame) -> Deliver (seq == wm+1: first time, advance), Duplicate (seq <= wm: re-ack,
//             do not redeliver) or Gap (a predecessor was lost: drop, sender re-sends in order).
//             The cumulative ack is `ack_for(sender)`. The watermark map is capped (LRU); a frame
//             carries `base` = oldest unacked seq so a receiver that evicted/never saw the sender can
//             re-admit it at `seq == base` (everything below base was already acked => delivered).
// Allocation: std::deque/std::string live on this retry path, NOT the mailbox hot path; no virtual/RTTI.
// ============================================================================================
struct ReliableFrame {
    std::uint64_t sender = 0;
    std::uint64_t seq = 0;
    std::uint64_t base = 0;  // oldest unacked seq at the sender when this frame was (re)sent
};

class ReliableSender {
public:
    explicit ReliableSender(std::uint64_t sender_hash) noexcept : sender_(sender_hash) {}

    std::uint64_t enqueue(std::string body) {
        pending_.push_back(Item{next_, std::move(body)});
        return next_++;
    }
    // Re-send up to `window` oldest unacked frames (0 = all) via
    // `sink(const ReliableFrame&, const std::string& body) -> void`. A lossy sink simply drops; the
    // next pump retries. Returns the number of frames sent.
    // RE-ENTRANCY (#27): the sink MAY call enqueue() on this sender (frames are located by seq at each
    // step, never by a saved iterator, so push_back is safe) and MAY call on_ack() (the ack is DEFERRED:
    // recorded, and applied once the outermost pump() returns, so the `body` reference handed to the
    // sink stays valid for the whole sink call).
    template <class Sink>
    std::size_t pump(Sink&& sink, std::size_t window = 0) {
        if (pending_.empty()) return 0;
        const bool outermost = !in_pump_;
        in_pump_ = true;
        PumpScope scope{*this, outermost};  // clears in_pump_ + applies deferred ack on every exit path
        std::size_t n = 0;
        std::uint64_t cursor = pending_.front().seq;
        while (window == 0 || n != window) {
            const auto it = std::lower_bound(pending_.begin(), pending_.end(), cursor,
                                             [](const Item& item, std::uint64_t s) { return item.seq < s; });
            if (it == pending_.end()) break;
            const std::uint64_t seq = it->seq;
            cursor = seq + 1;
            sink(ReliableFrame{sender_, seq, pending_.front().seq}, it->body);
            ++n;
        }
        return n;
    }
    void on_ack(std::uint64_t cumulative) {
        if (in_pump_) {  // inside a pump(): defer, so the in-flight body reference stays valid
            deferred_ack_ = std::max(deferred_ack_, cumulative);
            has_deferred_ack_ = true;
            return;
        }
        while (!pending_.empty() && pending_.front().seq <= cumulative) pending_.pop_front();
    }
    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t sender() const noexcept { return sender_; }

private:
    struct Item {
        std::uint64_t seq;
        std::string body;
    };
    // Scope guard for pump(): only the OUTERMOST pump clears in_pump_ and flushes the deferred ack
    // (a nested pump from inside a sink must not apply acks while the outer sink call is in flight).
    struct PumpScope {
        ReliableSender& s;
        bool outermost;
        ~PumpScope() {
            if (!outermost) return;
            s.in_pump_ = false;
            if (s.has_deferred_ack_) {
                s.has_deferred_ack_ = false;
                s.on_ack(s.deferred_ack_);
            }
        }
    };
    std::uint64_t sender_;
    std::uint64_t next_ = 1;
    std::deque<Item> pending_;
    bool in_pump_ = false;
    std::uint64_t deferred_ack_ = 0;
    bool has_deferred_ack_ = false;
};

class ReliableReceiver {
public:
    enum class Verdict : std::uint8_t { Deliver, Duplicate, Gap };

    // `max_senders` bounds the watermark map (0 = unbounded); see ADR-051 for the eviction trade-off.
    explicit ReliableReceiver(std::size_t max_senders = 0) { wm_.set_cap(max_senders); }

    [[nodiscard]] Verdict accept(const ReliableFrame& f) {
        const std::uint64_t* w = wm_.find(f.sender);
        if (w == nullptr) {
            if (f.seq != f.base) return Verdict::Gap;  // not the oldest unacked: wait for it
            wm_.touch(f.sender) = f.seq;
            return Verdict::Deliver;
        }
        if (f.seq <= *w) return Verdict::Duplicate;
        if (f.seq != *w + 1) return Verdict::Gap;
        wm_.touch(f.sender) = f.seq;
        return Verdict::Deliver;
    }
    // The cumulative ack to send back for `sender` (0 if unknown: acks nothing).
    [[nodiscard]] std::uint64_t ack_for(std::uint64_t sender) const noexcept {
        const std::uint64_t* w = wm_.find(sender);
        return w == nullptr ? 0 : *w;
    }
    [[nodiscard]] std::size_t tracked_senders() const noexcept { return wm_.size(); }
    [[nodiscard]] std::size_t evictions() const noexcept { return wm_.evictions(); }

private:
    detail::BoundedMap<std::uint64_t> wm_;
};

}  // namespace quark
