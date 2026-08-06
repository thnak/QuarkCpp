// Tests ADR-045 — `CapabilityRegistry` (capability_registry.hpp) and its wiring into `SwimMembership`
// (`set_capability_gossip`, cluster.hpp): a node's advertised `NodeCapabilities` (capabilities.hpp)
// converges to peers purely via the EXISTING SWIM gossip channel, the same way ADR-040's revocation
// gossip does (see security_revocation_sweep_test.cpp's Part 3, which this file's Part 3 mirrors).
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/capability_registry.hpp"
#include "quark/core/cluster.hpp"
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
    const ClusterId cluster{777};

    // ============================================================================================
    // Part 1 — CapabilityRegistry in isolation: publish/pull/merge, incarnation + local_seq
    // precedence, byte-lexicographic collision tiebreak, evict_dead, empty-view-for-unknown-node.
    // ============================================================================================
    {
        CapabilityRegistry reg;
        MembershipView empty_base;  // caller-provided base; the registry itself is membership-agnostic
        CapabilityView v0 = reg.view(empty_base);
        check(!v0.capabilities_of(NodeId{1}).has_flag("gpu"),
              "fresh registry: unknown node has no capabilities", ok);

        // --- publish_local -> pull() claims it and it becomes visible via view() immediately. ---
        reg.publish_local(NodeCapabilities{Flag{"gpu"}, Label{"zone", "eu"}, Scalar{"weight", 2.0}});
        const auto packed = reg.pull(NodeId{1}, /*self_incarnation=*/5, /*max_bytes=*/4096);
        check(packed.size() == 1 && packed[0].node == NodeId{1} && packed[0].incarnation == 5,
              "pull() packs exactly the freshly-claimed local entry", ok);
        CapabilityView v1 = reg.view(empty_base);
        check(v1.capabilities_of(NodeId{1}).has_flag("gpu"),
              "self capabilities visible via view() immediately after pull(), no round trip needed", ok);
        check(v1.capabilities_of(NodeId{1}).label("zone") == "eu", "label round-trips through encode/decode", ok);
        check(v1.capabilities_of(NodeId{1}).scalar("weight") == 2.0, "scalar round-trips through encode/decode", ok);

        // A second pull() with nothing newly published re-packs the SAME stored entry (no local churn).
        const auto packed_again = reg.pull(NodeId{1}, 5, 4096);
        check(packed_again.size() == 1 && packed_again[0].local_seq == packed[0].local_seq,
              "a pull() with no new local publish does not bump local_seq", ok);

        // --- merge(): a stale (lower) incarnation from elsewhere never overrides. ---
        CapabilityDigestEntry stale{NodeId{1}, /*incarnation=*/1, /*local_seq=*/999,
                                    detail::encode_node_capabilities(NodeCapabilities{Flag{"stale"}})};
        reg.merge({stale});
        check(reg.view(empty_base).capabilities_of(NodeId{1}).has_flag("gpu"),
              "a lower-incarnation entry never overrides a higher-incarnation one already held", ok);

        // --- merge(): a higher incarnation for a DIFFERENT node is accepted (first we hear of it). ---
        CapabilityDigestEntry fresh{NodeId{2}, 1, 0, detail::encode_node_capabilities(NodeCapabilities{Flag{"disk"}})};
        reg.merge({fresh});
        check(reg.view(empty_base).capabilities_of(NodeId{2}).has_flag("disk"),
              "a brand-new node's entry is accepted on first sight", ok);

        // --- merge(): same-incarnation, higher local_seq wins (the tiebreak the prover's fix added). ---
        CapabilityDigestEntry same_inc_newer{NodeId{2}, 1, /*local_seq=*/1,
                                             detail::encode_node_capabilities(NodeCapabilities{Flag{"disk2"}})};
        reg.merge({same_inc_newer});
        check(reg.view(empty_base).capabilities_of(NodeId{2}).has_flag("disk2") &&
                  !reg.view(empty_base).capabilities_of(NodeId{2}).has_flag("disk"),
              "same incarnation, higher local_seq supersedes (last-store-wins within an incarnation)", ok);
        CapabilityDigestEntry same_inc_older{NodeId{2}, 1, /*local_seq=*/0,
                                             detail::encode_node_capabilities(NodeCapabilities{Flag{"disk3"}})};
        reg.merge({same_inc_older});
        check(reg.view(empty_base).capabilities_of(NodeId{2}).has_flag("disk2"),
              "same incarnation, LOWER local_seq never overrides a higher one already held", ok);

        // --- evict_dead: removes the entry; view() reverts to empty for that node. ---
        reg.evict_dead(NodeId{2});
        check(!reg.view(empty_base).capabilities_of(NodeId{2}).has_flag("disk2"),
              "evict_dead removes the node's entry; view() reverts to the empty default", ok);
        check(reg.view(empty_base).capabilities_of(NodeId{1}).has_flag("gpu"),
              "evicting one node leaves an unrelated node's entry untouched", ok);
    }

    // ============================================================================================
    // Part 2 — pull() budget packing: an entry that doesn't fit the remaining budget is SKIPPED
    // (`continue`), never starves a smaller entry that DOES fit (the bug ADR-045's prover caught —
    // a `break` here would non-deterministically lose the fitting entry depending on hash-map
    // iteration order; `continue` guarantees it survives regardless of order).
    // ============================================================================================
    {
        CapabilityRegistry reg;
        MembershipView empty_base;
        // A big blob (many flags) that alone exceeds a small budget, and a tiny one that fits.
        NodeCapabilities big;
        for (int i = 0; i < 200; ++i) big.add(Flag{"flag_" + std::to_string(i)});
        reg.merge({CapabilityDigestEntry{NodeId{10}, 1, 0, detail::encode_node_capabilities(big)}});
        reg.merge({CapabilityDigestEntry{
            NodeId{11}, 1, 0, detail::encode_node_capabilities(NodeCapabilities{Flag{"x"}})}});

        const std::uint64_t small_budget = 64;  // far smaller than node 10's blob, big enough for node 11's
        const auto packed = reg.pull(NodeId{99}, 1, small_budget);
        bool has_small = false, has_big = false;
        std::uint64_t total = 0;
        for (const auto& e : packed) {
            if (e.node == NodeId{11}) has_small = true;
            if (e.node == NodeId{10}) has_big = true;
            total += e.blob.size();
        }
        check(has_small, "the fitting small entry is packed regardless of iteration order (continue, not break)", ok);
        check(!has_big, "the oversized entry that would blow the budget is skipped, not truncated", ok);
        check(total <= small_budget, "packed total never exceeds the requested byte budget", ok);
    }

    // ============================================================================================
    // Part 3 — SwimMembership::set_capability_gossip: a LOCAL publish converges to a peer purely via
    // the existing SWIM gossip channel (021 §3), same convergence style as membership/revocations.
    // ============================================================================================
    {
        LoopbackFabric fabric;
        VClock clk;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.ack_timeout_ns = 100'000'000;
        cfg.suspicion_timeout_ns = 500'000'000;
        cfg.gossip_fanout = 3;
        cfg.seed = 0x51D3;

        LoopbackTransport t1(fabric, NodeId{1}), t2(fabric, NodeId{2});
        SwimMembership swim1(NodeId{1}, t1, cfg), swim2(NodeId{2}, t2, cfg);
        swim1.set_clock(&vclock_read, &clk);
        swim2.set_clock(&vclock_read, &clk);
        (void)swim1.admit(NodeId{2}, cluster);
        (void)swim2.admit(NodeId{1}, cluster);

        CapabilityRegistry reg1, reg2;
        swim1.set_capability_gossip(
            [&reg1](NodeId self, std::uint64_t inc, std::uint64_t max) { return reg1.pull(self, inc, max); },
            [&reg1](const std::vector<CapabilityDigestEntry>& in) { reg1.merge(in); });
        swim2.set_capability_gossip(
            [&reg2](NodeId self, std::uint64_t inc, std::uint64_t max) { return reg2.pull(self, inc, max); },
            [&reg2](const std::vector<CapabilityDigestEntry>& in) { reg2.merge(in); });

        check(!reg2.view(swim2.view()).capabilities_of(NodeId{1}).has_flag("gpu"),
              "node 2 has not heard of node 1's capabilities yet", ok);
        reg1.publish_local(NodeCapabilities{Flag{"voice-relay"}, Scalar{"weight", 3.0}});  // origin: node 1 only

        bool converged = false;
        for (int round = 0; round < 50 && !converged; ++round) {
            clk.now += 120'000'000;
            swim1.tick();
            swim2.tick();
            converged = reg2.view(swim2.view()).capabilities_of(NodeId{1}).has_flag("voice-relay");
        }
        check(converged, "a locally-published capability reaches the peer via the existing gossip channel", ok);
        check(reg2.view(swim2.view()).capabilities_of(NodeId{1}).scalar("weight") == 3.0,
              "the full capability set (not just the flag) converges", ok);
        check(reg1.view(swim1.view()).capabilities_of(NodeId{1}).has_flag("voice-relay"),
              "the originating node still has its own published capability", ok);

        // A capability-constrained CapabilityView built the same way ADR-030's VoiceChannel does
        // (build_voice_route_snapshot) now sees node 1 as eligible from node 2's own local, converged state.
        const CapabilityView v2 = reg2.view(swim2.view());
        int eligible = 0;
        for (NodeId n : v2.nodes())
            if (v2.capabilities_of(n).has_flag("voice-relay")) ++eligible;
        check(eligible == 1, "capability-filtered eligibility over the converged, network-backed view is correct", ok);
    }

    // ============================================================================================
    // Part 4 — concurrency stress: `publish_local()`/`view()` (documented safe from ANY thread) raced
    // against the ONLY thread allowed to call `pull()`/`merge()` (the protocol-thread-owned contract,
    // capability_registry.hpp's file banner). This is exactly ADR-045's S1/S2 claim scope — the
    // residual risk its own decision record flags as the most important thing to verify under REAL
    // TSan rather than a substitute stress harness, which this test now is (VERIFICATION.md runs this
    // file under gcc/clang Release, ASan+UBSan, and ThreadSanitizer).
    // ============================================================================================
    {
        CapabilityRegistry reg;
        constexpr int kIters = 20000;
        std::atomic<int> well_formed{0};

        std::thread app_thread([&] {
            for (int i = 0; i < kIters; ++i) {
                reg.publish_local(NodeCapabilities{Scalar{"weight", static_cast<double>(i)}});
                MembershipView base;
                const NodeCapabilities self_caps = reg.view(base).capabilities_of(NodeId{1});
                const NodeCapabilities peer_caps = reg.view(base).capabilities_of(NodeId{2});
                // publish_local()/pull() race by design, so we assert WELL-FORMEDNESS, not a specific
                // interleaving: a torn/corrupt read would surface as a value outside the published
                // range, or a crash under ASan/TSan.
                const double w = self_caps.scalar("weight", -1.0);
                const bool self_ok = (w == -1.0) || (w >= 0.0 && w < static_cast<double>(kIters));
                const bool peer_ok = !peer_caps.has_flag("peer") || peer_caps.label("from") == "protocol";
                if (self_ok && peer_ok) well_formed.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::thread protocol_thread([&] {
            for (int i = 0; i < kIters; ++i) {
                (void)reg.pull(NodeId{1}, /*self_incarnation=*/1, /*max_bytes=*/4096);
                reg.merge({CapabilityDigestEntry{
                    NodeId{2}, /*incarnation=*/1, static_cast<std::uint32_t>(i),
                    detail::encode_node_capabilities(
                        NodeCapabilities{Flag{"peer"}, Label{"from", "protocol"}})}});
            }
        });

        app_thread.join();
        protocol_thread.join();
        check(well_formed.load() == kIters,
              "every concurrent publish_local()/view() read stayed well-formed under a racing pull()/merge()",
              ok);
    }

    std::printf("capability_gossip_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
