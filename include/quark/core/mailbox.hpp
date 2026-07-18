// Implements 003-Memory §Mailbox — the intrusive Vyukov MPSC mailbox; ADR-002/003/004 hot path.
//
// The queue node *is* the pooled Descriptor (its first member is the intrusive link), so
// enqueue/dequeue move one Descriptor* with no side allocation. Many producers enqueue
// wait-free with one `tail_.exchange(acq_rel)` + one link store; a SINGLE consumer drains a
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
    // Compiles to `1 lock xchg + 2 stores` on x86 (ADR-004 F1). The exchange is **acq_rel, not
    // release**: the acquire half orders the predecessor's node-init before the successor's link
    // store (publication ordering) so the queue is correct on weakly-ordered ISAs too (ADR-003).
    // On x86-TSO the xchg is already a full barrier, so acq_rel is free here.
    QUARK_ALWAYS_INLINE void enqueue(Descriptor* d) noexcept { link_push(d); }

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
        link_push(&stub_);
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
    // close-out) is:
    //
    //   consumer:  exec_state.store(Idle, release);  <consumer_close_out_fence()>;  probe_has_work()
    //   producer:  enqueue(d);                       <producer_close_out_fence()>;  exec_state.load(acquire)
    //
    // Both fences are the seq_cst StoreLoad half of a Dekker rendezvous — the one reordering
    // x86-TSO permits (a store buffered past a later load to a different address). Proven 0/300k
    // lost with the fence, >0 without (ADR-004 C3). The two halves are provided as seams so the
    // caller cannot forget the fence; exec_state itself stays with the actor (001/002).

    // Consumer half: issue the StoreLoad fence AFTER storing exec_state=Idle, then probe. Always
    // emits a real barrier (the exec_state release-store is a plain store, not an RMW).
    QUARK_ALWAYS_INLINE static void consumer_close_out_fence() noexcept {
        quark::pal::store_load_barrier();
    }

    // Producer half: the StoreLoad fence BEFORE loading exec_state after enqueue().
    // ELIDED on x86-TSO — enqueue()'s `tail_.exchange(acq_rel)` is itself a full StoreLoad
    // barrier there, so this is zero instructions (ADR-004). A real barrier is retained off x86.
    QUARK_ALWAYS_INLINE static void producer_close_out_fence() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        // elided — the acq_rel exchange already fenced StoreLoad on x86-TSO.
#else
        quark::pal::store_load_barrier();  // TODO(arm64): re-gate the close-out under weak memory
#endif
    }

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
        return tail_.load(std::memory_order_acquire) != &stub_;
    }

private:
    // The shared publish operation: one unconditional acq_rel exchange + one release link store.
    // Used by enqueue() and by the consumer-side stub re-arm.
    QUARK_ALWAYS_INLINE void link_push(Descriptor* d) noexcept {
        d->link.next.store(nullptr, std::memory_order_relaxed);
        Descriptor* prev = tail_.exchange(d, std::memory_order_acq_rel);
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
