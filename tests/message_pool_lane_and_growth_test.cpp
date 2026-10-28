// Implements 003-Memory §MessagePool (#30, #31): per-pool dense lane assignment spreads P threads over
// P distinct partitions; an empty partition borrows from siblings before heap-growing, so a skewed
// producer never allocates while the pool as a whole still has free cells.
#include <atomic>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

#include "quark/detail/message_pool.hpp"

using namespace quark;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

void lanes_are_distinct() {
    constexpr unsigned kP = 4;
    // Burn some process-wide lane ids first: the old scheme depended on them, the new one must not.
    { detail::MessagePool other(64, 1); (void)other.current_thread_partition(); }
    detail::MessagePool pool(4096, kP);
    std::vector<std::size_t> got(kP);
    std::atomic<unsigned> arrived{0};
    std::vector<std::thread> th;
    for (unsigned t = 0; t < kP; ++t)
        th.emplace_back([&, t] {
            arrived.fetch_add(1);
            while (arrived.load() < kP) {}
            got[t] = pool.current_thread_partition();
        });
    for (auto& t : th) t.join();
    std::set<std::size_t> uniq(got.begin(), got.end());
    check(uniq.size() == kP, "4 threads on a 4-partition pool land on 4 distinct partitions");
    // Stable per thread within one pool.
    check(pool.current_thread_partition() == pool.current_thread_partition(), "lane is stable");
}

void skewed_producer_borrows_instead_of_growing() {
    constexpr std::size_t kCap = 1024;
    detail::MessagePool pool(kCap, 4);  // 256 cells per partition
    check(pool.total_cells() == kCap, "pre-sized");
    // One thread holds 3x its own partition's share (and below the pool total): must borrow, not grow.
    std::vector<detail::MessagePool::Slot> held;
    for (std::size_t i = 0; i < 800; ++i) held.push_back(pool.acquire(nullptr));
    check(pool.total_cells() == kCap, "no heap growth while sibling partitions still hold free cells");
    // Past the pool total it must grow (and only then).
    for (std::size_t i = 0; i < 400; ++i) held.push_back(pool.acquire(nullptr));
    check(pool.total_cells() > kCap, "grows once the whole pool is exhausted");
    for (auto& s : held) pool.reclaim(s.desc);
    detail::flush_current_thread_message_caches();
}
}  // namespace

int main() {
    lanes_are_distinct();
    skewed_producer_borrows_instead_of_growing();
    if (g_fail == 0) std::printf("message_pool_lane_and_growth_test OK\n");
    return g_fail == 0 ? 0 : 1;
}
