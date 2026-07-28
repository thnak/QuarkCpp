// Mailbox backlog-buildup vs steady-state queue-depth benchmark — dimensions 14 and 22 of the
// mailbox benchmark suite. Both axes are about queue depth but are DISTINCT (per the commissioning
// brief): #14 is a TRANSIENT event (a burst piles up, then drains — does the drain-side tail
// degrade with how deep the pile got?); #22 is STEADY-STATE (the resident depth stays constant the
// whole run — does cache locality / structural overhead change with how big that steady depth is?).
//
//  14) Tail latency vs backlog — this is the EXACT axis that disqualified the REX/LIFO-reversal
//      mailbox lineage across 4 ADR rounds (decisions/ADR-031-mailbox-mpsc-hot-path-r8-judgment.md:
//      REX-CAS/C's own design summary concedes "O(batch) reversal breaches the 015/023 50µs p999
//      tail-latency ceiling"). The shipped Vyukov mailbox (mailbox.hpp) walks the chain with plain
//      `head_`/`next` loads — O(1) amortized per node, no reversal step — so per-dequeue-call
//      latency should stay FLAT across backlog depths 64/1024/16384/262144, not grow with depth.
//      This benchmark builds each backlog depth cold (single producer, no contention — isolating
//      the QUEUE STRUCTURE's drain behavior, not multi-producer timing), then times EACH individual
//      try_dequeue() call while draining it, and reports p50/p99/p999 PER DEPTH.
//
//  22) Queue depth (steady-state) — a single thread maintains a CONSTANT resident depth D (dequeue
//      one, immediately enqueue a fresh one, forever) and reports steady per-op throughput +
//      latency at several D. Unlike #14, nothing ever fully drains; this isolates whether a bigger
//      *standing* backlog changes steady per-op cost (e.g. cache locality: the resident window's
//      descriptors may no longer fit in L1/L2 at large D).
//
// Informational only (bench/ci_bench_gate.sh's [MISS]/[goal] tokens are not printed here) — always
// exits 0. Single-threaded throughout (no producer/consumer contention to control for) — safe to
// run unpinned or `taskset -c 0`.
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "bench/bench_harness.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/mailbox.hpp"
#include "pal/pal.hpp"

using namespace quark;

