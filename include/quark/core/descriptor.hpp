// Implements 003-Memory §Descriptor / §Cancellation — the pooled message descriptor:
// intrusive Vyukov link as first member, the packed {generation:48, state:4, flags:12}
// gen_state word, and the generation-gated claim/cancel/release CAS protocol.
// Hot path pinned by ADR-002/003/004 (48-bit generation, single packed CAS, tombstone reclaim).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "quark/core/config.hpp"

namespace quark {

struct Descriptor;

// The intrusive mailbox link. **Must be the first member of Descriptor** so a
// Descriptor* threads the Vyukov MPSC chain directly with no side allocation
// (003 §Mailbox structure). We store Descriptor* (not a bare MailNode*) throughout,
// so no down-cast is ever performed — the pointer-interconvertibility guard below is
// belt-and-suspenders over the offset-0 invariant.
struct MailNode {
    std::atomic<Descriptor*> next{nullptr};
};

// Lightweight message identity. The full messaging/addressing model is 006's; this is the
// stable per-message id the descriptor carries. Trivially-copyable value type.
struct MessageId {
    std::uint64_t value = 0;
    friend constexpr bool operator==(MessageId, MessageId) = default;
};

// Message lifecycle state — lives in the 4 state bits of gen_state (003 §Descriptor).
// Values 0..3 fit in 4 bits with room to grow WITHOUT stealing generation bits (ADR-004:
// the {48,4,12} bit budget is fixed).
enum class MsgState : std::uint8_t {
    Queued = 0,     // enqueued, awaiting drain
    Running = 1,    // claimed by the draining worker (Queued -> Running CAS)
    Completed = 2,  // handler finished; descriptor pending reclaim
    Cancelled = 3,  // late-cancel tombstone; skipped + reclaimed on drain
};

// --- Packed gen_state layout: {generation:48, state:4, flags:12} in one uint64_t --------
// The generation check and the state flip are ONE atomic CAS (003 §Cancellation): a
// concurrent release() (which bumps the generation) cannot slip between a check and a flip,
// closing the ADR-003 TOCTOU. 48-bit generation (ADR-004) pushes the reuse-wrap horizon past
// any process lifetime (a u32 wraps in ~24 h at 50 M msg/s and lets a stale handle wrongly
// cancel a live message).
struct GenState {
    static constexpr int flags_bits = 12;
    static constexpr int state_bits = 4;
    static constexpr int gen_bits = 48;
    static_assert(flags_bits + state_bits + gen_bits == 64, "packed word must be exactly 64 bits");

    static constexpr int state_shift = flags_bits;              // 12
    static constexpr int gen_shift = flags_bits + state_bits;   // 16

    static constexpr std::uint64_t flags_mask = (1ULL << flags_bits) - 1;                 // bits 0..11
    static constexpr std::uint64_t state_mask = ((1ULL << state_bits) - 1) << state_shift;  // bits 12..15
    static constexpr std::uint64_t gen_mask = ((1ULL << gen_bits) - 1);                    // 48-bit value
    static constexpr std::uint64_t gen_max = gen_mask;

