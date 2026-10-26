// Tests REVIEW-BACKLOG C5/C6 - the gossip-fed maps are bounded and subject-checked:
//   * SwimMembership accepts a piggybacked pressure / capability entry only for the authenticated
//     sender or a verified live member (never self, never a fabricated id), so `last_pressure_` and the
//     CapabilityRegistry cannot be grown by a peer-chosen node-id stream (cluster.hpp).
//   * CapabilityRegistry / RevocationRegistry cap their tables, refuse + count overflow, dedupe, and a
//     node's own fingerprint can be pinned against a gossiped revocation (020: no issuer signature
//     exists, so entitlement beyond that is not enforceable here - see revocation_registry.hpp).
//   * PeerCongestionTable never holds more than `max_gates` gates and never sweeps a congested one.
#include <cstdio>
#include <vector>

#include "quark/core/capability_registry.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/governance.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/revocation_registry.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

[[nodiscard]] Fingerprint fp_n(std::uint32_t n) {
    Fingerprint fp{};
    for (int b = 0; b < 4; ++b) fp[static_cast<std::size_t>(b)] = static_cast<std::byte>((n >> (b * 8)) & 0xFF);
    return fp;
}

[[nodiscard]] std::vector<std::byte> blob_gpu() {
    return detail::encode_node_capabilities(NodeCapabilities{Flag{"gpu"}});
}
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{4242};

    // ---- Part 1: SWIM subject check (pressure + capabilities) ---------------------------------
    {
        LoopbackFabric fab;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.control_rate_per_sec = 0;  // not under test here
        LoopbackTransport t1{fab, NodeId{1}}, t2{fab, NodeId{2}};
        SwimMembership swim{NodeId{1}, t1, cfg};
        (void)swim.admit(NodeId{2}, cluster);
        CapabilityRegistry reg;
        swim.set_capability_gossip(
            [&reg](NodeId s, std::uint64_t i, std::uint64_t m) { return reg.pull(s, i, m); },
            [&reg](const std::vector<CapabilityDigestEntry>& in) { reg.merge(in); });

        ControlMsg m;
        m.kind = ControlKind::Gossip;
        m.cluster = cluster;
        m.from = NodeId{2};
        m.from_incarnation = 1;
        for (std::uint64_t id = 1000; id < 1100; ++id) {  // 100 fabricated subjects
            m.pressure.push_back(MailboxPressureEntry{NodeId{id}, 1, 777});
            m.capabilities.push_back(CapabilityDigestEntry{NodeId{id}, 1, 1, blob_gpu()});
        }
        m.pressure.push_back(MailboxPressureEntry{NodeId{1}, 1, 555});  // about the receiver itself
        m.capabilities.push_back(CapabilityDigestEntry{NodeId{1}, 99, 99, blob_gpu()});
        m.pressure.push_back(MailboxPressureEntry{NodeId{2}, 1, 42});  // the sender: legitimate
        m.capabilities.push_back(CapabilityDigestEntry{NodeId{2}, 1, 1, blob_gpu()});

        MessageFrame f;
        f.from = NodeId{2};
        f.to = NodeId{1};
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(m);
        t2.send(NodeId{1}, std::move(f));

        check(swim.pressure_of(NodeId{2}) == 42, "sender's own pressure entry is recorded", ok);
        check(swim.pressure_of(NodeId{1000}) == 0, "pressure for a fabricated id is refused", ok);
        check(swim.pressure_of(NodeId{1}) == 0, "pressure about self is refused", ok);
        check(swim.pressure_entries() == 1, "last_pressure_ holds only the legitimate subject", ok);
        const MembershipView base = swim.view();
        check(reg.view(base).capabilities_of(NodeId{2}).has_flag("gpu"), "sender's own capabilities accepted", ok);
        check(!reg.view(base).capabilities_of(NodeId{1050}).has_flag("gpu"),
              "capabilities for a fabricated id are refused", ok);
        check(!reg.view(base).capabilities_of(NodeId{1}).has_flag("gpu"),
              "a peer cannot write our own capability entry", ok);
        check(reg.size() == 1, "capability table holds only the legitimate subject", ok);
        check(swim.gossip_rejected() == 2 * 101, "every refused entry is counted", ok);
    }

    // ---- Part 2: CapabilityRegistry cap -------------------------------------------------------
    {
        CapabilityRegistry reg(8);
        std::vector<CapabilityDigestEntry> in;
        for (std::uint64_t id = 1; id <= 100; ++id) in.push_back(CapabilityDigestEntry{NodeId{id}, 1, 1, blob_gpu()});
        reg.merge(in);
        check(reg.size() == 8, "table never exceeds max_entries", ok);
        check(reg.rejected() == 92, "overflow entries are counted", ok);
        // An existing node still updates in place when full.
        reg.merge({CapabilityDigestEntry{NodeId{1}, 2, 1,
                                         detail::encode_node_capabilities(NodeCapabilities{Flag{"ssd"}})}});
        check(reg.view(MembershipView{}).capabilities_of(NodeId{1}).has_flag("ssd"),
              "a full table still updates an existing node", ok);
        // A blob that does not decode is refused, not stored.
        CapabilityRegistry reg2(8);
        reg2.merge({CapabilityDigestEntry{NodeId{5}, 1, 1, std::vector<std::byte>{std::byte{0xFF}, std::byte{0xFF}}}});
        check(reg2.size() == 0 && reg2.rejected() == 1, "undecodable blob is refused", ok);
    }

    // ---- Part 3: RevocationRegistry cap / dedupe / protect -------------------------------------
    {
        RevocationRegistry reg(16);
        std::vector<Fingerprint> in;
        for (std::uint32_t i = 0; i < 100; ++i) in.push_back(fp_n(i));
        check(reg.merge(in), "first merge adds", ok);
        check(reg.size() == 16, "gossiped revocations are capped", ok);
        check(reg.rejected() == 84, "refused revocations are counted", ok);
        const auto snap = reg.snapshot();
        check(!reg.merge({fp_n(0)}), "re-merging an existing fingerprint is not a change", ok);
        check(reg.snapshot() == snap, "a no-change merge does not republish the snapshot", ok);
        reg.revoke_locally(fp_n(5000));
        check(reg.is_revoked(fp_n(5000)), "a local revocation is never refused, even when full", ok);

        RevocationRegistry own(16);
        own.protect(fp_n(7));
        (void)own.merge({fp_n(7), fp_n(8)});
        check(!own.is_revoked(fp_n(7)), "a gossiped revocation of our own fingerprint is dropped", ok);
        check(own.is_revoked(fp_n(8)), "other fingerprints still merge", ok);
    }

    // ---- Part 4: PeerCongestionTable bound -----------------------------------------------------
    {
        PeerCongestionTable t(/*capacity=*/0.0, /*refill=*/0.0, /*max_gates=*/64);
        t.mark_congested(NodeId{7}, /*until_ns=*/1'000'000'000'000);
        for (std::uint64_t id = 100; id < 5100; ++id) (void)t.admit(NodeId{id}, /*now_ns=*/10);
        check(t.size() <= 64, "gate table never exceeds max_gates", ok);
        check(!t.admit(NodeId{7}, 10), "a still-congested gate survives the sweep", ok);
    }

    std::puts(ok ? "cluster_gossip_bounds_test: PASS" : "cluster_gossip_bounds_test: FAIL");
    return ok ? 0 : 1;
}
