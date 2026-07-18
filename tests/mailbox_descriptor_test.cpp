// Tests 003-Memory §Descriptor — size ceiling, gen_state packing, and the single-thread
// FIFO + claim/complete state machine. ADR-004: 48-bit generation, one cache line.
// Uses an explicit check() (NOT <cassert> — asserts vanish under the Release NDEBUG build).
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"

using namespace quark;

namespace {
int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ++g_failures;
    }
}
}  // namespace

// Size ceiling: one cache line. Compile-time (also enforced in descriptor.hpp).
static_assert(sizeof(Descriptor) <= max_descriptor_size);
static_assert(sizeof(Descriptor) <= 64);

int main() {
    check(sizeof(Descriptor) <= 64, "descriptor fits one cache line");

    // --- gen_state packing round-trips {generation:48, state:4, flags:12}. ---
    {
        const std::uint64_t gen = (1ULL << 48) - 1;  // max 48-bit generation
        const std::uint16_t flags = 0x0ABC;          // 12-bit
        const std::uint64_t w = GenState::pack(gen, MsgState::Running, flags);
        check(GenState::generation_of(w) == gen, "generation round-trips (48-bit)");
        check(GenState::state_of(w) == MsgState::Running, "state round-trips");
        check(GenState::flags_of(w) == (flags & GenState::flags_mask), "flags round-trip");
        const std::uint64_t w2 = GenState::with_state(w, MsgState::Cancelled);
        check(GenState::generation_of(w2) == gen, "with_state preserves generation");
        check(GenState::flags_of(w2) == (flags & GenState::flags_mask), "with_state preserves flags");
        check(GenState::state_of(w2) == MsgState::Cancelled, "with_state flips only state");
        check(GenState::gen_max == ((1ULL << 48) - 1), "generation horizon is 48 bits");
    }

    // --- release() bumps the generation and resets state to Queued. ---
    {
        Descriptor d;
        check(d.state() == MsgState::Queued, "fresh descriptor is Queued");
        check(d.generation() == 0, "fresh descriptor generation is 0");
        d.release();
        check(d.generation() == 1, "release bumps generation");
        check(d.state() == MsgState::Queued, "release resets state to Queued");
    }

    // --- Single-thread FIFO through the mailbox + claim/complete. ---
    {
        DescriptorPool pool(8);
        Mailbox mb;
        check(mb.try_dequeue().status == DrainStatus::Empty, "empty queue reports Empty (not Busy)");

        constexpr int n = 5;
        for (int i = 0; i < n; ++i) {
            Descriptor* d = pool.acquire();
            check(d != nullptr, "pool hands out a descriptor");
            if (d == nullptr) break;
            d->message_id = MessageId{static_cast<std::uint64_t>(i)};
            mb.enqueue(d);
        }
        for (int i = 0; i < n; ++i) {
            DrainResult r = mb.try_dequeue();
            check(r.status == DrainStatus::Message, "dequeue yields a message");
            if (r.status != DrainStatus::Message) break;
            check(r.desc->message_id.value == static_cast<std::uint64_t>(i), "FIFO order preserved");
            check(r.desc->try_claim(), "Queued -> Running claim wins");
            check(r.desc->state() == MsgState::Running, "state is Running after claim");
            r.desc->complete();  // Running -> Completed
            check(r.desc->state() == MsgState::Completed, "state is Completed after complete()");
            pool.release(r.desc);  // back to pool, generation bumped
        }
        check(mb.try_dequeue().status == DrainStatus::Empty, "queue drains empty");
        check(pool.available() == pool.capacity(), "pool balanced after drain");
    }

    // --- Close-out seam: probe_has_work() read-only semantics (single-thread). ---
    {
        DescriptorPool pool(4);
        Mailbox mb;
        check(!mb.probe_has_work(), "empty mailbox: probe reports no work");
        Descriptor* d = pool.acquire();
        mb.enqueue(d);
        check(mb.probe_has_work(), "after enqueue: probe reports work");
        DrainResult r = mb.try_dequeue();
        check(r.status == DrainStatus::Message, "close-out drain yields the message");
        pool.release(r.desc);
        check(!mb.probe_has_work(), "after full drain: probe reports no work");
    }

    if (g_failures == 0)
        std::printf("mailbox_descriptor_test: OK (sizeof(Descriptor)=%zu)\n", sizeof(Descriptor));
    else
        std::printf("mailbox_descriptor_test: FAIL (%d checks failed)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
