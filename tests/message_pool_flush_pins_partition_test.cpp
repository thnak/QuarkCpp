// Issue #15 regression (ADR-037 amendment): a magazine flush that has already passed the liveness
// lock() must keep the Partition alive even if the MessagePool is destroyed at that exact instant.
// The test hook destroys the pool between lock() and the partition mutex acquisition; with the old
// empty-tag token this was a use-after-free (caught by ASan), now the flush pins the partition.
#define QUARK_MESSAGE_POOL_TEST_HOOKS 1
#include <cstdio>
#include <memory>
#include <thread>

#include "quark/detail/message_pool.hpp"

using quark::detail::MessagePool;

namespace {
std::unique_ptr<MessagePool> g_pool;
void destroy_pool_now() { g_pool.reset(); }
}  // namespace

int main() {
    int rounds_ok = 0;
    for (int round = 0; round < 200; ++round) {
        g_pool = std::make_unique<MessagePool>(256, 2);
        std::thread t([] {
            quark::detail::t_after_token_lock_hook = &destroy_pool_now;
            // Leave a non-empty magazine, then let thread exit run the flush (hook fires inside it).
            auto s = g_pool->acquire(nullptr);
            g_pool->reclaim(s.desc);
        });
        t.join();
        if (g_pool == nullptr) ++rounds_ok;  // the hook destroyed the pool mid-flush and nothing crashed
        g_pool.reset();
    }
    std::printf(rounds_ok == 200 ? "message_pool_flush_pins_partition_test: PASS\n"
                                 : "message_pool_flush_pins_partition_test: FAIL (%d/200)\n",
                rounds_ok);
    return rounds_ok == 200 ? 0 : 1;
}
