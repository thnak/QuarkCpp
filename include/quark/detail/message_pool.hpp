// Implements 006-Messaging-and-Addressing §tell + 003-Memory §Descriptor/§Payload — the send-side
// message source: a pooled Descriptor paired 1:1 with inline payload storage, so `tell`/`ask`
// construct a message with 0 heap allocation on the hot path (measured, ADR-007 F1 / 023).
//
// Each cell is {Descriptor, destroy-thunk, inline payload}. The Descriptor is the first member so a
// `Descriptor*` IS the cell address (offset-0, pointer-interconvertible — mirrors 003's mailbox
// intrusive-link rule); the free-list threads through the Descriptor's intrusive link while pooled.
// Reclaim (the Activation ReclaimSink, 002/003) runs the payload destructor, then recycles the cell.
//
// THREAD SAFETY: acquire() runs on arbitrary producer (send) lanes; reclaim() runs on the single
// per-shard drain lane. The free-list is mutex-guarded — a lock, never a heap allocation, so the
// 0-hot-path-alloc guarantee holds. This is NOT the drain path (the 0-cross-core-RMW invariant is
// about the sequential drain, 023), so a producer-side lock is in-budget.
//
// PARTITIONING (the 003/022 "per-producer caches" seam): a single shared free-list serializes every
// concurrent producer on one mutex regardless of which actor they target (measured: throughput
// flatlines under N producers whether messages spread across 100 or 100,000 actors — the pool, not
// the mailbox, was the bottleneck; see ADR-020's flagged-but-unproven "acquire() under
// producer/consumer imbalance" gap). `num_partitions` (ctor, default 1 == today's exact behavior)
// splits the free-list into independent partitions, one per calling thread's lane id (mod
// num_partitions). Each partition owns its OWN cell storage, so there is no second, pool-wide lock
// for growth. reclaim() — which runs on whichever worker drains the target shard, almost certainly a
// DIFFERENT thread than the one that acquired the cell — routes back to the cell's ORIGINAL
// partition (stamped once at construction), never the reclaiming thread's own lane, so partitions
// never need to coordinate with each other. This is thread-keyed, not shard-keyed: it removes
// producer-vs-producer contention but does not (yet) give the target shard's cache-locality that
// 003-Memory.md's "each shard owns its allocator" describes — a shard-keyed follow-up is a separate,
// larger seam (Engine-owned per-shard pools, spawn()'s reclaim-sink wiring, PostCourier).
//
// TLS ACQUIRE/RECLAIM MAGAZINE (ADR-037 — settles the residual mutex tax the partitioning above does
// NOT remove): even partitioned, acquire()/reclaim() still took `Partition::mu` once EACH — 2 lock/
// unlock round trips per message. A `design-debate-prove` debate (3 designs: lock-free CAS free
// list, this magazine, per-worker-lane SPSC return rings) proved this design the clean winner (8/8
// claims CORRECT vs the CAS design's uncontended cost regressing 15-25% and the SPSC design's
// isolated round-trip claim failing to clear its bar) — see ADR-037 for the full record. Each
// partition's mutex+free_head is UNCHANGED; every calling thread additionally keeps a small,
// non-atomic, per-partition "magazine" (a plain `Descriptor*[64]` array) in thread_local storage.
// acquire() pops from the calling thread's magazine and only touches the partition mutex when the
// magazine is empty, refilling up to 32 cells in one critical section (falling back to today's exact
// single grow_one() only if the partition itself is genuinely out of cells — the 0-alloc-once-pre-
// sized contract is unchanged bit-for-bit). reclaim() pushes into the drain lane's magazine for the
// cell's home partition and only touches that partition's mutex once the magazine hits capacity,
// splicing 32 cells back in one critical section. Net effect: the mutex is touched ~1/32nd as often
// (measured: 62,502 vs 2,000,000 lock() calls over 1M cycles), for a measured 1.27x-3.0x round-trip
// throughput improvement across producer counts {1,2,4}.
//
// A magazine's `bound` Partition* is guarded against use-after-free / cross-pool address reuse by a
// `std::weak_ptr<PartitionToken>` liveness check (owner_before-based, control-block identity, never a
// raw address compare) — a Partition* recycled by the allocator into an unrelated LATER pool can
// never satisfy this check against a stale binding from an earlier, now-destroyed pool. This closed
// a fatal UAF the ADR-037 red team found in the original raw-`Partition*` draft (reproduced under
// TSan+ASan; fixed and reproven clean, 0/60 sanitizer runs). If the owning pool is already gone when
// a magazine would otherwise flush (thread exit, or a rebind to a different partition), the residual
// cells are safely FORFEITED (not touched, not leaked memory — just returned to neither free list)
// rather than dereferencing freed Partition state.
//
// POOL LIFETIME CONTRACT (new, additive): a `MessagePool` is safe to destroy at any time — no design
// here ever dereferences a dead pool — but for the STRONG guarantee that every cell is physically
// back on some partition's free_head (exact steady-state cell-count conservation), every thread that
// ever called acquire()/reclaim() on it must call `flush_current_thread_message_caches()` before the
// pool is torn down, OR must have already exited (a thread's `LocalCacheTable` destructor flushes
// every bound, non-empty magazine automatically on thread exit — this is why ordinary Engine
// shutdown, which joins every worker thread, needs no explicit call). Skipping quiesce is SAFE (no
// UAF) but may leave up to `kCap` (64) cells per thread un-returned. Corollary: a partition's
// pre-sized capacity must budget headroom beyond the exact working-set count — up to `kCap` cells can
// sit resident in each thread that touches the partition rather than on its free_head at any instant.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "quark/core/activation.hpp"  // ReclaimSink
#include "quark/core/config.hpp"      // cache_line_size, QUARK_CACHE_ALIGNED
#include "quark/core/descriptor.hpp"