    [[nodiscard]] static constexpr std::uint64_t pack(std::uint64_t generation, MsgState state,
                                                      std::uint16_t flags) noexcept {
        return ((generation & gen_mask) << gen_shift) |
               (static_cast<std::uint64_t>(state) << state_shift) |
               (static_cast<std::uint64_t>(flags) & flags_mask);
    }
    [[nodiscard]] static constexpr std::uint64_t generation_of(std::uint64_t word) noexcept {
        return (word >> gen_shift) & gen_mask;
    }
    [[nodiscard]] static constexpr MsgState state_of(std::uint64_t word) noexcept {
        return static_cast<MsgState>((word >> state_shift) & ((1ULL << state_bits) - 1));
    }
    [[nodiscard]] static constexpr std::uint16_t flags_of(std::uint64_t word) noexcept {
        return static_cast<std::uint16_t>(word & flags_mask);
    }
    [[nodiscard]] static constexpr std::uint64_t with_state(std::uint64_t word,
                                                            MsgState state) noexcept {
        return (word & ~state_mask) | (static_cast<std::uint64_t>(state) << state_shift);
    }
};

// --- Control flags (the 12-bit `flags` field of gen_state) -------------------------------------
// Reserved bits for engine-internal control descriptors — never set by user `tell`/`ask` traffic.
// Bit 0: this descriptor is a Deactivate control message (ADR-028 Phase 1). `drain_step` recognizes
// it via the SAME flags word already captured by try_claim()'s CAS (no extra memory load) and
// converts it into a private `retire_requested_` flag instead of dispatching to the handler table.
inline constexpr std::uint16_t kControlFlagDeactivate = 1u << 0;

// The fixed-size, pooled message metadata block. Payload lives SEPARATELY (003): the descriptor
// only references it. One cache line max — enforced by the static_assert at the bottom.
struct Descriptor {
    MailNode link;                          // offset 0 — intrusive Vyukov link (MUST be first)
    std::atomic<std::uint64_t> gen_state;   // packed {generation:48, state:4, flags:12}
    MessageId message_id;                   // per-message id (006)
    void* payload;                          // pointer into the shard payload arena/pool (003)
    std::int64_t deadline_ns;               // steady-clock deadline, ns (011); 0 = none
    std::uint64_t trace_id;                 // trace correlation id (009)
    std::uint32_t payload_size;             // payload byte length
    std::uint16_t priority;                 // priority-band metadata (022)
    std::uint16_t reserved;                 // padding / future use

    Descriptor() noexcept
        : link{},
          gen_state(GenState::pack(0, MsgState::Queued, 0)),
          message_id{},
          payload(nullptr),
          deadline_ns(0),
          trace_id(0),
          payload_size(0),
          priority(0),
          reserved(0) {}

    // --- gen_state accessors (relaxed reads; callers order via the queue edges) -----------
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return GenState::generation_of(gen_state.load(std::memory_order_acquire));
    }
    [[nodiscard]] MsgState state() const noexcept {
        return GenState::state_of(gen_state.load(std::memory_order_acquire));
    }

