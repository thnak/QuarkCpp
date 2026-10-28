// Implements 003-Memory §Mailbox — the intrusive Vyukov MPSC mailbox; ADR-002/003/004 hot path.
//
// The queue node *is* the pooled Descriptor (its first member is the intrusive link), so
// enqueue/dequeue move one Descriptor* with no side allocation. Many producers enqueue
// wait-free with one `tail_.exchange(seq_cst)` + one link store; a SINGLE consumer drains a
// consumer-private `head_` with plain loads (0 cross-core RMW on the steady drain path, 023).
//
// This layer owns the QUEUE PRIMITIVE + the Descriptor gen_state only. It does NOT own the
// worker, the scheduler, or the actor exec-state (those are 001/002). The exec-state wakeup +
// seq_cst Dekker CLOSE-OUT is exposed here as a documented seam (see §Close-out seam) for 002
// to drive; the mailbox never wires wakeup to emptiness — Vyukov emptiness is non-linearizable.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "quark/core/config.hpp"
#include "quark/core/descriptor.hpp"
#include "pal/pal.hpp"

namespace quark {

// Result of a single consumer drain step. Emptiness is NON-LINEARIZABLE (ADR-002), so the
// transient publish window is surfaced as a distinct `Busy` — never fold it into `Empty`.
enum class DrainStatus : std::uint8_t {
    Message,  // `desc` is a dequeued descriptor
    Empty,    // head reached tail; no producer mid-publish
    Busy,     // a producer did tail_.exchange but has not yet linked its node (bounded spin, 002)
};

struct DrainResult {
    Descriptor* desc = nullptr;  // valid iff status == Message
    DrainStatus status = DrainStatus::Empty;
};

// The per-actor MPSC mailbox. Owns ordering only — never payload memory (003, ADR-002).
// Non-copyable, non-movable (contains atomics and self-referential head_/stub_).
class Mailbox {
public:
    Mailbox() noexcept
        : head_(&stub_), tail_(&stub_) {
        // stub starts unlinked; head_ and tail_ both anchor on it (canonical Vyukov init).
        stub_.link.next.store(nullptr, std::memory_order_relaxed);
    }

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    Mailbox(Mailbox&&) = delete;
    Mailbox& operator=(Mailbox&&) = delete;

    // --- Producer side (many threads) -----------------------------------------------------
    // Wait-free, allocation-free, ABA-free by construction (never compares an address).
    // Compiles to `1 lock xchg + 2 stores` on x86 (ADR-004 F1). The exchange is **at least acq_rel,
    // never release**: the acquire half orders the predecessor's node-init before the successor's
    // link store (publication ordering) so the queue is correct on weakly-ordered ISAs too (ADR-003).
    // The PRODUCER's exchange is seq_cst (ADR-066): it is also the store half of the producer side of
    // the close-out Dekker rendezvous, so it must sit in the single total order S with the consumer's
    // seq_cst relinquish store + probe. Identical instruction to acq_rel on x86-64 (`xchg`) and AArch64
    // (`swpal` / `ldaxr`+`stlxr`) — and it is what lets the producer-side StoreLoad fence go entirely
    // (formerly elided on x86 only and a real `dmb ish` per enqueue on AArch64).
    QUARK_ALWAYS_INLINE void enqueue(Descriptor* d) noexcept {
        link_push(d, std::memory_order_seq_cst);
    }

    // --- Consumer side (exactly one worker at a time — the single-executor invariant) ------
    // Pops one descriptor with plain loads on the consumer-private `head_`. Steady-state pops do
    // ZERO cross-core RMW (023 hard budget); the ONE `tail_.exchange` fires only at the
    // empty-boundary stub re-arm (ADR-004 F3), never on the multi-node drain path.
    [[nodiscard]] DrainResult try_dequeue() noexcept {
        Descriptor* front = head_;
        Descriptor* next = front->link.next.load(std::memory_order_acquire);

        if (front == &stub_) {
            if (next == nullptr) {
                // Only the stub is visible from head_. Either truly empty, or a producer has
                // done its exchange but not yet linked (Busy). Read-only disambiguation:
                if (tail_.load(std::memory_order_acquire) == &stub_)
                    return {nullptr, DrainStatus::Empty};
                return {nullptr, DrainStatus::Busy};  // producer mid-publish
            }
            // Skip the stub and continue from the first real node.
            head_ = next;
            front = next;
            next = front->link.next.load(std::memory_order_acquire);
        }

        if (next != nullptr) {
            head_ = next;
            return {front, DrainStatus::Message};
        }

        // `front` is (apparently) the last node. If tail_ has moved past it, a producer is
        // mid-publish linking front->next — that is Busy, not Empty.
        Descriptor* tail = tail_.load(std::memory_order_acquire);
        if (front != tail) {
            return {nullptr, DrainStatus::Busy};
        }

        // Re-arm the stub so the queue is never truly empty (Vyukov invariant). This is the ONE
        // consumer-side exchange (occupancy-1 boundary only). Then front->next -> &stub_.
        link_push(&stub_, std::memory_order_acq_rel);  // consumer re-arm: not a Dekker half
        next = front->link.next.load(std::memory_order_acquire);
        if (next != nullptr) {
            head_ = next;
            return {front, DrainStatus::Message};
        }
        return {nullptr, DrainStatus::Busy};
    }

