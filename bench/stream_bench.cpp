// Hot-path microbenchmark for the 024 inbound-stream ingest path vs the 023 budgets (§Streaming
// ingest / ADR-005). Stream frames ride a DIFFERENT hot path than discrete `tell`: a batch drain off
// a per-stream credit ring, not one descriptor per frame. This bench measures that path directly:
//
//   1. SUSTAINED INGEST throughput — a single-thread producer/consumer ping-pong (fill to credit
//      depletion, drain a batch, repeat) over a bounded ring, M frames total, reported M frames/s/core
//      (023: goal ≥ 10 M/s / hard ≥ 4 M/s; ADR-005 measured 30–57 M/s).
//   2. PER-FRAME drain-step cost — amortized ns/frame inside a StreamBatch (023: p50 ≤ 100 ns / hard
//      ≤ 250 ns; ADR-005 p50 15–18 ns). A per-frame op is ~sub-clock, so it is reported AMORTIZED over
//      a tight drain loop, like the placement bench (the honest std-only measurement at that scale).
//   3. PER-FRAME vs DISCRETE `tell` — the ring's whole reason to exist: ingest must be materially
//      cheaper per item than a discrete mailbox round trip (023: ≥ 3× cheaper; ADR-005 12.6–19.7×).
//
// The ring is allocated ONCE (cold, up front) from a monotonic buffer — the steady path is 0 heap
// allocation (the 024 §0-alloc gate is a separate deterministic test; here we simply never allocate
// on the loop). Pin it: `taskset -c 0 build/bench/stream_bench` — single core, never saturate.
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "quark/core/shard_memory.hpp"  // DescriptorPool (discrete-tell contrast)
#include "quark/core/stream_activation.hpp"
#include "quark/core/stream_channel.hpp"
#include "quark/detail/hash.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

struct Frame {
    std::uint64_t id;
    std::uint64_t checksum;
};
static_assert(sizeof(Frame) <= kStreamInlineMax, "frame fits the 024 inline slot");

// 1+2) Sustained ingest throughput AND amortized per-frame drain cost, in one pass.
void bench_ingest() {
    constexpr std::uint64_t kFrames = 50'000'000;  // total frames streamed
    constexpr std::uint32_t kCapacity = 1024;      // ring size == max credit == max in-flight
    constexpr std::uint32_t kDrainBudget = 256;    // frames drained per consumer turn

    StreamChannel<Frame>::Config cfg;
    cfg.capacity = kCapacity;
    std::pmr::monotonic_buffer_resource mr;  // cold: the ring is allocated once, here
    StreamChannel<Frame> ch(cfg, &mr);

    std::uint64_t next_push = 0, processed = 0, sink = 0;

    const auto t0 = pal::clock::now();
    while (processed < kFrames) {
        while (next_push < kFrames) {
            Frame f{next_push, detail::splitmix64(next_push)};
            if (!ch.try_push(f)) break;   // credit depleted → drain to return credit (backpressure)
            ++next_push;
        }
        StreamBatch<Frame> batch(ch, kDrainBudget);
        while (const Frame* f = batch.next()) {
            sink += f->checksum ^ f->id;  // touch payload so the drain isn't elided
            ++processed;
            batch.retire();               // return one unit of credit to the producer
        }
    }
    const auto t1 = pal::clock::now();
    volatile std::uint64_t keep = sink;
    (void)keep;

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mps = static_cast<double>(kFrames) / secs / 1e6;
    const double ns_per = bench::ns_between(t0, t1) / static_cast<double>(kFrames);
    std::printf("1) sustained ingest (bounded %u-slot ring, budget %u, %llu frames):\n",
                kCapacity, kDrainBudget, static_cast<unsigned long long>(kFrames));
    std::printf("   %8.2f M frames/s/core  %s (023 goal ≥ 10 / hard ≥ 4)   (%.3fs)\n", mps,
                bench::thr_verdict(mps, bench::budget::tell_sustained_goal_mps,
                                   bench::budget::tell_sustained_floor_mps), secs);
    std::printf("2) amortized per-frame drain-step cost:\n");
    std::printf("   %8.2f ns/frame  %s (023 p50 ≤ 100 / hard ≤ 250)\n", ns_per,
                bench::lat_verdict(ns_per, 100.0, 250.0));
    std::printf("   (sink=%llu)\n", static_cast<unsigned long long>(sink));
}

// 3) The contrast the ring exists for: per-frame ingest vs a discrete mailbox enqueue→dequeue `tell`.
void bench_vs_tell() {
    constexpr std::uint64_t kOps = 50'000'000;

    // discrete tell: one descriptor per item through the 003 mailbox (the mailbox_bench peak path).
    DescriptorPool dpool(64);
    Mailbox mb;
    Descriptor* d = dpool.acquire();
    std::uint64_t c1 = 0;
    const auto a0 = pal::clock::now();
    for (std::uint64_t i = 0; i < kOps; ++i) {
        mb.enqueue(d);
        DrainResult r = mb.try_dequeue();
        if (r.status != DrainStatus::Message) { std::fprintf(stderr, "tell miss\n"); return; }
        c1 += reinterpret_cast<std::uintptr_t>(r.desc) & 1U;
        d = r.desc;
    }
    const auto a1 = pal::clock::now();
    const double tell_ns = bench::ns_between(a0, a1) / static_cast<double>(kOps);
    dpool.release(d);

    // stream ingest: one frame per item through the credit ring (batched drain).
    constexpr std::uint32_t kCapacity = 1024, kDrainBudget = 256;
    StreamChannel<Frame>::Config cfg;
    cfg.capacity = kCapacity;
    std::pmr::monotonic_buffer_resource mr;
    StreamChannel<Frame> ch(cfg, &mr);
    std::uint64_t next_push = 0, processed = 0, c2 = 0;
    const auto b0 = pal::clock::now();
    while (processed < kOps) {
        while (next_push < kOps) {
            if (!ch.try_push(Frame{next_push, next_push})) break;
            ++next_push;
        }
        StreamBatch<Frame> batch(ch, kDrainBudget);
        while (const Frame* f = batch.next()) { c2 += f->id; ++processed; batch.retire(); }
    }
    const auto b1 = pal::clock::now();
    const double frame_ns = bench::ns_between(b0, b1) / static_cast<double>(kOps);

    const double cheaper = frame_ns > 0 ? tell_ns / frame_ns : 0.0;
    std::printf("3) per-frame ingest vs discrete tell (both per-item, ns):\n");
    std::printf("   discrete tell   = %6.2f ns/item\n", tell_ns);
    std::printf("   stream ingest   = %6.2f ns/frame\n", frame_ns);
    std::printf("   ingest is %.1f× cheaper per item  %s (023 goal ≥ 3×)\n", cheaper,
                cheaper >= 3.0 ? "[goal]" : "[under 3×]");
    std::printf("   (c1=%llu c2=%llu)\n", static_cast<unsigned long long>(c1),
                static_cast<unsigned long long>(c2));
}

}  // namespace

int main() {
    std::printf("== Quark 024 streaming ingest bench (pin with taskset -c 0) ==\n");
    bench_ingest();
    std::printf("\n");
    bench_vs_tell();
    return 0;
}