namespace {

// ---- Dimension 14: tail latency vs (transient) backlog depth ------------------------------------
void bench_backlog_tail_latency() {
    std::printf("=== [dim 14] tail latency vs backlog depth (transient burst-then-drain) ===\n");
    const std::vector<std::uint64_t> depths = {64, 1024, 16384, 262144};

    for (std::uint64_t depth : depths) {
        std::vector<Descriptor> descs(depth);
        for (std::uint64_t i = 0; i < depth; ++i) descs[i].message_id = MessageId{i};

        Mailbox mb;
        // Build the backlog cold (untimed) — single producer, no draining yet.
        for (std::uint64_t i = 0; i < depth; ++i) mb.enqueue(&descs[i]);

        // Now drain it, timing EACH try_dequeue() call individually.
        std::vector<double> ns;
        ns.reserve(depth);
        std::uint64_t checksum = 0;
        for (std::uint64_t drained = 0; drained < depth; ++drained) {
            const auto t0 = pal::clock::now();
            DrainResult r = mb.try_dequeue();
            const auto t1 = pal::clock::now();
            // Single producer, single (this) consumer thread, strictly sequential — every call
            // must resolve to Message; Busy/Empty here would indicate a genuine structural bug,
            // not a transient race (there is no concurrent producer to race against).
            if (r.status != DrainStatus::Message) {
                std::fprintf(stderr, "  unexpected drain status %d at depth=%" PRIu64 " index=%" PRIu64 "\n",
                             static_cast<int>(r.status), depth, drained);
                return;
            }
            ns.push_back(bench::ns_between(t0, t1));
            checksum += r.desc->message_id.value;
        }

        bench::Stats s = bench::summarize(ns);
        std::printf("  depth=%7" PRIu64 "  p50=%7.1f ns   p99=%8.1f ns   p999=%9.1f ns   mean=%7.1f ns"
                    "   (checksum=%" PRIu64 ")\n",
                    depth, s.p50, s.p99, s.p999, s.mean, checksum);
    }
    std::printf("  [info] flat p50/p99/p999 across depths (no growth with backlog size) confirms the\n"
                "  Vyukov chain-walk has no O(batch) reversal step — the exact pathology that\n"
                "  disqualified the REX/LIFO-reversal lineage (ADR-002/020/029/031) against the\n"
                "  015/023 50us p999 tail-latency ceiling. A steep p999 growth with depth here would\n"
                "  be a genuine regression against that invariant, not expected/tolerated noise.\n");
}

// ---- Dimension 22: throughput/latency at steady-state resident queue depth ----------------------
void bench_steady_state_depth() {
    std::printf("\n=== [dim 22] steady-state resident queue depth (dequeue-one/enqueue-one loop) ===\n");
    const std::vector<std::uint64_t> steady_depths = {1, 64, 1024, 16384};
    constexpr std::uint64_t kOpsPerDepth = 2'000'000;

    for (std::uint64_t depth : steady_depths) {
        // Pool large enough to hold the resident window; recycle indices round-robin so the same
        // physical Descriptor objects are reused (0 allocation on the steady loop, as the real
        // engine's pooled descriptors would be).
        std::vector<Descriptor> descs(depth);
        for (std::uint64_t i = 0; i < depth; ++i) descs[i].message_id = MessageId{i};

        Mailbox mb;
        for (std::uint64_t i = 0; i < depth; ++i) mb.enqueue(&descs[i]);  // prime the steady window

        std::vector<double> ns;
        ns.reserve(kOpsPerDepth);
        std::uint64_t next_tag = depth;
        std::uint64_t checksum = 0;

        const auto wall0 = pal::clock::now();
        for (std::uint64_t op = 0; op < kOpsPerDepth; ++op) {
            const auto t0 = pal::clock::now();
            DrainResult r = mb.try_dequeue();
            const auto t1 = pal::clock::now();
            // Strictly sequential single-thread loop (no concurrent producer) — always Message.
            if (r.status != DrainStatus::Message) {
                std::fprintf(stderr, "  unexpected drain status %d at depth=%" PRIu64 " op=%" PRIu64 "\n",
                             static_cast<int>(r.status), depth, op);
                return;
            }
            checksum += r.desc->message_id.value;
            r.desc->message_id = MessageId{next_tag++};
            mb.enqueue(r.desc);  // immediately restore the resident depth
            ns.push_back(bench::ns_between(t0, t1));
        }
        const auto wall1 = pal::clock::now();

        bench::Stats s = bench::summarize(ns);
        const double secs = std::chrono::duration<double>(wall1 - wall0).count();
        const double mps = secs > 0.0 ? static_cast<double>(kOpsPerDepth) / secs / 1e6 : 0.0;
        std::printf("  depth=%6" PRIu64 "  %7.2f M msg/s   p50=%7.1f ns   p99=%8.1f ns   p999=%9.1f ns"
                    "   (checksum=%" PRIu64 ")\n",
                    depth, mps, s.p50, s.p99, s.p999, checksum);
    }
    std::printf("  [info] a per-op cost that grows with steady-state depth would indicate cache-\n"
                "  locality pressure from a large resident working set (dim 20's \"Cache\" axis is the\n"
                "  dedicated deep-dive on this; this table is the queue-depth-indexed summary).\n");
}

}  // namespace

int main() {
    std::printf("== Quark mailbox backlog/queue-depth bench (dims 14, 22; single-thread, no contention) ==\n");
    bench_backlog_tail_latency();
    bench_steady_state_depth();
    return 0;
}