namespace quark::detail {

// Type-erased payload destructor. `void(*)(void*) noexcept` matches the pool's stored thunk.
template <class Msg>
void destroy_payload(void* p) noexcept {
    static_cast<Msg*>(p)->~Msg();
}

// A calling thread's stable lane id within this process, assigned once (lazily) from a single
// monotonic counter shared by every MessagePool. A pool reduces it mod its own partition count, so
// the same physical thread can land on different partition indices in different pools — harmless,
// since a partition only needs to be stable PER THREAD WITHIN ONE POOL, never coordinated globally.
// Race-free by construction: the thread_local is never touched cross-thread, and the atomic
// fetch_add only needs distinct values (no ordering with anything else), so relaxed suffices.
inline std::atomic<std::uint32_t> g_next_pool_lane_id{0};
inline thread_local std::uint32_t t_pool_lane_id = 0xFFFF'FFFFu;  // sentinel: not yet assigned

[[nodiscard]] inline std::uint32_t pool_lane_id() noexcept {
    if (t_pool_lane_id == 0xFFFF'FFFFu)
        t_pool_lane_id = g_next_pool_lane_id.fetch_add(1, std::memory_order_relaxed);
    return t_pool_lane_id;
}

// Empty tag whose control block gives each Partition a generation-safe identity: a raw Partition*
// recycled by the allocator into an unrelated later pool can never satisfy owner_before() against a
// stale weak_ptr bound to the old pool's token (ADR-037 S2 — closes the magazine UAF class).
struct PartitionToken {};

class MessagePool {
public:
    // Inline payload capacity per cell. Sized for the local send path (small messages + an ask
    // envelope carrying a Responder). Oversized messages are a compile error at the send site.
    static constexpr std::size_t kMaxPayload = 192;
    static constexpr std::size_t kPayloadAlign = 16;

    struct Slot {
        Descriptor* desc = nullptr;
        void* payload = nullptr;
    };

    // `num_partitions` defaults to 1 — every existing single-arg call site keeps today's exact
    // behavior (one mutex, one free-list, fronted by the same TLS magazine). Pass > 1 to spread
    // producer-side contention across independent partitions (see the file banner).
    explicit MessagePool(std::size_t capacity, std::size_t num_partitions = 1)
        : num_partitions_(num_partitions == 0 ? 1 : num_partitions),
          partitions_(std::make_unique<Partition[]>(num_partitions_)) {
        for (std::size_t i = 0; i < num_partitions_; ++i)
            partitions_[i].alive_token = std::make_shared<PartitionToken>();
        for (std::size_t i = 0; i < capacity; ++i) grow_one(partitions_[i % num_partitions_]);
    }
    MessagePool(const MessagePool&) = delete;
    MessagePool& operator=(const MessagePool&) = delete;

    // Hot path (send lane): pop a cell from the calling thread's magazine, refilling from the
    // partition's mutex-guarded free list only when the magazine is empty (ADR-037). Grows cold if
    // the partition is genuinely exhausted (pre-size to avoid it) — same cold-miss contract as before.
    [[nodiscard]] Slot acquire(void (*destroy)(void*) noexcept) noexcept {
        Partition& p = partitions_[pool_lane_id() % num_partitions_];
        LocalCache& lc = local_cache_for(p);
        if (lc.count == 0) refill(lc, p);
        Descriptor* d = lc.cells[--lc.count];
        Cell* c = cell_of(d);
        c->destroy = destroy;
        d->link.next.store(nullptr, std::memory_order_relaxed);
        return Slot{d, c->payload};
    }

    // Reclaim (drain lane): run the payload destructor (which, for an unanswered ask, fails the
    // ReplyCell — reply-before-teardown), bump the descriptor generation, and push into the calling
    // (reclaiming) thread's magazine for the cell's ORIGINALLY-acquired partition (never the
    // reclaiming thread's own lane — the reclaiming worker is very likely not the thread that
    // acquired this cell). Only touches the partition mutex once that magazine hits capacity.
    void reclaim(Descriptor* d) noexcept {
        Cell* c = cell_of(d);
        if (c->destroy != nullptr && d->payload != nullptr) c->destroy(d->payload);
        c->destroy = nullptr;
        d->release();  // bump generation, reset to Queued (003 §Cancellation)
        Partition* p = c->home;  // stamped once at grow_one, immutable after
        LocalCache& lc = local_cache_for(*p);
        if (lc.count == LocalCache::kCap) flush_partial(lc);
        lc.cells[lc.count++] = d;
    }

    static void reclaim_thunk(Descriptor* d, void* self) noexcept {
        static_cast<MessagePool*>(self)->reclaim(d);
    }
    // The Activation reclamation seam (002/003): wire this into the Activation so completed /
    // tombstoned / torn-down messages return their cell here.
    [[nodiscard]] ReclaimSink sink() noexcept { return ReclaimSink{&reclaim_thunk, this}; }

    // ADR-037: flush the CALLING thread's TLS magazines across every MessagePool it has touched.
    // See quark::detail::flush_current_thread_message_caches() (free-function wrapper) for when a
    // caller needs this — thread exit already flushes automatically, so this is only for a thread
    // that keeps running past a specific pool's lifetime and wants exact conservation on it.
    static void flush_thread_local_caches() noexcept {
        for (auto& lc : t_cache_mag.slots)
            if (lc.bound != nullptr && lc.count != 0) LocalCacheTable::flush_all_locked(lc);
    }

private:
    struct Partition;  // fwd — Cell::home points to it

    struct alignas(kPayloadAlign) Cell {
        Descriptor desc;                        // offset 0 — Descriptor* == Cell*
        void (*destroy)(void*) noexcept;         // payload destructor thunk
        Partition* home = nullptr;               // which partition to reclaim() back into
        alignas(kPayloadAlign) unsigned char payload[kMaxPayload];
    };
    static_assert(offsetof(Cell, desc) == 0, "Descriptor must be the first member (offset-0 cast)");
    static_assert(std::is_standard_layout_v<Cell>);

    // One independent shard of pool state. Cache-line aligned so two partitions' mutexes never
    // false-share. Each partition owns its OWN cell storage — no cross-partition shared mutable
    // state exists, so growth never needs a second, pool-wide lock.
    struct QUARK_CACHE_ALIGNED Partition {
        std::mutex mu;
        Descriptor* free_head = nullptr;
        std::vector<std::unique_ptr<Cell>> cells;
        std::shared_ptr<PartitionToken> alive_token;  // constructed once, at pool-ctor time (ADR-037)
    };

    // Descriptor is the first member (offset 0), so a Descriptor* IS a Cell*. Route through void*
    // (not reinterpret_cast) so the offset-0 identity carries no -Wcast-align noise.
    static Cell* cell_of(Descriptor* d) noexcept {
        return static_cast<Cell*>(static_cast<void*>(d));
    }

    void grow_one(Partition& p) {  // caller holds p.mu (or is the ctor, single-threaded)
        auto c = std::make_unique<Cell>();
        c->destroy = nullptr;
        c->home = &p;
        Descriptor* d = &c->desc;
        d->link.next.store(p.free_head, std::memory_order_relaxed);
        p.free_head = d;
        p.cells.push_back(std::move(c));
    }

    // ---- ADR-037: the TLS magazine fronting each partition's mutex+free_head -----------------
    struct alignas(quark::cache_line_size) LocalCache {
        Partition* bound = nullptr;
        std::weak_ptr<PartitionToken> bound_token;  // generation-safe identity of `bound`
        std::uint32_t count = 0;
        static constexpr std::uint32_t kCap = 64;
        static constexpr std::uint32_t kRefillBatch = 32;
        static constexpr std::uint32_t kReturnBatch = 32;
        Descriptor* cells[kCap];
    };

    // Direct-mapped table of magazines, ONE per thread, shared across every MessagePool the calling
    // thread ever touches (keyed by Partition* address, not by pool). A thread touching more than
    // ~kSlots/2 distinct live partitions degrades toward the pre-magazine mutex cost via hash-
    // collision eviction — safe, just less amortized (ADR-037 residual risk; not exercised by the
    // repo's current shard/worker-count scale).
    struct LocalCacheTable {
        static constexpr std::uint32_t kSlots = 64;
        LocalCache slots[kSlots];

        static void flush_all_locked(LocalCache& lc) noexcept {
            auto tok = lc.bound_token.lock();
            if (!tok) {
                // Owning pool is already gone: the partition (and its mutex) may be freed. Safely
                // forfeit the residual cells instead of touching freed memory (ADR-037 S2).
                lc.count = 0;
                lc.bound = nullptr;
                lc.bound_token.reset();
                return;
            }
            std::lock_guard<std::mutex> g(lc.bound->mu);
            for (std::uint32_t i = 0; i < lc.count; ++i) {
                Descriptor* d = lc.cells[i];
                d->link.next.store(lc.bound->free_head, std::memory_order_relaxed);
                lc.bound->free_head = d;
            }
            lc.count = 0;
        }

        ~LocalCacheTable() {
            for (auto& lc : slots)
                if (lc.bound != nullptr && lc.count != 0) flush_all_locked(lc);
        }
    };
    static thread_local LocalCacheTable t_cache_mag;  // out-of-line below (needs a complete-class
                                                       // context for LocalCache's default members)

    static std::uint32_t slot_for(Partition* p) noexcept {
        auto bits = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
        std::uint64_t h = (bits >> 6) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::uint32_t>(h >> (64 - 6));  // 6 bits -> 64 slots
    }

    static LocalCache& local_cache_for(Partition& p) noexcept {
        LocalCache& lc = t_cache_mag.slots[slot_for(&p)];
        // Slot match requires BOTH the raw pointer AND live-token agreement (owner_before, control-
        // block identity) against p.alive_token — closes the address-reuse attack: a freed-and-
        // recycled Partition* at the same address can never satisfy this check because its token's
        // control block differs from the stale lc.bound_token's.
        bool same = (lc.bound == &p) &&
                    !lc.bound_token.owner_before(p.alive_token) &&
                    !p.alive_token.owner_before(lc.bound_token);
        if (!same) {
            if (lc.bound != nullptr && lc.count != 0) LocalCacheTable::flush_all_locked(lc);
            lc.bound = &p;
            lc.bound_token = p.alive_token;
            lc.count = 0;
        }
        return lc;
    }

    void refill(LocalCache& lc, Partition& p) {
        std::lock_guard<std::mutex> g(p.mu);
        std::uint32_t n = 0;
        while (n < LocalCache::kRefillBatch) {
            if (p.free_head == nullptr) {
                if (n > 0) break;
                grow_one(p);
            }
            Descriptor* d = p.free_head;
            p.free_head = d->link.next.load(std::memory_order_relaxed);
            lc.cells[n++] = d;
        }
        lc.count = n;
    }

    static void flush_partial(LocalCache& lc) {
        std::lock_guard<std::mutex> g(lc.bound->mu);
        for (std::uint32_t i = 0; i < LocalCache::kReturnBatch; ++i) {
            Descriptor* d = lc.cells[--lc.count];
            d->link.next.store(lc.bound->free_head, std::memory_order_relaxed);
            lc.bound->free_head = d;
        }
    }

    std::size_t num_partitions_;
    std::unique_ptr<Partition[]> partitions_;
};

inline thread_local MessagePool::LocalCacheTable MessagePool::t_cache_mag{};

// Quantum-bounded proactive quiesce (ADR-037): flushes every bound, non-empty magazine slot for the
// CALLING thread through the normal mutex-guarded splice. Not required for safety (thread exit
// already flushes automatically via LocalCacheTable's destructor — ordinary Engine shutdown, which
// joins every worker thread, needs no explicit call) — this is for a thread that keeps running past
// the lifetime of a specific MessagePool it used (e.g. it will next touch a DIFFERENT, unrelated
// pool) and wants exact cell-count conservation on the old pool rather than forfeiting a resident
// batch. Also bounds reclaimed-cell staleness under bursty traffic (fewer than kReturnBatch messages
// between idle periods would otherwise sit resident indefinitely) if a caller invokes this at a
// natural checkpoint (e.g. once per drain quantum) — not currently wired into the Engine's scheduler
// loop (that integration is unproven/out of scope for ADR-037; see OpenQuestions.md).
inline void flush_current_thread_message_caches() noexcept {
    MessagePool::flush_thread_local_caches();
}

}  // namespace quark::detail
