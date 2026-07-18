// Hot-path microbenchmark for the 016 wire fast path vs the 023 budgets (ADR-016 §"Encode-budget
// evidence"). Three encodes of the same named 24 B POD into a COLD caller buffer, single core:
//
//   A) memcpy baseline   — raw 24-byte copy (the near-memcpy floor).
//   B) tagless encode    — the wire fast path (values in tag order, packed).
//   C) tagged encode     — the canonical TLV (must be measurably slower — fast path not vacuous).
//
// 023 line 60: tagless encode <= 200 ns goal / <= 500 ns Hard, near-memcpy. ADR-016 measured
// p99 25-28 ns (~20x under Hard). Cold buffer = a fresh 64 B line strided over 64 MB (> L3) so
// the real write-back store cost is present. Pin it: `taskset -c 0 build/bench/serialize_bench`.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quark/core/serialize.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct Pod {
    std::uint64_t id;
    std::uint32_t qty;
    std::uint32_t flags;
    double price;
};

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

// Prevent the compiler from eliding the encode / hoisting the input.
template <class T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}
inline void clobber() { asm volatile("" : : : "memory"); }

QUARK_SERIALIZE(Pod, (1, id), (2, qty), (3, flags), (4, price))

}  // namespace

int main() {
    std::printf("== Quark 016 Serialization bench (pin with taskset -c 0) ==\n");
    std::printf("   Pod: sizeof=%zu  tagless=%zu B  fingerprint=%016llx\n", sizeof(Pod),
                [] { Pod z{}; return tagless_size(z); }(),
                static_cast<unsigned long long>(fingerprint_v<Pod>));

    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kSamples = 1'000'000;

    // Cold destination: stride a 64 B line across 64 MB (> L3) so each encode hits a cold line.
    constexpr std::size_t kArena = 64u * 1024u * 1024u;
    constexpr std::size_t kStride = 64;
    std::vector<std::byte> arena(kArena);
    const std::size_t kLines = kArena / kStride;

    std::vector<double> memcpy_ns, tagless_ns, tagged_ns;
    memcpy_ns.reserve(kSamples);
    tagless_ns.reserve(kSamples);
    tagged_ns.reserve(kSamples);

    Pod base{0x0102030405060708ULL, 0xaabbccddU, 0x11223344U, 3.14159};

    for (std::uint64_t i = 0; i < kWarmup + kSamples; ++i) {
        Pod p = base;
        p.id = i;  // vary input every iteration
        std::byte* dst = arena.data() + (i % kLines) * kStride;

        // A) memcpy baseline (same 24 bytes).
        do_not_optimize(p);
        const auto a0 = pal::clock::now();
        std::memcpy(dst, &p, sizeof(Pod));
        clobber();
        const auto a1 = pal::clock::now();
        do_not_optimize(*dst);

        // B) tagless encode.
        dst = arena.data() + ((i + 1) % kLines) * kStride;
        do_not_optimize(p);
        const auto b0 = pal::clock::now();
        const std::size_t bn = encode_tagless(p, dst);
        clobber();
        const auto b1 = pal::clock::now();
        do_not_optimize(bn);
        do_not_optimize(*dst);

        // C) tagged encode.
        dst = arena.data() + ((i + 2) % kLines) * kStride;
        do_not_optimize(p);
        const auto c0 = pal::clock::now();
        auto cn = encode_tagged(p, dst, kStride);
        clobber();
        const auto c1 = pal::clock::now();
        do_not_optimize(cn.has_value());
        do_not_optimize(*dst);

        if (i >= kWarmup) {
            memcpy_ns.push_back(std::chrono::duration<double, std::nano>(a1 - a0).count());
            tagless_ns.push_back(std::chrono::duration<double, std::nano>(b1 - b0).count());
            tagged_ns.push_back(std::chrono::duration<double, std::nano>(c1 - c0).count());
        }
    }

    auto report = [](const char* name, std::vector<double>& v) {
        const double p50 = percentile(v, 0.50);
        const double p99 = percentile(v, 0.99);
        const double p999 = percentile(v, 0.999);
        std::printf("  %-16s p50=%7.2f  p99=%7.2f  p999=%7.2f ns\n", name, p50, p99, p999);
        return p99;
    };

    std::printf("cold buffer (64 B line strided over 64 MB > L3):\n");
    const double mc99 = report("A memcpy", memcpy_ns);
    const double tl99 = report("B tagless", tagless_ns);
    const double tg50 = percentile(tagged_ns, 0.50);
    report("C tagged", tagged_ns);

    const double tl50 = percentile(tagless_ns, 0.50);
    std::printf("\n016/023 gate (tagless p99):\n");
    std::printf("  tagless p99 = %.2f ns   %s   %s\n", tl99,
                tl99 <= 500 ? "[<=500 Hard OK]" : "[MISS Hard]",
                tl99 <= 200 ? "[<=200 goal OK]" : "[over goal]");
    std::printf("  near-memcpy factor (tagless p99 / memcpy p99) = %.2fx  %s\n",
                mc99 > 0 ? tl99 / mc99 : 0.0,
                (mc99 > 0 && tl99 / mc99 <= 5.0) ? "[<=5x OK]" : "[check]");
    std::printf("  fast-path-not-vacuous (tagged p50 / tagless p50) = %.2fx  %s\n",
                tl50 > 0 ? tg50 / tl50 : 0.0,
                (tl50 > 0 && tg50 > tl50) ? "[tagged slower OK]" : "[check]");
    return 0;
}
