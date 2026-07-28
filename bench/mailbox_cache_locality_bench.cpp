// Cache-locality benchmark — dimension 20 of the mailbox benchmark suite. Quantifies the delta
// between a HOT descriptor pool (a small working set that stays resident in L1/L2 across repeated
// enqueue/dequeue cycles) and a COLD one (a working set deliberately sized past this machine's L2/L3
// so each access is a real cache miss) — using the SAME DescriptorPool/Mailbox code path both times,
// varying only how large the touched working set is.
//
// Detects L2/L3 sizes from /sys/devices/system/cpu/cpu0/cache/index*/size when available (Linux);
// falls back to reasonable defaults otherwise. Informational only — no [MISS]/[goal] tokens; always
// exits 0. Single-threaded (no producer/consumer contention to control for) — safe to run unpinned
// or `taskset -c 0`.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// Parse a /sys cache size file like "256K" or "8192K" or "30M" into bytes. Returns 0 on failure.
std::uint64_t read_cache_size_bytes(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string s;
    f >> s;
    if (s.empty()) return 0;
    char unit = s.back();
    std::uint64_t mult = 1;
    std::string digits = s;
    if (unit == 'K' || unit == 'k') { mult = 1024; digits.pop_back(); }
    else if (unit == 'M' || unit == 'm') { mult = 1024 * 1024; digits.pop_back(); }
    return static_cast<std::uint64_t>(std::atoll(digits.c_str())) * mult;
}

// Best-effort L2/L3 detection via /sys (index2/index3 are conventionally L2/L3 on Linux/x86).
// Falls back to 1 MiB / 8 MiB if /sys is unavailable (container, non-Linux, etc).
struct CacheSizes { std::uint64_t l2 = 1024ull * 1024, l3 = 8ull * 1024 * 1024; };
CacheSizes detect_cache_sizes() {
    CacheSizes cs;
    for (int idx = 0; idx <= 3; ++idx) {
        const std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx);
        std::ifstream lvl(base + "/level");
        std::ifstream typ(base + "/type");
        if (!lvl || !typ) continue;
        int level = 0; lvl >> level;
        std::string type; typ >> type;
        const std::uint64_t bytes = read_cache_size_bytes(base + "/size");
        if (bytes == 0) continue;
        if (level == 2 && (type == "Unified" || type == "Data")) cs.l2 = bytes;
        if (level == 3 && (type == "Unified" || type == "Data")) cs.l3 = bytes;
    }
    return cs;
}

// Run kOps enqueue->dequeue round trips cycling through `working_set` distinct descriptors (a
// bigger working_set forces more of the touched memory to be genuinely resident/non-resident in
// cache). Returns per-op ns.
//
// Access order is a FIXED RANDOM PERMUTATION of the working set, replayed repeatedly — NOT
// sequential/strided index%working_set. A hardware prefetcher trivially hides latency for a
// predictable sequential stream regardless of how large the touched span is (confirmed during this
// bench's own development: the naive sequential version showed a flat 1.00x cold/hot ratio, an
// artifact of the prefetcher, not evidence the descriptor pool has no cache-locality cost). A
// permuted-but-repeating access order defeats stride prediction while still exercising exactly
// `working_set` distinct cache lines, which is the actual variable under test.
double run(std::size_t working_set, std::uint64_t ops) {
    std::vector<Descriptor> descs(working_set);
    for (std::size_t i = 0; i < working_set; ++i) descs[i].message_id = MessageId{i};

    std::vector<std::size_t> order(working_set);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::mt19937_64 rng(0xC0FFEE);
    std::shuffle(order.begin(), order.end(), rng);

    Mailbox mb;
    std::uint64_t checksum = 0;
    const auto t0 = pal::clock::now();
    for (std::uint64_t i = 0; i < ops; ++i) {
        Descriptor* d = &descs[order[i % working_set]];
        mb.enqueue(d);
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) { std::fprintf(stderr, "cache bench: drain miss\n"); return -1.0; }
        checksum += r.desc->message_id.value;
    }
    const auto t1 = pal::clock::now();
    (void)checksum;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(ops);
}

}  // namespace

int main() {
    CacheSizes cs = detect_cache_sizes();
    std::printf("== Quark mailbox cache-locality bench (dim 20; single-thread, pin -c 0 for cleanest numbers) ==\n");
    std::printf("detected L2=%.1f MiB  L3=%.1f MiB (via /sys, or defaults if unavailable)\n\n",
                static_cast<double>(cs.l2) / (1024.0 * 1024.0), static_cast<double>(cs.l3) / (1024.0 * 1024.0));

    constexpr std::uint64_t kOps = 20'000'000;
    const std::size_t desc_bytes = sizeof(Descriptor);

    // HOT: a tiny working set (a handful of descriptors) that trivially stays in L1 the whole run.
    const std::size_t hot_n = 8;
    // COLD: enough descriptors that the touched span is well past L3 (2x L3 by byte footprint).
    const std::size_t cold_n = static_cast<std::size_t>((cs.l3 * 2) / desc_bytes) + 1;

    const double hot_ns = run(hot_n, kOps);
    const double cold_ns = run(cold_n, kOps);

    std::printf("HOT  (working set = %zu descriptors, %zu B, fits in L1)   : %6.2f ns/op\n", hot_n,
                hot_n * desc_bytes, hot_ns);
    std::printf("COLD (working set = %zu descriptors, %.1f MiB, > 2x L3)  : %6.2f ns/op\n", cold_n,
                static_cast<double>(cold_n * desc_bytes) / (1024.0 * 1024.0), cold_ns);
    std::printf("  cold/hot ratio: %.2fx\n", hot_ns > 0 ? cold_ns / hot_ns : 0.0);
    std::printf("  [info] this isolates descriptor-pool cache locality alone (enqueue/dequeue over a\n"
                "  varying working set), distinct from dim 22's steady-QUEUE-DEPTH sweep (which\n"
                "  keeps the SAME small resident set touched repeatedly regardless of depth).\n");
    return 0;
}
