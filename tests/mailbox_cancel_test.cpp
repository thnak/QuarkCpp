// Tests 003-Memory §Cancellation — the generation-gated tombstone. ADR-002/003/004:
//  (1) a cancelled message is reclaimed exactly once, no handler runs, no double-free;
//  (2) a stale handle (post-release) cancel is a defined no-op (not a heap-use-after-free);
//  (3) a cancel racing the drain's Queued->Running claim-CAS resolves to exactly one clean
//      reclaim — never both a handler AND a tombstone, never a double free.
// Run under ASan (UAF on stale cancel) and TSan (the claim-vs-cancel race).
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"

using namespace quark;

namespace {
void assert_true(bool cond, const char* what, bool& all_ok) {
    if (!cond) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        all_ok = false;
    }
}
}  // namespace

int main() {
    bool all_ok = true;

    // --- (1) Deterministic cancel -> tombstone, reclaimed exactly once, no handler. ----------
    {
        DescriptorPool pool(4);
        Mailbox mb;
        Descriptor* d = pool.acquire();
        MessageHandle h = handle_of(d);
        mb.enqueue(d);

        assert_true(h.cancel(), "cancel of a queued message wins", all_ok);   // Queued->Cancelled
        assert_true(!h.cancel(), "second cancel is a no-op", all_ok);         // already cancelled

        DrainResult r = mb.try_dequeue();
        int handler_ran = 0, free_count = 0;
        if (r.status == DrainStatus::Message) {
            if (r.desc->try_claim()) {   // must FAIL — it is a tombstone
                ++handler_ran;           // (would run the handler)
            } else {
                // tombstone: skip the handler, reclaim once.
            }
            pool.release(r.desc);
            ++free_count;
        }
        assert_true(handler_ran == 0, "no handler runs on a cancelled message", all_ok);
        assert_true(free_count == 1, "cancelled descriptor reclaimed exactly once", all_ok);
        assert_true(pool.available() == pool.capacity(), "pool balanced after tombstone", all_ok);
    }

    // --- (2) Stale handle: cancel after release() is a generation-mismatch no-op (no UAF). ----
    {
        DescriptorPool pool(4);
        Descriptor* d = pool.acquire();
        MessageHandle stale = handle_of(d);          // captures generation g
        pool.release(d);                             // bumps generation to g+1
        // The descriptor may be handed back out; cancelling through the stale handle must be a
        // safe no-op, NOT a write into recycled memory (ASan would flag a UAF on the bug form).
        Descriptor* reused = pool.acquire();
        assert_true(reused == d, "pool hands the same slot back", all_ok);
        assert_true(!stale.cancel(), "stale-generation cancel is a no-op", all_ok);
        assert_true(reused->state() == MsgState::Queued, "reused descriptor untouched by stale cancel",
                    all_ok);
        pool.release(reused);
    }

    // --- (3) Race: cancel vs the drain claim-CAS -> exactly one clean reclaim per descriptor. -
    {
        constexpr unsigned kN = 200'000;
        std::vector<Descriptor> descs(kN);
        std::vector<MessageHandle> handles(kN);
        std::vector<std::uint8_t> released(kN, 0);
        Mailbox mb;

        for (unsigned i = 0; i < kN; ++i) {
            handles[i] = handle_of(&descs[i]);
            mb.enqueue(&descs[i]);
        }

        std::atomic<bool> go{false};
        std::atomic<std::uint64_t> cancelled_won{0};

        // Canceller thread races the consumer, cancelling every handle. It NEVER frees.
        std::jthread canceller([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (unsigned i = 0; i < kN; ++i)
                if (handles[i].cancel()) cancelled_won.fetch_add(1, std::memory_order_relaxed);
        });

        go.store(true, std::memory_order_release);

        // Single consumer (main): claim-or-tombstone, and free EACH descriptor exactly once.
        std::uint64_t handled = 0, tombstoned = 0, drained = 0, double_free = 0;
        std::uint64_t idle = 0;
        constexpr std::uint64_t kStall = 2'000'000'000ULL;
        while (drained < kN) {
            DrainResult r = mb.try_dequeue();
            if (r.status != DrainStatus::Message) {
                if (++idle > kStall) { std::fprintf(stderr, "STALL cancel-race\n"); return 1; }
                continue;
            }
            idle = 0;
            const auto idx = static_cast<unsigned>(r.desc - descs.data());
            if (r.desc->try_claim()) {
                ++handled;   // won the claim: run the handler (elided), then reclaim
            } else {
                ++tombstoned;  // cancel won: skip handler, reclaim as tombstone
            }
            if (released[idx]) ++double_free;   // must stay 0 — exactly one reclaim per descriptor
            released[idx] = 1;
            ++drained;
        }

        // Join the canceller BEFORE reading its counter: the drain can observe a cancel's effect
        // (try_claim fails on the Queued->Cancelled CAS) before the canceller runs its *separate*
        // cancelled_won.fetch_add, so tombstoned can momentarily lead cancelled_won. The engine
        // invariant (double_free==0, exactly-once) holds regardless; this join makes the harness's
        // cross-thread ACCOUNTING check race-free (the counts are a true invariant only once every
        // cancel attempt has completed).
        canceller.join();
        const bool ok = (handled + tombstoned == kN) && double_free == 0 &&
                        (tombstoned == cancelled_won.load());
        assert_true(ok, "claim-vs-cancel resolves to exactly one clean reclaim per descriptor",
                    all_ok);
        std::printf("  cancel-race: handled=%" PRIu64 " tombstoned=%" PRIu64 " cancelled_won=%"
                    PRIu64 " double_free=%" PRIu64 "\n",
                    handled, tombstoned, cancelled_won.load(), double_free);
    }

    std::printf("mailbox_cancel_test: %s\n", all_ok ? "OK" : "FAIL");
    return all_ok ? 0 : 1;
}
