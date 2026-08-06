// Tests ADR-046 — cross-node mailbox backpressure signalling: the `PeerCongestionGate`/
// `PeerCongestionTable` sender-side admission checkpoint (governance.hpp), and its wiring into
// `SwimMembership`'s watermark-crossing detection + edge-triggered `Congested` control frame
// (cluster.hpp), delivered to the peer's own `Transport::mark_congested` (transport.hpp).
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/governance.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{909};
    const NodeId peer{42};

    // ============================================================================================
    // Part 1 — PeerCongestionGate/PeerCongestionTable in isolation. Uncongested fast path always
    // Accepts; once congested, admission falls to the bucket and correctly Sheds past capacity;
    // the TTL self-heals with no explicit "clear".
    // ============================================================================================
    {
        PeerCongestionGate gate(/*capacity=*/3.0, /*refill_per_sec=*/0.0);
        check(gate.admit(0) == Admit::Accept, "uncongested gate: always Accept (fast path)", ok);
        check(gate.admit(1'000'000'000) == Admit::Accept,
              "uncongested gate: Accept regardless of elapsed time", ok);

        gate.mark_congested(/*until_ns=*/1'000);
        int accepted = 0;
        for (int i = 0; i < 5; ++i)
            if (gate.admit(/*now_ns=*/500) == Admit::Accept) ++accepted;
        check(accepted == 3, "congested gate: exactly `capacity` admits, then Shed (no refill)", ok);
        check(gate.admit(500) == Admit::Shed, "congested gate: stays Shed once the bucket is spent", ok);

        check(gate.admit(/*now_ns=*/1'000) == Admit::Accept,
              "TTL expiry self-heals: admit at/after `until_ns` reverts to Accept, no explicit clear", ok);
    }
    {
        PeerCongestionTable table(/*bucket_capacity=*/4.0, /*bucket_refill_per_sec=*/0.0);
        check(table.admit(peer, 0), "table: an unmarked peer is always admitted", ok);
        table.mark_congested(peer, /*until_ns=*/10'000);
        int accepted = 0;
        for (int i = 0; i < 10; ++i)
            if (table.admit(peer, /*now_ns=*/5'000)) ++accepted;
        check(accepted == 4, "table: lazily-created gate enforces the configured bucket capacity", ok);
        // A DIFFERENT peer was never marked congested — the table is per-peer, not global.
        check(table.admit(NodeId{peer.value + 1}, 5'000),
              "table: congestion on one peer never throttles an unrelated peer", ok);
    }

    // ============================================================================================
    // Part 2 — end to end: two SwimMembership + two LoopbackTransport (mirrors
    // capability_gossip_test.cpp's Part 3 harness). Node B's resident_total crosses the configured
    // watermark; B broadcasts `Congested` to A; A's OWN transport throttles further sends to B;
    // virtual-clock TTL expiry self-heals it with no second frame required.
    // ============================================================================================
    {
        LoopbackFabric fabric;
        VClock clk;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.ack_timeout_ns = 100'000'000;
        cfg.suspicion_timeout_ns = 500'000'000;
        cfg.gossip_fanout = 3;
        cfg.seed = 0xC0FFEE;
        cfg.congestion_high_watermark = 100;
        cfg.congestion_low_watermark = 50;
        cfg.congestion_window_ns = 1'000'000'000;   // 1s TTL
        cfg.congestion_refresh_ns = 400'000'000;

        LoopbackTransport ta(fabric, NodeId{1}), tb(fabric, NodeId{2});
        ta.set_clock(&vclock_read, &clk);
        tb.set_clock(&vclock_read, &clk);
        SwimMembership swimA(NodeId{1}, ta, cfg), swimB(NodeId{2}, tb, cfg);
        swimA.set_clock(&vclock_read, &clk);
        swimB.set_clock(&vclock_read, &clk);
        (void)swimA.admit(NodeId{2}, cluster);
        (void)swimB.admit(NodeId{1}, cluster);

        // Node B is overloaded; node A is quiet. Only B advertises/monitors pressure.
        std::atomic<std::uint64_t> b_resident_total{200};  // above congestion_high_watermark (100)
        swimB.set_pressure_gossip([&] { return b_resident_total.load(); },
                                   [](const std::vector<MailboxPressureEntry>&) {});

        check(ta.admit_send(NodeId{2}), "before any congestion signal, A admits sends to B", ok);

        // B's resident_total (200) is already past congestion_high_watermark (100) — the very first
        // tick fires the edge-triggered broadcast (loopback delivery is inline/synchronous).
        clk.now += 120'000'000;
        swimA.tick();
        swimB.tick();
        check(swimA.pressure_of(NodeId{2}) == 200,
              "A absorbed B's periodic resident_total piggyback, carried on the SAME Congested frame", ok);

        // Burst tolerance: the fast (uncongested) path never touched the bucket above, so it still
        // holds LoopbackTransport's default full capacity (64) — admit_send drains exactly that many
        // before it starts Shedding every subsequent call (real token-bucket semantics, not a flag).
        // `clk.now` does not advance across this loop, so refill contributes nothing mid-burst.
        int accepted_in_burst = 0;
        for (int i = 0; i < 80; ++i)
            if (ta.admit_send(NodeId{2})) ++accepted_in_burst;
        check(accepted_in_burst == 64,
              "B's Congested signal throttled A's transport to exactly its bucket capacity, then Shed", ok);
        check(!ta.admit_send(NodeId{2}), "the gate stays Shed while still within the congestion window", ok);

        // Virtual-clock TTL expiry: no second frame arrives, yet admission self-heals.
        clk.now += cfg.congestion_window_ns + 1;
        check(ta.admit_send(NodeId{2}),
              "TTL self-heals admission after `congestion_window_ns` elapses, no explicit clear frame", ok);
    }

    // ============================================================================================
    // Part 3 — concurrency stress reproducing ADR-046's own S1b over-admission-counting methodology:
    // N sender threads hammering PeerCongestionTable::admit() on one already-congested peer with a
    // small, non-refilling bucket. A correctly-synchronized bucket admits EXACTLY `capacity` total
    // across every thread combined — the ADR's own red-team found an unsynchronized bucket
    // reproducibly over-admits 19-70% beyond capacity; this proves the shipped `bucket_mu_` fix.
    // ============================================================================================
    {
        constexpr double kCapacity = 50.0;
        constexpr int kThreads = 4;
        constexpr int kItersPerThread = 5'000;
        PeerCongestionTable table(kCapacity, /*bucket_refill_per_sec=*/0.0);
        table.mark_congested(peer, /*until_ns=*/std::numeric_limits<std::int64_t>::max());

        std::atomic<std::uint64_t> total_accepted{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&] {
                for (int i = 0; i < kItersPerThread; ++i)
                    if (table.admit(peer, /*now_ns=*/0)) total_accepted.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& w : workers) w.join();

        check(total_accepted.load() == static_cast<std::uint64_t>(kCapacity),
              "concurrent admit() across N threads never over-admits beyond the bucket's capacity", ok);
    }

    std::printf("cross_node_backpressure_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