    // --- Close-out seam for 002 (the exec-state wakeup + seq_cst Dekker rendezvous) --------
    //
    // The mailbox does NOT own exec_state; 002 does. The full release close-out (002 §Release
    // close-out) is a Dekker StoreLoad rendezvous spelled as seq_cst OPERATIONS (ADR-066):
    //
    //   consumer:  exec_state.store(Idle, seq_cst);    probe_has_work()  [tail_.load(seq_cst)]
    //   producer:  enqueue(d)  [tail_.exchange(seq_cst)];  exec_state CAS (seq_cst success+failure)
    //
    // All four operations are in the single total order S, so "the consumer's probe misses the
    // enqueue AND the producer's CAS misses the relinquish" is impossible (C++20 [atomics.order]) —
    // the property ADR-004 C3 proved with the former store + standalone-fence + load form (0/300k
    // lost with the StoreLoad ordering, >0 without). Unlike a standalone fence, every edge here is an
    // atomic operation ThreadSanitizer models. The store half lives on the exec-state cell
    // (ExecStateCell::release_to_idle_sc / park_sc / retire_to_dormant); the load half is below.

    // Read-only work probe used by the consumer's close-out (step 2 of the seq_cst Dekker). Does NOT
    // dequeue and does NOT touch the consumer-private head_ — the caller must re-acquire ownership
    // (CAS Idle->Running) before touching head_ again. Returns true if there is work or a producer is
    // mid-publish (stay awake); false only when the queue is genuinely empty.
    [[nodiscard]] bool probe_has_work() const noexcept {
        // `tail_ != &stub_` is the complete wakeup predicate: try_dequeue returns Empty (the ONLY
        // path to DrainedEmpty -> close_out -> here) exactly when head_ == tail_ == &stub_, so the
        // probe is always evaluated with tail_ re-armed to the stub. A producer's enqueue moves tail_
        // OFF the stub via `tail_.exchange` BEFORE it links the node, so this single atomic load
        // catches every wakeup — including the mid-publish window where the link store has not landed.
        //
        // The former `|| head_->link.next != nullptr` term was (a) REDUNDANT — with head_ == &stub_ it
        // reduced to stub_.link.next, which is non-null only after tail_ has already moved off the stub
        // — and (b) a DATA RACE: it READ the consumer-private head_ AFTER close_out (activation.hpp)
        // had already released Running, so it raced the NEXT owner lane's try_dequeue write of head_
        // (TSan-proven under scheduling stress). Reading only the atomic tail_ removes that race and
        // preserves the lost-wakeup Dekker guarantee (ADR-004 C3; proven by sched_no_lost_wakeup).
        //
        // seq_cst (ADR-066): this is the LOAD half of the consumer's side of the Dekker — it must be
        // in S after the caller's seq_cst relinquish store. A plain `mov` on x86-64 and the same
        // `ldar` an acquire load uses on baseline AArch64.
        return tail_.load(std::memory_order_seq_cst) != &stub_;
    }

private:
    // The shared publish operation: one unconditional exchange + one release link store. Used by
    // enqueue() (seq_cst — a Dekker half) and by the consumer-side stub re-arm (acq_rel). `order` is
    // always a constant at both always-inlined call sites.
    QUARK_ALWAYS_INLINE void link_push(Descriptor* d, std::memory_order order) noexcept {
        d->link.next.store(nullptr, std::memory_order_relaxed);
        Descriptor* prev = tail_.exchange(d, order);
        prev->link.next.store(d, std::memory_order_release);
    }

    // Consumer-private drain cursor — plain loads only. Its cross-worker visibility rides the
    // actor exec-state CAS (001/002), NOT an atomic of its own. Kept off tail_'s / stub_'s lines.
    Descriptor* head_;

    // Producer-shared tail — the only contended atomic. Own cache line to isolate producer
    // exchange traffic from the consumer-private head_.
    QUARK_CACHE_ALIGNED std::atomic<Descriptor*> tail_;

    // The stub sentinel on its OWN cache line: a producer writes the stub's link on every
    // idle->active re-arm, so a co-located stub false-shares head_ and taxes the dequeue hot
    // path (ADR-004, perf-c2c confirmed). Separation enforced by the static_assert below.
    QUARK_CACHE_ALIGNED Descriptor stub_;

    // Layout guard in a member-function body: a complete-class context with private access, so
    // offsetof can name head_/stub_ (Mailbox is standard-layout — all data members share access,
    // no virtual/bases — so offsetof is well-defined and warning-free). Non-template member
    // bodies are compiled at class definition, so the assert always fires.
    static void assert_layout() noexcept {
        static_assert(offsetof(Mailbox, stub_) - offsetof(Mailbox, head_) >= quark::cache_line_size,
                      "stub must sit on its own cache line, off consumer-private head_ (ADR-004)");
    }
};

static_assert(std::is_standard_layout_v<Mailbox>);

}  // namespace quark