    // --- Claim (consumer/drain side): Queued -> Running -----------------------------------
    // Called by the draining worker when it reaches this descriptor. Returns true if the claim
    // won; false means a late cancel flipped Queued -> Cancelled first (the caller then reclaims
    // it as a tombstone — one free, no handler runs). ADR-004 C2: exactly one clean reclamation.
    //
    // The `observed_flags` overload (ADR-028 Phase 1) additionally reports the packed `flags` word
    // as it stood at the winning CAS — the SAME load the claim already performs, so a caller that
    // wants to recognize a control descriptor (e.g. kControlFlagDeactivate) pays no extra memory
    // load to do so.
    [[nodiscard]] bool try_claim(std::uint16_t* observed_flags) noexcept {
        std::uint64_t cur = gen_state.load(std::memory_order_acquire);
        for (;;) {
            if (GenState::state_of(cur) != MsgState::Queued) return false;  // cancelled under us
            const std::uint64_t desired = GenState::with_state(cur, MsgState::Running);
            if (gen_state.compare_exchange_weak(cur, desired, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                if (observed_flags) *observed_flags = GenState::flags_of(cur);
                return true;
            }
        }
    }
    [[nodiscard]] bool try_claim() noexcept { return try_claim(nullptr); }

    // --- Cancel (producer/external side, generation-gated): Queued -> Cancelled ------------
    // Writes the Cancelled tombstone ONLY IF handle_generation matches AND the state is still
    // Queued, in one packed CAS. A late cancel racing release() finds a generation mismatch (or
    // a lost CAS) and is a safe no-op — the ADR-002/003/004 fix for the bare-pointer UAF.
    [[nodiscard]] bool try_cancel(std::uint64_t handle_generation) noexcept {
        std::uint64_t cur = gen_state.load(std::memory_order_acquire);
        for (;;) {
            if (GenState::generation_of(cur) != (handle_generation & GenState::gen_max))
                return false;  // stale handle: descriptor already recycled
            if (GenState::state_of(cur) != MsgState::Queued)
                return false;  // already claimed/completed/cancelled
            const std::uint64_t desired = GenState::with_state(cur, MsgState::Cancelled);
            if (gen_state.compare_exchange_weak(cur, desired, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                return true;
        }
    }

    // --- Mark the running handler done: Running -> Completed -------------------------------
    void complete() noexcept {
        std::uint64_t cur = gen_state.load(std::memory_order_relaxed);
        gen_state.store(GenState::with_state(cur, MsgState::Completed), std::memory_order_release);
    }

    // --- Set the control flags (single-writer only) -----------------------------------------
    // NEVER call on a descriptor that is enqueued/reachable by another thread — this is a cold,
    // pre-post setup step (building a control descriptor like a Deactivate) or a test-harness hook,
    // not a hot-path or concurrent-write operation. A plain relaxed load+store is correct because
    // the caller is required to own the descriptor exclusively at the point of the call.
    void set_flags(std::uint16_t flags) noexcept {
        const std::uint64_t cur = gen_state.load(std::memory_order_relaxed);
        const std::uint64_t masked = cur & ~GenState::flags_mask;
        gen_state.store(masked | (static_cast<std::uint64_t>(flags) & GenState::flags_mask),
                        std::memory_order_relaxed);
    }

    // --- Release (cold path, pool return): bump generation, reset to Queued ----------------
    // Bumping the generation BEFORE the descriptor is handed back fences a late cancel against
    // reuse (a stale handle now mismatches). Single-writer: only the owning consumer reclaims.
    void release() noexcept {
        const std::uint64_t cur = gen_state.load(std::memory_order_relaxed);
        const std::uint64_t next_gen = (GenState::generation_of(cur) + 1) & GenState::gen_max;
        gen_state.store(GenState::pack(next_gen, MsgState::Queued, 0), std::memory_order_release);
    }
};

// A small, trivially-copyable reference to a queued message, stored BY VALUE wherever a message
// is named (cancellation tokens, reply routing) — 003 §MessageHandle. The 48-bit generation
// fences it against descriptor reuse.
struct MessageHandle {
    Descriptor* desc = nullptr;
    std::uint64_t generation = 0;  // 48-bit value (masked)

    [[nodiscard]] bool valid() const noexcept { return desc != nullptr; }

    // Cancel the referenced message iff it is still queued and the generation still matches.
    [[nodiscard]] bool cancel() const noexcept {
        return desc != nullptr && desc->try_cancel(generation);
    }
    friend constexpr bool operator==(const MessageHandle&, const MessageHandle&) = default;
};

// Snapshot the current handle for a descriptor (captures its live generation).
[[nodiscard]] inline MessageHandle handle_of(Descriptor* d) noexcept {
    return MessageHandle{d, d ? d->generation() : 0};
}

// --- Compile-time layout guards (003 §Mailbox structure; ADR-003 portability) ---------------
static_assert(std::is_standard_layout_v<Descriptor>);
static_assert(offsetof(Descriptor, link) == 0, "intrusive link must be the first member");
static_assert(std::is_standard_layout_v<std::atomic<Descriptor*>>);  // desc chaining cast safety
#ifdef __cpp_lib_is_pointer_interconvertible
// Not provided by libstdc++ as shipped with Clang 20.1 (feature-test macro undefined) — an
// unconditional use fails to compile there (ADR-003), so it is #ifdef-guarded; the
// is_standard_layout guards above are the always-on invariant.
static_assert(std::is_pointer_interconvertible_with_class(&Descriptor::link));
#endif
static_assert(sizeof(Descriptor) <= quark::max_descriptor_size,
              "descriptor must fit in one cache line (003, 023)");

}  // namespace quark
